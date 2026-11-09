// The real accelerators (Phase 5, design.md 6.11): Direct3D 12, DXCore and
// DirectML, all part of Windows. The only file that includes their headers.
#include "nova/accel.h"
#include "nova/accelop.h"
#include "nova/pool.h" // clock_seconds
#include "nova/soup.h" // Lane (Phase 8)

#include "warp_layout.inc" // the warp image's layout (Phase 8)

#include <windows.h>
// initguid.h first: it makes dxcore_interface.h define its attribute GUIDs here.
#include <initguid.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectML.h>
#include <dxcore.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nova {

namespace {

template <class T> void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void say(TextBuf* r, const char* what) {
    if (r)
        r->append(what);
}

void say_hr(TextBuf* r, const char* what, HRESULT hr) {
    if (!r)
        return;
    char b[160];
    snprintf(b, sizeof b, "%s (hr 0x%08lX)", what, static_cast<unsigned long>(hr));
    r->append(b);
}

// --- discovery -------------------------------------------------------------

struct Found {
    IDXCoreAdapter* adapter;
    AccelDevice d;
};

// Every adapter that supports Direct3D 12 core compute or generic ML, once
// each (by LUID), GPUs first by dedicated memory, then the rest.
u32 enumerate(Found* out, u32 max) {
    IDXCoreAdapterFactory* factory = nullptr;
    if (FAILED(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory))))
        return 0;
    u32 n = 0;
    LUID seen[64];
    u32 nseen = 0;
    const GUID kinds[2] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE, DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML};
    for (const GUID& kind : kinds) {
        IDXCoreAdapterList* list = nullptr;
        if (FAILED(factory->CreateAdapterList(1, &kind, IID_PPV_ARGS(&list))))
            continue;
        const u32 count = list->GetAdapterCount();
        for (u32 i = 0; i < count && n < max; ++i) {
            IDXCoreAdapter* a = nullptr;
            if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&a))))
                continue;
            LUID luid = {};
            a->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof luid, &luid);
            bool dup = false;
            for (u32 s = 0; s < nseen; ++s)
                dup = dup || (seen[s].LowPart == luid.LowPart && seen[s].HighPart == luid.HighPart);
            if (dup || nseen == 64) {
                a->Release();
                continue;
            }
            seen[nseen++] = luid;
            Found& f = out[n++];
            f.adapter = a;
            memset(&f.d, 0, sizeof f.d);
            size_t size = 0;
            if (SUCCEEDED(a->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &size)) && size > 0) {
                char buf[512] = {};
                a->GetProperty(DXCoreAdapterProperty::DriverDescription, size < sizeof buf ? size : sizeof buf - 1, buf);
                strncpy(f.d.name, buf, sizeof f.d.name - 1);
            }
            u64 mem = 0;
            a->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, sizeof mem, &mem);
            f.d.memory = mem;
            bool hw = false, integrated = false;
            a->GetProperty(DXCoreAdapterProperty::IsHardware, sizeof hw, &hw);
            a->GetProperty(DXCoreAdapterProperty::IsIntegrated, sizeof integrated, &integrated);
            f.d.hardware = hw ? 1 : 0;
            f.d.integrated = integrated ? 1 : 0;
            f.d.gpu = a->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS) ? 1 : 0;
            f.d.npu = f.d.gpu ? 0 : 1;
        }
        list->Release();
    }
    factory->Release();
    // GPUs first, hardware first, most dedicated memory first (insertion sort).
    auto before = [](const AccelDevice& x, const AccelDevice& y) {
        if (x.gpu != y.gpu)
            return x.gpu > y.gpu;
        if (x.hardware != y.hardware)
            return x.hardware > y.hardware;
        return x.memory > y.memory;
    };
    for (u32 i = 1; i < n; ++i) {
        Found t = out[i];
        u32 j = i;
        while (j > 0 && before(t.d, out[j - 1].d)) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = t;
    }
    for (u32 i = 0; i < n; ++i)
        out[i].d.index = i;
    return n;
}

void release_all(Found* f, u32 n) {
    for (u32 i = 0; i < n; ++i)
        release(f[i].adapter);
}

// --- a device with one compute queue -----------------------------------------

// NOVA_D3D_DEBUG=1 turns on the Direct3D 12 and DirectML debug layers (for
// diagnosing a device; slow) and prints their messages when a list fails.
bool debug_layers() {
    const char* v = getenv("NOVA_D3D_DEBUG");
    return v && v[0] == '1';
}

void dump_messages(ID3D12Device* dev) {
    if (!debug_layers() || !dev)
        return;
    ID3D12InfoQueue* iq = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&iq))))
        return;
    const UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        iq->GetMessage(i, nullptr, &len);
        D3D12_MESSAGE* msg = static_cast<D3D12_MESSAGE*>(malloc(len));
        if (msg && SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            fprintf(stderr, "d3d12: %.*s\n", static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
        free(msg);
    }
    iq->ClearStoredMessages();
    iq->Release();
}

struct Queue {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    u64 value = 0;

    bool init(IDXCoreAdapter* a, bool gpu, TextBuf* r) {
        HRESULT hr = E_FAIL;
        if (debug_layers()) {
            ID3D12Debug* dbg = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
                dbg->EnableDebugLayer();
                dbg->Release();
            }
        }
        if (gpu) {
            hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        } else {
            hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_1_0_CORE, IID_PPV_ARGS(&dev));
            if (FAILED(hr))
                hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_1_0_GENERIC, IID_PPV_ARGS(&dev));
        }
        if (FAILED(hr)) {
            say_hr(r, "no Direct3D 12 device", hr);
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc))) ||
            FAILED(hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc, nullptr,
                                               IID_PPV_ARGS(&list))) ||
            FAILED(hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            say_hr(r, "queue setup failed", hr);
            return false;
        }
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }

    // Another compute queue (with its own list and fence) on an existing
    // device, so the two can run at once and share resources.
    bool init_on(ID3D12Device* d, TextBuf* r) {
        dev = d;
        dev->AddRef();
        HRESULT hr = E_FAIL;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc))) ||
            FAILED(hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc, nullptr,
                                               IID_PPV_ARGS(&list))) ||
            FAILED(hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            say_hr(r, "second queue setup failed", hr);
            return false;
        }
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }

    // Makes what this queue runs next wait (on the GPU) for what queue o has
    // started so far.
    void after(const Queue& o) { queue->Wait(o.fence, o.value); }

    // Closes the list and starts it (Phase 8: the CPU works while it runs).
    bool exec(TextBuf* r) {
        HRESULT hr = list->Close();
        if (FAILED(hr)) {
            say_hr(r, "command list failed to close", hr);
            return false;
        }
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++value);
        return true;
    }
    // Waits for what exec() started, and opens the list again.
    bool wait(TextBuf* r) {
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, INFINITE);
        }
        HRESULT hr = dev->GetDeviceRemovedReason();
        if (FAILED(hr)) {
            say_hr(r, "device removed", hr);
            return false;
        }
        alloc->Reset();
        list->Reset(alloc, nullptr);
        return true;
    }

    // Closes the list, runs it, waits for it, and opens it again.
    bool run(TextBuf* r) {
        HRESULT hr = list->Close();
        if (FAILED(hr)) {
            say_hr(r, "command list failed to close", hr);
            return false;
        }
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++value);
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, INFINITE);
        }
        hr = dev->GetDeviceRemovedReason();
        if (FAILED(hr)) {
            say_hr(r, "device removed", hr);
            return false;
        }
        alloc->Reset();
        list->Reset(alloc, nullptr);
        return true;
    }

    ID3D12Resource* buffer(u64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = heap;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size < 256 ? 256 : size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* res = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res))))
            return nullptr;
        return res;
    }

    void barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }

    void uav_barrier(ID3D12Resource* res) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        list->ResourceBarrier(1, &b);
    }

    void destroy() {
        release(list);
        release(alloc);
        release(queue);
        release(fence);
        release(dev);
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
    }
};

// Copies `size` bytes from p into a fresh upload buffer.
ID3D12Resource* upload(Queue& q, const void* p, u64 size) {
    ID3D12Resource* res = q.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    if (!res)
        return nullptr;
    void* m = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(res->Map(0, &none, &m))) {
        res->Release();
        return nullptr;
    }
    memcpy(m, p, size);
    res->Unmap(0, nullptr);
    return res;
}

bool read_back(ID3D12Resource* rb, void* out, u64 size) {
    void* m = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(size)};
    if (FAILED(rb->Map(0, &all, &m)))
        return false;
    memcpy(out, m, size);
    D3D12_RANGE none = {0, 0};
    rb->Unmap(0, &none);
    return true;
}

// --- float16 for the probe (small integers only: exact) ----------------------

u16 half_of(float f) {
    u32 x;
    memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 exp = static_cast<i32>((x >> 23) & 0xFF) - 127 + 15;
    const u32 mant = x & 0x7FFFFFu;
    if ((x & 0x7FFFFFFFu) == 0)
        return static_cast<u16>(sign);
    if (exp >= 31)
        return static_cast<u16>(sign | 0x7C00u); // too big: infinity
    if (exp <= 0)
        return static_cast<u16>(sign); // too small for the probe's values
    return static_cast<u16>(sign | (static_cast<u32>(exp) << 10) | (mant >> 13));
}

float float_of(u16 h) {
    const u32 sign = (h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    u32 x;
    if (exp == 0)
        x = sign; // the probe never produces subnormals
    else if (exp == 31)
        x = sign | 0x7F800000u | (mant << 13);
    else
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

// --- the DirectML matrix multiply probe --------------------------------------

u64 tensor_bytes(u32 elements, u32 elem_size) { return (static_cast<u64>(elements) * elem_size + 3) & ~3ull; }

void buffer_tensor(DML_BUFFER_TENSOR_DESC* t, DML_TENSOR_DATA_TYPE type, const UINT* sizes, u32 elem_size) {
    memset(t, 0, sizeof *t);
    t->DataType = type;
    t->Flags = DML_TENSOR_FLAG_NONE;
    t->DimensionCount = 4;
    t->Sizes = sizes;
    t->Strides = nullptr;
    t->TotalTensorSizeInBytes = tensor_bytes(sizes[0] * sizes[1] * sizes[2] * sizes[3], elem_size);
}

// One known-answer multiply: A (M x K) times B (K x N), values -32..31 with
// a row and a column at -32 so one sum reaches K x 1024 (the largest an NPU op
// can make). Returns true if it ran and every element is exactly right.
// `low`: K = 32 and values 0..7, so every sum stays below 2048, where
// float16 is still exact (the digit form an NPU might need). `graph`: compile
// as a one-node graph (how NPUs usually take work), with half precision
// allowed.
bool dml_multiply(Queue& q, IDMLDevice* dml, u32 form, TextBuf* r, bool low = false, bool graph = false,
                  bool own_b = false) {
    const u32 M = 16, K = low ? 32 : 256, N = 16;
    static i32 a[16 * 256], b[256 * 16];
    u32 seed = 12345;
    auto next = [&seed, low]() {
        seed = seed * 1664525u + 1013904223u;
        return low ? static_cast<i32>((seed >> 16) % 8) : static_cast<i32>((seed >> 16) % 64) - 32;
    };
    for (u32 i = 0; i < M * K; ++i)
        a[i] = next();
    for (u32 i = 0; i < K * N; ++i)
        b[i] = next();
    for (u32 i = 0; i < K; ++i) {
        a[i] = low ? 7 : -32;         // row 0 of A
        b[i * N + 0] = low ? 7 : -32; // column 0 of B
    }
    i64 expect[M * N];
    for (u32 m = 0; m < M; ++m)
        for (u32 n = 0; n < N; ++n) {
            i64 acc = 0;
            for (u32 k = 0; k < K; ++k)
                acc += static_cast<i64>(a[m * K + k]) * b[k * N + n];
            expect[m * N + n] = acc;
        }

    DML_TENSOR_DATA_TYPE in_type, out_type;
    u32 in_size, out_size;
    const char* label;
    if (form == kProbeInt8) {
        in_type = DML_TENSOR_DATA_TYPE_INT8, out_type = DML_TENSOR_DATA_TYPE_INT32;
        in_size = 1, out_size = 4, label = "int8 -> int32";
    } else if (form == kProbeFloat32) {
        in_type = out_type = DML_TENSOR_DATA_TYPE_FLOAT32;
        in_size = out_size = 4, label = "float32";
    } else {
        in_type = out_type = DML_TENSOR_DATA_TYPE_FLOAT16;
        in_size = out_size = 2, label = "float16";
    }
    say(r, "    ");
    say(r, label);
    say(r, low ? " (sums < 2048)" : "");
    say(r, graph ? " as a graph" : "");
    say(r, own_b ? " with B as weights" : "");
    say(r, ": ");

    const UINT as[4] = {1, 1, M, K}, bs[4] = {1, 1, K, N}, os[4] = {1, 1, M, N};
    DML_BUFFER_TENSOR_DESC ta, tb, to;
    buffer_tensor(&ta, in_type, as, in_size);
    buffer_tensor(&tb, in_type, bs, in_size);
    buffer_tensor(&to, out_type, os, out_size);
    if (own_b) // B handed over at initialization, as fixed weights
        tb.Flags = DML_TENSOR_FLAG_OWNED_BY_DML;
    DML_TENSOR_DESC da = {DML_TENSOR_TYPE_BUFFER, &ta}, db = {DML_TENSOR_TYPE_BUFFER, &tb},
                    dout = {DML_TENSOR_TYPE_BUFFER, &to};
    DML_MATRIX_MULTIPLY_INTEGER_OPERATOR_DESC mmi = {&da, nullptr, &db, nullptr, &dout};
    DML_GEMM_OPERATOR_DESC gemm = {};
    gemm.ATensor = &da;
    gemm.BTensor = &db;
    gemm.CTensor = nullptr;
    gemm.OutputTensor = &dout;
    gemm.TransA = DML_MATRIX_TRANSFORM_NONE;
    gemm.TransB = DML_MATRIX_TRANSFORM_NONE;
    gemm.Alpha = 1.0f;
    gemm.Beta = 0.0f;
    DML_OPERATOR_DESC od = {};
    if (form == kProbeInt8)
        od = {DML_OPERATOR_MATRIX_MULTIPLY_INTEGER, &mmi};
    else
        od = {DML_OPERATOR_GEMM, &gemm};

    IDMLOperator* op = nullptr;
    IDMLCompiledOperator* compiled = nullptr;
    IDMLOperatorInitializer* init = nullptr;
    IDMLBindingTable* table = nullptr;
    IDMLCommandRecorder* rec = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource *ra = nullptr, *rb = nullptr, *rout = nullptr, *up = nullptr, *readback = nullptr;
    ID3D12Resource *temp = nullptr, *persist = nullptr;
    bool ok = false;
    HRESULT hr;
    do {
        if (FAILED(hr = dml->CreateOperator(&od, IID_PPV_ARGS(&op)))) {
            say_hr(r, "operator refused", hr);
            break;
        }
        if (graph) {
            IDMLDevice1* dml1 = nullptr;
            if (FAILED(hr = dml->QueryInterface(IID_PPV_ARGS(&dml1)))) {
                say_hr(r, "no graph compiler", hr);
                break;
            }
            // Graph inputs 0 (A) and 1 (B) feed the node's inputs 0 and the
            // B slot (2 for the integer multiply, 1 for GEMM); its output 0 is
            // the graph's output.
            DML_OPERATOR_GRAPH_NODE_DESC node = {op, "mm"};
            DML_GRAPH_NODE_DESC nodes[1] = {{DML_GRAPH_NODE_TYPE_OPERATOR, &node}};
            DML_INPUT_GRAPH_EDGE_DESC in0 = {0, 0, 0, "a"};
            DML_INPUT_GRAPH_EDGE_DESC in1 = {1, 0, form == kProbeInt8 ? 2u : 1u, "b"};
            DML_OUTPUT_GRAPH_EDGE_DESC out0 = {0, 0, 0, "out"};
            DML_GRAPH_EDGE_DESC ins[2] = {{DML_GRAPH_EDGE_TYPE_INPUT, &in0}, {DML_GRAPH_EDGE_TYPE_INPUT, &in1}};
            DML_GRAPH_EDGE_DESC outs[1] = {{DML_GRAPH_EDGE_TYPE_OUTPUT, &out0}};
            DML_GRAPH_DESC gd = {};
            gd.InputCount = 2;
            gd.OutputCount = 1;
            gd.NodeCount = 1;
            gd.Nodes = nodes;
            gd.InputEdgeCount = 2;
            gd.InputEdges = ins;
            gd.OutputEdgeCount = 1;
            gd.OutputEdges = outs;
            hr = dml1->CompileGraph(&gd, DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION, IID_PPV_ARGS(&compiled));
            dml1->Release();
            if (FAILED(hr)) {
                say_hr(r, "not supported (graph compile failed)", hr);
                break;
            }
        } else if (FAILED(hr = dml->CompileOperator(op, DML_EXECUTION_FLAG_NONE, IID_PPV_ARGS(&compiled)))) {
            say_hr(r, "not supported (compile failed)", hr);
            break;
        }
        IDMLCompiledOperator* ops[] = {compiled};
        if (FAILED(hr = dml->CreateOperatorInitializer(1, ops, IID_PPV_ARGS(&init)))) {
            say_hr(r, "initializer failed", hr);
            break;
        }
        const DML_BINDING_PROPERTIES ip = init->GetBindingProperties();
        const DML_BINDING_PROPERTIES ep = compiled->GetBindingProperties();
        const UINT descriptors = ip.RequiredDescriptorCount > ep.RequiredDescriptorCount ? ip.RequiredDescriptorCount
                                                                                        : ep.RequiredDescriptorCount;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = descriptors ? descriptors : 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(hr = q.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
            say_hr(r, "descriptor heap failed", hr);
            break;
        }
        DML_BINDING_TABLE_DESC btd = {init, heap->GetCPUDescriptorHandleForHeapStart(),
                                      heap->GetGPUDescriptorHandleForHeapStart(), descriptors};
        if (FAILED(hr = dml->CreateBindingTable(&btd, IID_PPV_ARGS(&table))) ||
            FAILED(hr = dml->CreateCommandRecorder(IID_PPV_ARGS(&rec)))) {
            say_hr(r, "binding setup failed", hr);
            break;
        }
        // Inputs, packed in the form's element type.
        const u64 a_bytes = ta.TotalTensorSizeInBytes, b_bytes = tb.TotalTensorSizeInBytes;
        const u64 o_bytes = to.TotalTensorSizeInBytes;
        static unsigned char host[4 * 16 * 256 + 4 * 256 * 16]; // the largest form
        memset(host, 0, sizeof host);
        unsigned char* ha = host;
        unsigned char* hb = host + a_bytes;
        for (u32 i = 0; i < M * K; ++i) {
            if (form == kProbeInt8)
                reinterpret_cast<signed char*>(ha)[i] = static_cast<signed char>(a[i]);
            else if (form == kProbeFloat32)
                reinterpret_cast<float*>(ha)[i] = static_cast<float>(a[i]);
            else
                reinterpret_cast<u16*>(ha)[i] = half_of(static_cast<float>(a[i]));
        }
        for (u32 i = 0; i < K * N; ++i) {
            if (form == kProbeInt8)
                reinterpret_cast<signed char*>(hb)[i] = static_cast<signed char>(b[i]);
            else if (form == kProbeFloat32)
                reinterpret_cast<float*>(hb)[i] = static_cast<float>(b[i]);
            else
                reinterpret_cast<u16*>(hb)[i] = half_of(static_cast<float>(b[i]));
        }
        up = upload(q, host, a_bytes + b_bytes);
        ra = q.buffer(a_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        rb = q.buffer(b_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        rout = q.buffer(o_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        readback = q.buffer(o_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        const u64 temp_size =
            ip.TemporaryResourceSize > ep.TemporaryResourceSize ? ip.TemporaryResourceSize : ep.TemporaryResourceSize;
        if (temp_size)
            temp = q.buffer(temp_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (ep.PersistentResourceSize)
            persist = q.buffer(ep.PersistentResourceSize, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (!up || !ra || !rb || !rout || !readback || (temp_size && !temp) ||
            (ep.PersistentResourceSize && !persist)) {
            say(r, "buffer allocation failed");
            break;
        }
        q.list->CopyBufferRegion(ra, 0, up, 0, a_bytes);
        q.list->CopyBufferRegion(rb, 0, up, a_bytes, b_bytes);
        q.barrier(ra, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        q.barrier(rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Initialize, then execute.
        ID3D12DescriptorHeap* heaps[] = {heap};
        q.list->SetDescriptorHeaps(1, heaps);
        DML_BUFFER_BINDING tb_bind = {temp, 0, temp_size};
        DML_BINDING_DESC temp_desc = {DML_BINDING_TYPE_BUFFER, &tb_bind};
        DML_BUFFER_BINDING pb_bind = {persist, 0, ep.PersistentResourceSize};
        DML_BINDING_DESC persist_desc = {DML_BINDING_TYPE_BUFFER, &pb_bind};
        if (ip.TemporaryResourceSize)
            table->BindTemporaryResource(&temp_desc);
        if (ep.PersistentResourceSize)
            table->BindOutputs(1, &persist_desc);
        DML_BUFFER_BINDING init_in[4] = {};
        DML_BUFFER_ARRAY_BINDING init_arr = {};
        DML_BINDING_DESC init_desc = {DML_BINDING_TYPE_BUFFER_ARRAY, &init_arr};
        if (own_b) { // the weights: B in its slot, nothing elsewhere
            const u32 slots = graph ? 2 : (form == kProbeInt8 ? 4 : 3);
            const u32 b_slot = graph ? 1 : (form == kProbeInt8 ? 2 : 1);
            init_in[b_slot] = {rb, 0, b_bytes};
            init_arr = {slots, init_in};
            table->BindInputs(1, &init_desc);
        }
        rec->RecordDispatch(q.list, init, table);
        q.uav_barrier(nullptr);
        if (!q.run(r)) // initialization first, as its own submission
            break;
        q.list->SetDescriptorHeaps(1, heaps);
        btd.Dispatchable = compiled;
        btd.SizeInDescriptors = descriptors;
        if (FAILED(hr = table->Reset(&btd))) {
            say_hr(r, "binding table reset failed", hr);
            break;
        }
        DML_BUFFER_BINDING ab = {ra, 0, a_bytes}, bb = {rb, 0, b_bytes}, ob = {rout, 0, o_bytes};
        DML_BINDING_DESC ad = {DML_BINDING_TYPE_BUFFER, &ab}, bd = {DML_BINDING_TYPE_BUFFER, &bb},
                         odsc = {DML_BINDING_TYPE_BUFFER, &ob}, none = {DML_BINDING_TYPE_NONE, nullptr};
        const DML_BINDING_DESC bslot = own_b ? none : bd;
        if (graph) { // a graph's inputs are only the edges it declares
            DML_BINDING_DESC ins[2] = {ad, bslot};
            table->BindInputs(2, ins);
        } else if (form == kProbeInt8) {
            DML_BINDING_DESC ins[4] = {ad, none, bslot, none};
            table->BindInputs(4, ins);
        } else {
            DML_BINDING_DESC ins[3] = {ad, bslot, none};
            table->BindInputs(3, ins);
        }
        table->BindOutputs(1, &odsc);
        if (ep.TemporaryResourceSize)
            table->BindTemporaryResource(&temp_desc);
        if (ep.PersistentResourceSize)
            table->BindPersistentResource(&persist_desc);
        rec->RecordDispatch(q.list, compiled, table);
        q.uav_barrier(nullptr);
        q.barrier(rout, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        q.list->CopyBufferRegion(readback, 0, rout, 0, o_bytes);
        if (!q.run(r))
            break;
        unsigned char got[4 * M * N] = {};
        if (!read_back(readback, got, o_bytes)) {
            say(r, "readback failed");
            break;
        }
        u32 wrong = 0;
        for (u32 i = 0; i < M * N; ++i) {
            double v;
            if (form == kProbeInt8)
                v = reinterpret_cast<i32*>(got)[i];
            else if (form == kProbeFloat32)
                v = reinterpret_cast<float*>(got)[i];
            else
                v = float_of(reinterpret_cast<u16*>(got)[i]);
            wrong += v != static_cast<double>(expect[i]);
        }
        char b2[96];
        if (wrong == 0) {
            snprintf(b2, sizeof b2, "ran, exact (%ux%ux%u, largest sum %lld)", M, K, N,
                     static_cast<long long>(expect[0]));
            ok = true;
        } else {
            snprintf(b2, sizeof b2, "ran, %u of %u results wrong", wrong, M * N);
            say(r, b2);
            // What came back, for diagnosis: the first results and the right ones.
            say(r, " -- got");
            for (u32 i = 0; i < 4; ++i) {
                double v = form == kProbeInt8 ? reinterpret_cast<i32*>(got)[i]
                                              : (form == kProbeFloat32 ? reinterpret_cast<float*>(got)[i]
                                                                       : float_of(reinterpret_cast<u16*>(got)[i]));
                snprintf(b2, sizeof b2, " %g", v);
                say(r, b2);
            }
            say(r, ", want");
            for (u32 i = 0; i < 4; ++i) {
                snprintf(b2, sizeof b2, " %lld", static_cast<long long>(expect[i]));
                say(r, b2);
            }
            b2[0] = 0;
        }
        say(r, b2);
    } while (false);
    say(r, "\n");
    release(op);
    release(compiled);
    release(init);
    release(table);
    release(rec);
    release(heap);
    release(ra);
    release(rb);
    release(rout);
    release(up);
    release(readback);
    release(temp);
    release(persist);
    return ok;
}

// Data movement on a device, no DirectML: upload -> device buffer -> readback.
bool copy_round_trip(Queue& q, TextBuf* r) {
    const u32 count = 1024;
    static u32 host[1024], back[1024];
    for (u32 i = 0; i < count; ++i)
        host[i] = i * 2654435761u;
    ID3D12Resource* up = upload(q, host, count * 4);
    ID3D12Resource* dev = q.buffer(count * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
    ID3D12Resource* rb = q.buffer(count * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    bool ok = false;
    say(r, "    copy round trip: ");
    if (up && dev && rb) {
        q.list->CopyBufferRegion(dev, 0, up, 0, count * 4);
        q.barrier(dev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        q.list->CopyBufferRegion(rb, 0, dev, 0, count * 4);
        if (q.run(r) && read_back(rb, back, count * 4)) {
            ok = memcmp(host, back, count * 4) == 0;
            say(r, ok ? "right" : "WRONG");
        }
    } else {
        say(r, "buffer allocation failed");
    }
    say(r, "\n");
    release(up);
    release(dev);
    release(rb);
    return ok;
}

// The simplest DirectML work, float16 output = input, as a graph: does data
// get through DirectML on this device at all?
bool dml_identity(Queue& q, IDMLDevice* dml, TextBuf* r) {
    const UINT sizes[4] = {1, 1, 16, 32};
    DML_BUFFER_TENSOR_DESC t;
    buffer_tensor(&t, DML_TENSOR_DATA_TYPE_FLOAT16, sizes, 2);
    DML_TENSOR_DESC td = {DML_TENSOR_TYPE_BUFFER, &t};
    DML_ELEMENT_WISE_IDENTITY_OPERATOR_DESC id = {&td, &td, nullptr};
    DML_OPERATOR_DESC od = {DML_OPERATOR_ELEMENT_WISE_IDENTITY, &id};
    IDMLOperator* op = nullptr;
    IDMLDevice1* dml1 = nullptr;
    IDMLCompiledOperator* compiled = nullptr;
    IDMLOperatorInitializer* init = nullptr;
    IDMLBindingTable* table = nullptr;
    IDMLCommandRecorder* rec = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource *up = nullptr, *in = nullptr, *out = nullptr, *rb = nullptr, *temp = nullptr, *persist = nullptr;
    bool ok = false;
    HRESULT hr;
    say(r, "    float16 identity as a graph: ");
    do {
        if (FAILED(hr = dml->CreateOperator(&od, IID_PPV_ARGS(&op))) ||
            FAILED(hr = dml->QueryInterface(IID_PPV_ARGS(&dml1)))) {
            say_hr(r, "refused", hr);
            break;
        }
        DML_OPERATOR_GRAPH_NODE_DESC node = {op, "id"};
        DML_GRAPH_NODE_DESC nodes[1] = {{DML_GRAPH_NODE_TYPE_OPERATOR, &node}};
        DML_INPUT_GRAPH_EDGE_DESC in0 = {0, 0, 0, "x"};
        DML_OUTPUT_GRAPH_EDGE_DESC out0 = {0, 0, 0, "y"};
        DML_GRAPH_EDGE_DESC ins[1] = {{DML_GRAPH_EDGE_TYPE_INPUT, &in0}};
        DML_GRAPH_EDGE_DESC outs[1] = {{DML_GRAPH_EDGE_TYPE_OUTPUT, &out0}};
        DML_GRAPH_DESC gd = {1, 1, 1, nodes, 1, ins, 1, outs, 0, nullptr};
        if (FAILED(hr = dml1->CompileGraph(&gd, DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
                                           IID_PPV_ARGS(&compiled)))) {
            say_hr(r, "graph compile failed", hr);
            break;
        }
        IDMLCompiledOperator* ops[] = {compiled};
        if (FAILED(hr = dml->CreateOperatorInitializer(1, ops, IID_PPV_ARGS(&init)))) {
            say_hr(r, "initializer failed", hr);
            break;
        }
        const DML_BINDING_PROPERTIES ip = init->GetBindingProperties(), ep = compiled->GetBindingProperties();
        const UINT descriptors = ip.RequiredDescriptorCount > ep.RequiredDescriptorCount ? ip.RequiredDescriptorCount
                                                                                        : ep.RequiredDescriptorCount;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptors ? descriptors : 1,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(hr = q.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
            say_hr(r, "descriptor heap failed", hr);
            break;
        }
        DML_BINDING_TABLE_DESC btd = {init, heap->GetCPUDescriptorHandleForHeapStart(),
                                      heap->GetGPUDescriptorHandleForHeapStart(), descriptors};
        if (FAILED(hr = dml->CreateBindingTable(&btd, IID_PPV_ARGS(&table))) ||
            FAILED(hr = dml->CreateCommandRecorder(IID_PPV_ARGS(&rec)))) {
            say_hr(r, "binding setup failed", hr);
            break;
        }
        static u16 host[512], back[512];
        for (u32 i = 0; i < 512; ++i)
            host[i] = half_of(static_cast<float>(i % 7));
        const u64 bytes = t.TotalTensorSizeInBytes;
        up = upload(q, host, bytes);
        in = q.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        out = q.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        rb = q.buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        const u64 temp_size =
            ip.TemporaryResourceSize > ep.TemporaryResourceSize ? ip.TemporaryResourceSize : ep.TemporaryResourceSize;
        if (temp_size)
            temp = q.buffer(temp_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (ep.PersistentResourceSize)
            persist = q.buffer(ep.PersistentResourceSize, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        q.list->CopyBufferRegion(in, 0, up, 0, bytes);
        q.barrier(in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[] = {heap};
        q.list->SetDescriptorHeaps(1, heaps);
        DML_BUFFER_BINDING tbb = {temp, 0, temp_size}, pbb = {persist, 0, ep.PersistentResourceSize};
        DML_BINDING_DESC tdsc = {DML_BINDING_TYPE_BUFFER, &tbb}, pdsc = {DML_BINDING_TYPE_BUFFER, &pbb};
        if (ip.TemporaryResourceSize)
            table->BindTemporaryResource(&tdsc);
        if (ep.PersistentResourceSize)
            table->BindOutputs(1, &pdsc);
        rec->RecordDispatch(q.list, init, table);
        q.uav_barrier(nullptr);
        btd.Dispatchable = compiled;
        table->Reset(&btd);
        DML_BUFFER_BINDING ib = {in, 0, bytes}, ob = {out, 0, bytes};
        DML_BINDING_DESC idsc = {DML_BINDING_TYPE_BUFFER, &ib}, odsc = {DML_BINDING_TYPE_BUFFER, &ob};
        table->BindInputs(1, &idsc);
        table->BindOutputs(1, &odsc);
        if (ep.TemporaryResourceSize)
            table->BindTemporaryResource(&tdsc);
        if (ep.PersistentResourceSize)
            table->BindPersistentResource(&pdsc);
        rec->RecordDispatch(q.list, compiled, table);
        q.uav_barrier(nullptr);
        q.barrier(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        q.list->CopyBufferRegion(rb, 0, out, 0, bytes);
        if (!q.run(r) || !read_back(rb, back, bytes))
            break;
        u32 wrong = 0;
        for (u32 i = 0; i < 512; ++i)
            wrong += back[i] != host[i];
        char b[96];
        snprintf(b, sizeof b, wrong ? "%u of 512 wrong (first %g, want %g)" : "right", wrong,
                 static_cast<double>(float_of(back[0])), static_cast<double>(float_of(host[0])));
        say(r, b);
        ok = wrong == 0;
    } while (false);
    say(r, "\n");
    release(op);
    release(dml1);
    release(compiled);
    release(init);
    release(table);
    release(rec);
    release(heap);
    release(up);
    release(in);
    release(out);
    release(rb);
    release(temp);
    release(persist);
    return ok;
}

// The small float16 multiply (A M x K, B K x N, values 0..7) as a 1x1
// convolution graph, the shape NPUs are built for: input [1, K, M, 1] (A
// transposed, channels = K), filter [N, K, 1, 1] (B transposed), output
// [1, N, M, 1] = (A B) transposed. `weights`: the filter as fixed weights.
bool dml_conv(Queue& q, IDMLDevice* dml, TextBuf* r, bool weights) {
    const u32 M = 16, K = 32, N = 16;
    i32 a[M * K], b[K * N];
    u32 seed = 777;
    for (u32 i = 0; i < M * K; ++i)
        a[i] = static_cast<i32>(((seed = seed * 1664525u + 1013904223u) >> 16) % 8);
    for (u32 i = 0; i < K * N; ++i)
        b[i] = static_cast<i32>(((seed = seed * 1664525u + 1013904223u) >> 16) % 8);
    static u16 hin[K * M], hfilt[N * K], hout[N * M];
    for (u32 m = 0; m < M; ++m)
        for (u32 k = 0; k < K; ++k)
            hin[k * M + m] = half_of(static_cast<float>(a[m * K + k]));
    for (u32 n = 0; n < N; ++n)
        for (u32 k = 0; k < K; ++k)
            hfilt[n * K + k] = half_of(static_cast<float>(b[k * N + n]));
    const UINT is[4] = {1, K, M, 1}, fs[4] = {N, K, 1, 1}, os[4] = {1, N, M, 1};
    DML_BUFFER_TENSOR_DESC ti, tf, to;
    buffer_tensor(&ti, DML_TENSOR_DATA_TYPE_FLOAT16, is, 2);
    buffer_tensor(&tf, DML_TENSOR_DATA_TYPE_FLOAT16, fs, 2);
    buffer_tensor(&to, DML_TENSOR_DATA_TYPE_FLOAT16, os, 2);
    if (weights)
        tf.Flags = DML_TENSOR_FLAG_OWNED_BY_DML;
    DML_TENSOR_DESC di = {DML_TENSOR_TYPE_BUFFER, &ti}, df = {DML_TENSOR_TYPE_BUFFER, &tf},
                    dout = {DML_TENSOR_TYPE_BUFFER, &to};
    const UINT one[2] = {1, 1}, zero[2] = {0, 0};
    DML_CONVOLUTION_OPERATOR_DESC cd = {};
    cd.InputTensor = &di;
    cd.FilterTensor = &df;
    cd.BiasTensor = nullptr;
    cd.OutputTensor = &dout;
    cd.Mode = DML_CONVOLUTION_MODE_CROSS_CORRELATION;
    cd.Direction = DML_CONVOLUTION_DIRECTION_FORWARD;
    cd.DimensionCount = 2;
    cd.Strides = one;
    cd.Dilations = one;
    cd.StartPadding = zero;
    cd.EndPadding = zero;
    cd.OutputPadding = zero;
    cd.GroupCount = 1;
    DML_OPERATOR_DESC od = {DML_OPERATOR_CONVOLUTION, &cd};
    IDMLOperator* op = nullptr;
    IDMLDevice1* dml1 = nullptr;
    IDMLCompiledOperator* compiled = nullptr;
    IDMLOperatorInitializer* init = nullptr;
    IDMLBindingTable* table = nullptr;
    IDMLCommandRecorder* rec = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource *up = nullptr, *rin = nullptr, *rf = nullptr, *rout = nullptr, *rb = nullptr;
    ID3D12Resource *temp = nullptr, *persist = nullptr;
    bool ok = false;
    HRESULT hr;
    say(r, weights ? "    float16 (sums < 2048) as a 1x1 convolution, filter as weights: "
                   : "    float16 (sums < 2048) as a 1x1 convolution: ");
    do {
        if (FAILED(hr = dml->CreateOperator(&od, IID_PPV_ARGS(&op))) ||
            FAILED(hr = dml->QueryInterface(IID_PPV_ARGS(&dml1)))) {
            say_hr(r, "refused", hr);
            break;
        }
        DML_OPERATOR_GRAPH_NODE_DESC node = {op, "conv"};
        DML_GRAPH_NODE_DESC nodes[1] = {{DML_GRAPH_NODE_TYPE_OPERATOR, &node}};
        DML_INPUT_GRAPH_EDGE_DESC in0 = {0, 0, 0, "x"}, in1 = {1, 0, 1, "w"};
        DML_OUTPUT_GRAPH_EDGE_DESC out0 = {0, 0, 0, "y"};
        DML_GRAPH_EDGE_DESC ins[2] = {{DML_GRAPH_EDGE_TYPE_INPUT, &in0}, {DML_GRAPH_EDGE_TYPE_INPUT, &in1}};
        DML_GRAPH_EDGE_DESC outs[1] = {{DML_GRAPH_EDGE_TYPE_OUTPUT, &out0}};
        DML_GRAPH_DESC gd = {2, 1, 1, nodes, 2, ins, 1, outs, 0, nullptr};
        if (FAILED(hr = dml1->CompileGraph(&gd, DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
                                           IID_PPV_ARGS(&compiled)))) {
            say_hr(r, "graph compile failed", hr);
            break;
        }
        IDMLCompiledOperator* ops[] = {compiled};
        if (FAILED(hr = dml->CreateOperatorInitializer(1, ops, IID_PPV_ARGS(&init)))) {
            say_hr(r, "initializer failed", hr);
            break;
        }
        const DML_BINDING_PROPERTIES ip = init->GetBindingProperties(), ep = compiled->GetBindingProperties();
        const UINT descriptors = ip.RequiredDescriptorCount > ep.RequiredDescriptorCount ? ip.RequiredDescriptorCount
                                                                                        : ep.RequiredDescriptorCount;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptors ? descriptors : 1,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(hr = q.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
            say_hr(r, "descriptor heap failed", hr);
            break;
        }
        DML_BINDING_TABLE_DESC btd = {init, heap->GetCPUDescriptorHandleForHeapStart(),
                                      heap->GetGPUDescriptorHandleForHeapStart(), descriptors};
        if (FAILED(hr = dml->CreateBindingTable(&btd, IID_PPV_ARGS(&table))) ||
            FAILED(hr = dml->CreateCommandRecorder(IID_PPV_ARGS(&rec)))) {
            say_hr(r, "binding setup failed", hr);
            break;
        }
        const u64 ib = ti.TotalTensorSizeInBytes, fb = tf.TotalTensorSizeInBytes, ob = to.TotalTensorSizeInBytes;
        static unsigned char host[2 * (K * M + N * K)];
        memcpy(host, hin, ib);
        memcpy(host + ib, hfilt, fb);
        up = upload(q, host, ib + fb);
        rin = q.buffer(ib, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        rf = q.buffer(fb, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        rout = q.buffer(ob, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        rb = q.buffer(ob, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        const u64 temp_size =
            ip.TemporaryResourceSize > ep.TemporaryResourceSize ? ip.TemporaryResourceSize : ep.TemporaryResourceSize;
        if (temp_size)
            temp = q.buffer(temp_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (ep.PersistentResourceSize)
            persist = q.buffer(ep.PersistentResourceSize, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        q.list->CopyBufferRegion(rin, 0, up, 0, ib);
        q.list->CopyBufferRegion(rf, 0, up, ib, fb);
        q.barrier(rin, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        q.barrier(rf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[] = {heap};
        q.list->SetDescriptorHeaps(1, heaps);
        DML_BUFFER_BINDING tbb = {temp, 0, temp_size}, pbb = {persist, 0, ep.PersistentResourceSize};
        DML_BINDING_DESC tdsc = {DML_BINDING_TYPE_BUFFER, &tbb}, pdsc = {DML_BINDING_TYPE_BUFFER, &pbb};
        if (ip.TemporaryResourceSize)
            table->BindTemporaryResource(&tdsc);
        if (ep.PersistentResourceSize)
            table->BindOutputs(1, &pdsc);
        DML_BUFFER_BINDING init_in[2] = {{nullptr, 0, 0}, {rf, 0, fb}};
        DML_BUFFER_ARRAY_BINDING arr = {2, init_in};
        DML_BINDING_DESC adsc = {DML_BINDING_TYPE_BUFFER_ARRAY, &arr};
        if (weights)
            table->BindInputs(1, &adsc);
        rec->RecordDispatch(q.list, init, table);
        q.uav_barrier(nullptr);
        // Initialization runs as its own submission: the weights must be in
        // the persistent resource before the execution reads them.
        if (!q.run(r))
            break;
        q.list->SetDescriptorHeaps(1, heaps);
        btd.Dispatchable = compiled;
        table->Reset(&btd);
        DML_BUFFER_BINDING bi = {rin, 0, ib}, bf = {rf, 0, fb}, bo = {rout, 0, ob};
        DML_BINDING_DESC di2 = {DML_BINDING_TYPE_BUFFER, &bi}, df2 = {DML_BINDING_TYPE_BUFFER, &bf},
                         do2 = {DML_BINDING_TYPE_BUFFER, &bo}, none = {DML_BINDING_TYPE_NONE, nullptr};
        DML_BINDING_DESC ins2[2] = {di2, weights ? none : df2};
        table->BindInputs(2, ins2);
        table->BindOutputs(1, &do2);
        if (ep.TemporaryResourceSize)
            table->BindTemporaryResource(&tdsc);
        if (ep.PersistentResourceSize)
            table->BindPersistentResource(&pdsc);
        rec->RecordDispatch(q.list, compiled, table);
        q.uav_barrier(nullptr);
        q.barrier(rout, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        q.list->CopyBufferRegion(rb, 0, rout, 0, ob);
        if (!q.run(r) || !read_back(rb, hout, ob))
            break;
        u32 wrong = 0;
        double first = 0, want = 0;
        for (u32 m = 0; m < M; ++m)
            for (u32 n = 0; n < N; ++n) {
                i64 acc = 0;
                for (u32 k = 0; k < K; ++k)
                    acc += static_cast<i64>(a[m * K + k]) * b[k * N + n];
                const double got = float_of(hout[n * M + m]);
                if (m == 0 && n == 0)
                    first = got, want = static_cast<double>(acc);
                wrong += got != static_cast<double>(acc);
            }
        char buf[112];
        snprintf(buf, sizeof buf, wrong ? "ran, %u of 256 wrong (first %g, want %g)" : "ran, exact", wrong, first, want);
        say(r, buf);
        ok = wrong == 0;
    } while (false);
    say(r, "\n");
    release(op);
    release(dml1);
    release(compiled);
    release(init);
    release(table);
    release(rec);
    release(heap);
    release(up);
    release(rin);
    release(rf);
    release(rout);
    release(rb);
    release(temp);
    release(persist);
    return ok;
}

// --- the GPU backend: every op kind in one compute shader ---------------------
//
// Each op gets `items` threads: one per element (elementwise, reductions,
// dot, argmax), one per output row (matvec) or element (matmul). A thread
// finds its op by binary search over the ops' first item. Reductions use
// integer atomics: order-independent, so exact. argmax packs (value, reversed
// index) so an atomic max picks the largest value and, among equals, the
// first. Codons travel as one uint each.
const char* const kAccelHlsl = R"(
struct Op { uint kind; uint n; uint key; uint src; uint dst; uint mat; uint outp; uint start; };
StructuredBuffer<Op> ops : register(t0);
StructuredBuffer<uint> cod : register(t1);
RWStructuredBuffer<uint> outc : register(u0);
RWStructuredBuffer<uint> scal : register(u1);
cbuffer K : register(b0) { uint nops; uint items; uint row; uint pad0; };
int sv(uint v) { return int(v & 63u) - 32; }
uint satc(int a) { int q = a >> 5; q = clamp(q, -32, 31); return uint(q + 32); }
[numthreads(64, 1, 1)]
void main(uint3 g : SV_GroupID, uint3 l : SV_GroupThreadID) {
    uint t = (g.y * row + g.x) * 64u + l.x;
    if (t >= items) return;
    uint lo = 0u, hi = nops;
    while (hi - lo > 1u) { uint mid = (lo + hi) >> 1; if (ops[mid].start <= t) lo = mid; else hi = mid; }
    Op o = ops[lo];
    uint i = t - o.start;
    uint k = o.n;
    if (o.kind == 17u) {
        int acc = 0;
        for (uint x = 0u; x < k; ++x) acc += sv(cod[o.mat + i * k + x]) * sv(cod[o.src + x]);
        outc[o.outp + i] = satc(acc);
        return;
    }
    if (o.kind == 18u) {
        uint j = i / k, c = i % k;
        int acc = 0;
        for (uint x = 0u; x < k; ++x) acc += sv(cod[o.src + j * k + x]) * sv(cod[o.mat + x * k + c]);
        outc[o.outp + i] = satc(acc);
        return;
    }
    uint s = (o.kind == 5u) ? 0u : cod[o.src + i];
    uint d = (o.kind == 0u || (o.kind >= 8u && o.kind <= 16u)) ? cod[o.dst + i] : 0u;
    switch (o.kind) {
    case 0u: if (s != d) InterlockedAdd(scal[lo], 1u); break;
    case 1u: if (s == o.key) InterlockedAdd(scal[lo], 1u); break;
    case 2u: if (s == o.key) InterlockedMin(scal[lo], i); break;
    case 3u: InterlockedAdd(scal[lo], s); break;
    case 4u: case 7u: outc[o.outp + i] = s; break;
    case 5u: outc[o.outp + i] = o.key; break;
    case 6u: outc[o.outp + i] = (63u - s) & 255u; break;
    case 8u: outc[o.outp + i] = (d + s) & 63u; break;
    case 9u: outc[o.outp + i] = (d - s) & 63u; break;
    case 10u: outc[o.outp + i] = (d ^ s) & 63u; break;
    case 11u: outc[o.outp + i] = d & s & 63u; break;
    case 12u: outc[o.outp + i] = (d | s) & 63u; break;
    case 13u: outc[o.outp + i] = min(d, s) & 63u; break;
    case 14u: outc[o.outp + i] = max(d, s) & 63u; break;
    case 15u: outc[o.outp + i] = (d * s) & 63u; break;
    case 16u: InterlockedAdd(scal[lo], uint(sv(s) * sv(d))); break;
    case 19u: InterlockedMax(scal[lo], (s << 24) | (0xFFFFFFu - i)); break;
    }
}
)";

struct GpuOpDesc {
    u32 kind, n, key, src, dst, mat, outp, start;
};
static_assert(sizeof(GpuOpDesc) == 32, "matches the shader's Op");

u64 align256(u64 v) { return (v + 255) & ~255ull; }

} // namespace

struct GpuBackend::Impl {
    Queue q;
    char name[128] = {};
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    struct Slot {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Resource *up = nullptr, *outc = nullptr, *scal = nullptr, *rb = nullptr;
        u64 up_cap = 0, out_cap = 0, scal_cap = 0, rb_cap = 0;
        D3D12_RESOURCE_STATES outc_state = D3D12_RESOURCE_STATE_COMMON, scal_state = D3D12_RESOURCE_STATE_COMMON;
        u64 fence = 0;
        bool busy = false;
        u32 nops = 0, out_codons = 0;
        Array<u32> kinds, outs; // per op, for collect
    } slots[kAccelSlots];

    void move(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES& cur,
              D3D12_RESOURCE_STATES to) {
        if (cur == to)
            return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = cur;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
        cur = to;
    }

    // Makes sure *res holds at least `need` bytes (recreating it bigger).
    bool ensure(ID3D12Resource*& res, u64& cap, u64 need, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav,
                D3D12_RESOURCE_STATES* tracked) {
        if (res && cap >= need)
            return true;
        release(res);
        u64 size = cap ? cap : 65536;
        while (size < need)
            size *= 2;
        res = q.buffer(size, heap, state, uav);
        cap = res ? size : 0;
        if (tracked)
            *tracked = D3D12_RESOURCE_STATE_COMMON; // buffers start in the common state
        return res != nullptr;
    }
};

bool GpuBackend::open(u32 index, TextBuf* err) {
    close();
    Found found[16];
    const u32 n = enumerate(found, 16);
    if (index >= n || !found[index].d.gpu) {
        release_all(found, n);
        say(err, "not a GPU");
        return false;
    }
    Impl* m = new Impl;
    strncpy(m->name, found[index].d.name, sizeof m->name - 1);
    bool ok = m->q.init(found[index].adapter, true, err);
    release_all(found, n);
    ID3DBlob *cs = nullptr, *msg = nullptr, *sig = nullptr;
    HRESULT hr = S_OK;
    if (ok) {
        hr = D3DCompile(kAccelHlsl, strlen(kAccelHlsl), "nova_accel", nullptr, nullptr, "main", "cs_5_1",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &msg);
        if (FAILED(hr)) {
            say_hr(err, "accelerator shader failed to compile", hr);
            if (msg && err) {
                err->append(": ");
                err->append(static_cast<const char*>(msg->GetBufferPointer()));
            }
            ok = false;
        }
    }
    release(msg);
    if (ok) {
        D3D12_ROOT_PARAMETER params[5] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 1;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 1;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[4].Constants.ShaderRegister = 0;
        params[4].Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 5;
        rsd.pParameters = params;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        if (FAILED(hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &msg)) ||
            FAILED(hr = m->q.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                      IID_PPV_ARGS(&m->root)))) {
            say_hr(err, "root signature failed", hr);
            ok = false;
        } else {
            pd.pRootSignature = m->root;
            pd.CS.pShaderBytecode = cs->GetBufferPointer();
            pd.CS.BytecodeLength = cs->GetBufferSize();
            if (FAILED(hr = m->q.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m->pso)))) {
                say_hr(err, "pipeline failed", hr);
                ok = false;
            }
        }
    }
    for (u32 s = 0; ok && s < kAccelSlots; ++s) {
        Impl::Slot& sl = m->slots[s];
        if (FAILED(hr = m->q.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&sl.alloc))) ||
            FAILED(hr = m->q.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, sl.alloc, nullptr,
                                                    IID_PPV_ARGS(&sl.list)))) {
            say_hr(err, "command list failed", hr);
            ok = false;
        } else {
            sl.list->Close();
        }
    }
    release(cs);
    release(msg);
    release(sig);
    impl_ = m;
    if (!ok)
        close();
    return ok;
}

void GpuBackend::close() {
    if (!impl_)
        return;
    Impl* m = impl_;
    if (m->q.queue && m->q.fence) { // let the device finish first
        m->q.queue->Signal(m->q.fence, ++m->q.value);
        if (m->q.fence->GetCompletedValue() < m->q.value) {
            m->q.fence->SetEventOnCompletion(m->q.value, m->q.event);
            WaitForSingleObject(m->q.event, INFINITE);
        }
    }
    for (u32 s = 0; s < kAccelSlots; ++s) {
        Impl::Slot& sl = m->slots[s];
        release(sl.list);
        release(sl.alloc);
        release(sl.up);
        release(sl.outc);
        release(sl.scal);
        release(sl.rb);
    }
    release(m->pso);
    release(m->root);
    m->q.destroy();
    delete m;
    impl_ = nullptr;
}

const char* GpuBackend::name() const { return impl_ ? impl_->name : "none"; }

int GpuBackend::submit(const AccelOp* ops, const u32* pick, u32 n, const u8* in) {
    if (!impl_ || n == 0)
        return -1;
    Impl* m = impl_;
    int slot = -1;
    for (u32 s = 0; s < kAccelSlots; ++s)
        if (!m->slots[s].busy) {
            slot = static_cast<int>(s);
            break;
        }
    if (slot < 0)
        return -1;
    Impl::Slot& sl = m->slots[slot];
    // The op table, the codons (one uint each) and the scalars' starting values.
    u64 codons = 0, items = 0;
    u32 out_codons = 0;
    for (u32 i = 0; i < n; ++i) {
        const AccelOp& op = ops[pick[i]];
        codons += op.src_n + op.dst_n + op.mat_n;
        items += op.kind == kAccMatvec ? op.n : (op.kind == kAccMatmul ? op.n * op.n : op.n);
        out_codons += op.out_n;
    }
    if (items > 0xFFFFFFFFull || codons > 0x3FFFFFFFull)
        return -1;
    const u64 ops_bytes = align256(n * sizeof(GpuOpDesc));
    const u64 cod_bytes = align256((codons ? codons : 1) * 4);
    const u64 scal_bytes = align256(n * 4);
    const u64 out_bytes = align256((out_codons ? out_codons : 1) * 4);
    if (!m->ensure(sl.up, sl.up_cap, ops_bytes + cod_bytes + scal_bytes, D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ, false, nullptr) ||
        !m->ensure(sl.outc, sl.out_cap, out_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true,
                   &sl.outc_state) ||
        !m->ensure(sl.scal, sl.scal_cap, scal_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true,
                   &sl.scal_state) ||
        !m->ensure(sl.rb, sl.rb_cap, out_bytes + scal_bytes, D3D12_HEAP_TYPE_READBACK,
                   D3D12_RESOURCE_STATE_COPY_DEST, false, nullptr))
        return -1;
    unsigned char* up = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(sl.up->Map(0, &none, reinterpret_cast<void**>(&up))))
        return -1;
    GpuOpDesc* desc = reinterpret_cast<GpuOpDesc*>(up);
    u32* cod = reinterpret_cast<u32*>(up + ops_bytes);
    u32* scal0 = reinterpret_cast<u32*>(up + ops_bytes + cod_bytes);
    sl.kinds.resize(n);
    sl.outs.resize(n);
    u32 cpos = 0, opos = 0, ipos = 0;
    for (u32 i = 0; i < n; ++i) {
        const AccelOp& op = ops[pick[i]];
        const u8* src = in + op.in_off;
        GpuOpDesc& d = desc[i];
        d.kind = op.kind;
        d.n = op.n;
        d.key = op.key & 63;
        d.src = cpos;
        d.dst = cpos + op.src_n;
        d.mat = cpos + op.src_n + op.dst_n;
        d.outp = opos;
        d.start = ipos;
        const u32 reads = op.src_n + op.dst_n + op.mat_n;
        for (u32 j = 0; j < reads; ++j)
            cod[cpos + j] = src[j];
        cpos += reads;
        opos += op.out_n;
        ipos += op.kind == kAccMatvec ? op.n : (op.kind == kAccMatmul ? op.n * op.n : op.n);
        scal0[i] = op.kind == kAccFind ? kAccNotFound : 0u;
        sl.kinds[i] = op.kind;
        sl.outs[i] = op.out_n;
    }
    sl.up->Unmap(0, nullptr);
    sl.nops = n;
    sl.out_codons = out_codons;
    // Record: scalars' starting values in, the kernel, results out.
    sl.alloc->Reset();
    sl.list->Reset(sl.alloc, m->pso);
    ID3D12GraphicsCommandList* L = sl.list;
    // Buffers decay to the common state when a submission finishes.
    sl.outc_state = sl.scal_state = D3D12_RESOURCE_STATE_COMMON;
    m->move(L, sl.scal, sl.scal_state, D3D12_RESOURCE_STATE_COPY_DEST);
    L->CopyBufferRegion(sl.scal, 0, sl.up, ops_bytes + cod_bytes, n * 4);
    m->move(L, sl.scal, sl.scal_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m->move(L, sl.outc, sl.outc_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    L->SetComputeRootSignature(m->root);
    L->SetPipelineState(m->pso);
    const D3D12_GPU_VIRTUAL_ADDRESS va = sl.up->GetGPUVirtualAddress();
    L->SetComputeRootShaderResourceView(0, va);
    L->SetComputeRootShaderResourceView(1, va + ops_bytes);
    L->SetComputeRootUnorderedAccessView(2, sl.outc->GetGPUVirtualAddress());
    L->SetComputeRootUnorderedAccessView(3, sl.scal->GetGPUVirtualAddress());
    const u64 groups = (items + 63) / 64;
    if (groups) {
        const u32 row = static_cast<u32>(groups < 65535 ? groups : 65535);
        const u32 rows = static_cast<u32>((groups + row - 1) / row);
        const u32 k[4] = {n, static_cast<u32>(items), row, 0};
        L->SetComputeRoot32BitConstants(4, 4, k, 0);
        L->Dispatch(row, rows, 1);
    }
    m->move(L, sl.outc, sl.outc_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m->move(L, sl.scal, sl.scal_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (out_codons)
        L->CopyBufferRegion(sl.rb, 0, sl.outc, 0, static_cast<u64>(out_codons) * 4);
    L->CopyBufferRegion(sl.rb, out_bytes, sl.scal, 0, static_cast<u64>(n) * 4);
    if (FAILED(L->Close()))
        return -1;
    ID3D12CommandList* lists[] = {L};
    m->q.queue->ExecuteCommandLists(1, lists);
    m->q.queue->Signal(m->q.fence, ++m->q.value);
    sl.fence = m->q.value;
    sl.busy = true;
    return slot;
}

bool GpuBackend::collect(int ticket, u8* codons, u32* scalars) {
    if (!impl_ || ticket < 0 || ticket >= static_cast<int>(kAccelSlots))
        return false;
    Impl* m = impl_;
    Impl::Slot& sl = m->slots[ticket];
    if (!sl.busy)
        return false;
    if (m->q.fence->GetCompletedValue() < sl.fence) {
        m->q.fence->SetEventOnCompletion(sl.fence, m->q.event);
        WaitForSingleObject(m->q.event, INFINITE);
    }
    sl.busy = false;
    if (FAILED(m->q.dev->GetDeviceRemovedReason()))
        return false;
    const u64 out_bytes = align256((sl.out_codons ? sl.out_codons : 1) * 4);
    void* mapped = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(out_bytes + sl.nops * 4)};
    if (FAILED(sl.rb->Map(0, &all, &mapped)))
        return false;
    const u32* rb = static_cast<const u32*>(mapped);
    for (u32 i = 0; i < sl.out_codons; ++i)
        codons[i] = static_cast<u8>(rb[i]);
    const u32* sc = reinterpret_cast<const u32*>(static_cast<const unsigned char*>(mapped) + out_bytes);
    for (u32 i = 0; i < sl.nops; ++i) {
        u32 v = sc[i];
        if (sl.kinds[i] == kAccArgmax) // no element (n = 0) leaves it 0: index 0
            v = v ? 0xFFFFFFu - (v & 0xFFFFFFu) : 0u;
        scalars[i] = v;
    }
    D3D12_RANGE none = {0, 0};
    sl.rb->Unmap(0, &none);
    return true;
}

// --- warps on the GPU (Phase 8, design.md 6.14) --------------------------------
//
// The kernel is warp_kernel.hlsl: warp_isa.inc itself, between a head that
// binds its macros to HLSL and a main that picks the body record, one thread
// group per record and one thread per lane. The CPU reference includes the
// same warp_isa.inc as C++ (warp.cpp), so both run one source.
//
// Its bytecode (docs/dxc-plan.md): DXC's, compiled at build time for 32
// lanes and embedded (the plain kernel and each groupshared tier), when the
// build had DXC, the world has 32 lanes and the device takes Shader Model
// 6.0; else FXC's, compiled here from the embedded source (and cached).
namespace {

#include "warp_isa_bytes.inc"
#include "warp_layout_bytes.inc"
#include "warp_kernel_bytes.inc"

#if NOVA_DXC
#include "warp_dxil_16_0.inc"
#include "warp_dxil_16_256.inc"
#include "warp_dxil_16_512.inc"
#include "warp_dxil_16_1024.inc"
#include "warp_dxil_16_2048.inc"
#include "warp_dxil_32_0.inc"
#include "warp_dxil_32_256.inc"
#include "warp_dxil_32_512.inc"
#include "warp_dxil_32_1024.inc"
#include "warp_dxil_32_2048.inc"
#include "warp_dxil_64_0.inc"
#include "warp_dxil_64_256.inc"
#include "warp_dxil_64_512.inc"
#include "warp_dxil_64_1024.inc"
#include "warp_dxil_64_2048.inc"
#include "warp_dxil_128_0.inc"
#include "warp_dxil_128_256.inc"
#include "warp_dxil_128_512.inc"
#include "warp_dxil_128_1024.inc"
#include "warp_dxil_128_2048.inc"
#include "warp_dxil_256_0.inc"
#include "warp_dxil_256_256.inc"
#include "warp_dxil_256_512.inc"
#include "warp_dxil_256_1024.inc"
#include "warp_dxil_256_2048.inc"
#include "warp_dxil_512_0.inc"
#include "warp_dxil_512_256.inc"
#include "warp_dxil_512_512.inc"
#include "warp_dxil_512_1024.inc"
#include "warp_dxil_512_2048.inc"
#include "warp_dxil_1024_0.inc"
#include "warp_dxil_1024_256.inc"
#include "warp_dxil_1024_512.inc"
#include "warp_dxil_1024_1024.inc"
#include "warp_dxil_1024_2048.inc"
#endif

// DXC's kernels in this build: lanes, groupshared words, the bytecode.
struct DxilKernel {
    u32 lanes, lds;
    const unsigned char* code;
    usize size;
};
#if NOVA_DXC
#define NOVA_DXIL(w, l) {w, l, kWarpDxil##w##_##l, sizeof kWarpDxil##w##_##l - 1}
#define NOVA_DXIL_WIDTH(w) NOVA_DXIL(w, 0), NOVA_DXIL(w, 256), NOVA_DXIL(w, 512), NOVA_DXIL(w, 1024), NOVA_DXIL(w, 2048)
const DxilKernel kDxil[] = {
    NOVA_DXIL_WIDTH(16),  NOVA_DXIL_WIDTH(32),  NOVA_DXIL_WIDTH(64),   NOVA_DXIL_WIDTH(128),
    NOVA_DXIL_WIDTH(256), NOVA_DXIL_WIDTH(512), NOVA_DXIL_WIDTH(1024),
};
#undef NOVA_DXIL_WIDTH
#undef NOVA_DXIL
constexpr u32 kDxilCount = sizeof kDxil / sizeof kDxil[0];
#else
const DxilKernel* const kDxil = nullptr;
constexpr u32 kDxilCount = 0;
#endif

const DxilKernel* find_dxil(u32 lanes, u32 lds) {
    for (u32 i = 0; i < kDxilCount; ++i)
        if (kDxil[i].lanes == lanes && kDxil[i].lds == lds)
            return &kDxil[i];
    return nullptr;
}

// FXC's includes: the kernel file's two, from the bytes embedded beside it.
struct WarpIncludes : ID3DInclude {
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        if (strcmp(name, "warp_layout.inc") == 0) {
            *data = kWarplayoutSource;
            *bytes = static_cast<UINT>(sizeof kWarplayoutSource - 1);
            return S_OK;
        }
        if (strcmp(name, "warp_isa.inc") == 0) {
            *data = kWarpisaSource;
            *bytes = static_cast<UINT>(sizeof kWarpisaSource - 1);
            return S_OK;
        }
        return E_FAIL;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
};

// A groupshared tier's largest size (WarpKernelOptions::lds): its image, the
// constants (256 words) and the step's own words (two a lane, and a few)
// within D3D12's 32 KB a group, at the widest warps (1,024 lanes:
// docs/inject-plan.md W3).
constexpr u32 kWarpLdsMax = 5632;
static_assert(kWarpLdsMax + 256 + 2 * kMaxLanes + 32 <= 8192, "the widest warps' groupshared words fit 32 KB");
static_assert(WC_COUNT <= 256, "the constants fit gs_cst");

static_assert(sizeof(Lane) == 29 * 4 && kMaxStack == 16, "the kernel's Lane is soup.h's");

// The kernel takes FXC about a minute and a half to compile, so its bytecode
// is kept: in memory for the process, and in a file beside the executable
// (nova_warp_<key>.cso), keyed by a hash of the sources, the lane count, the
// groupshared size and the compiler flags. A stale or damaged file only costs
// a compile.
struct WarpCode {
    u64 key = 0;
    Array<u8> code;
};
WarpCode g_warp_code[8];
u32 g_warp_codes = 0;

UINT warp_compile_flags() {
    // NOVA_WARP_O: 0 (the default) skips FXC's optimizer, which takes 3x as
    // long and the driver re-optimizes anyway; 3 is its strongest.
    const char* v = getenv("NOVA_WARP_O");
    return v && v[0] == '3' ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : D3DCOMPILE_SKIP_OPTIMIZATION;
}

void warp_code_path(u64 key, char* out, usize n) {
    char exe[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    usize cut = len;
    while (cut > 0 && exe[cut - 1] != '\\' && exe[cut - 1] != '/')
        --cut;
    exe[cut] = 0;
    snprintf(out, n, "%snova_warp_%016llx.cso", exe, static_cast<unsigned long long>(key));
}

// FXC's bytecode for the kernel with `lanes` lanes and `lds` groupshared
// words, compiled or from a cache.
bool warp_fxc_bytecode(u32 lanes, u32 lds, Array<u8>* code, TextBuf* err) {
    const UINT flags = warp_compile_flags();
    char nl[16], ld[16];
    snprintf(nl, sizeof nl, "%u", lanes);
    snprintf(ld, sizeof ld, "%u", lds);
    const D3D_SHADER_MACRO defs[] = {{"W_LANES", nl}, {"W_LDS", ld}, {nullptr, nullptr}};
    Hasher h(0x7761727063736F00ull); // "warpcso"
    h.bytes(kWarpkernelSource, sizeof kWarpkernelSource - 1);
    h.bytes(kWarplayoutSource, sizeof kWarplayoutSource - 1);
    h.bytes(kWarpisaSource, sizeof kWarpisaSource - 1);
    h.word(lanes);
    h.word(lds);
    h.word(flags);
    const u64 key = h.finish();
    for (u32 i = 0; i < g_warp_codes; ++i) {
        if (g_warp_code[i].key == key) {
            code->assign(g_warp_code[i].code.data(), g_warp_code[i].code.size());
            return true;
        }
    }
    char path[MAX_PATH + 64];
    warp_code_path(key, path, sizeof path);
    bool have = false;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < (64l << 20)) {
            code->resize(static_cast<usize>(size));
            have = fread(code->data(), 1, static_cast<usize>(size), f) == static_cast<usize>(size);
        }
        fclose(f);
    }
    if (!have) {
        ID3DBlob *cs = nullptr, *msg = nullptr;
        WarpIncludes inc;
        const HRESULT hr = D3DCompile(kWarpkernelSource, sizeof kWarpkernelSource - 1, "warp_kernel.hlsl", defs, &inc,
                                      "main", "cs_5_1", flags, 0, &cs, &msg);
        if (FAILED(hr)) {
            say_hr(err, "warp kernel failed to compile", hr);
            if (msg && err) {
                err->append(": ");
                err->append(static_cast<const char*>(msg->GetBufferPointer()));
            }
            release(msg);
            return false;
        }
        release(msg);
        code->assign(static_cast<const u8*>(cs->GetBufferPointer()), cs->GetBufferSize());
        release(cs);
        if (FILE* f = fopen(path, "wb")) {
            fwrite(code->data(), 1, code->size(), f);
            fclose(f);
        }
    }
    WarpCode& slot = g_warp_code[g_warp_codes < 8 ? g_warp_codes++ : 7];
    slot.key = key;
    slot.code.assign(code->data(), code->size());
    return true;
}

bool ensure_buffer(Queue& q, ID3D12Resource*& res, u64& cap, u64 need, D3D12_HEAP_TYPE heap,
                   D3D12_RESOURCE_STATES state, bool uav) {
    if (res && cap >= need)
        return true;
    release(res);
    u64 size = cap ? cap : 65536;
    while (size < need)
        size *= 2;
    res = q.buffer(size, heap, state, uav);
    cap = res ? size : 0;
    return res != nullptr;
}

} // namespace

// The lanes' own small kernel: moves whole warps' lanes between a packed list
// and the GPU's lane heap (mode 0: list into the heap, sending them; mode 1:
// heap into the list, fetching them). Lanes are 29 words. Each warp has three
// words in g_at: its first lane in the heap, its first lane in the list, and
// its lanes (its node's width: docs/inject-plan.md W1). A group a warp, so
// no integer division (which this GPU can get wrong, warp_isa.inc's w_ulong:
// a word's warp as t / words once landed lanes in the wrong warp).
const char* const kLaneMoveHlsl = R"(
StructuredBuffer<uint> g_at : register(t0);
StructuredBuffer<uint> g_src : register(t1);
RWStructuredBuffer<uint> g_heap : register(u0);
RWStructuredBuffer<uint> g_out : register(u1);
cbuffer K : register(b0) { uint g_warps; uint g_unused; uint g_mode; uint g_row; };
[numthreads(64, 1, 1)]
void main(uint3 g : SV_GroupID, uint3 l : SV_GroupThreadID) {
    uint w = g.y * g_row + g.x;
    if (w >= g_warps)
        return;
    uint h = g_at[3u * w] * 29u, s = g_at[3u * w + 1u] * 29u, words = g_at[3u * w + 2u] * 29u;
    for (uint k = l.x; k < words; k += 64u) {
        if (g_mode == 0u)
            g_heap[h + k] = g_src[s + k];
        else
            g_out[s + k] = g_heap[h + k];
    }
}
)";

// The lanes of warps that stopped at a host instruction, gathered after the
// round's run into one buffer that comes back with the round (so the host
// needn't fetch them in a round trip of their own). A group per record and
// warp: the first thread checks the warp's wait flag and takes a slot and
// room for its lanes (in any order: the host finds each warp by its lane
// index); the group copies the lanes. Each warp takes its own record's width
// of room (WB_LANES: docs/inject-plan.md W1, nodes' widths differ). g_out:
// [0] warps that wanted a slot, [1] lanes they wanted, [2, 2 + 2 x cap) each
// slot's heap lane index and its first lane in the lanes (none: no room),
// then the lanes, room for lcap of them. A warp past either capacity isn't
// gathered (the host fetches it as before). Groups: x the warp in its
// record, y + z x g_row the record (no integer division: see kLaneMoveHlsl).
const char* const kLaneGatherHlsl = R"(
StructuredBuffer<uint4> g_tab : register(t0);
StructuredBuffer<uint> g_img : register(t1);
RWStructuredBuffer<uint> g_heap : register(u0);
RWStructuredBuffer<uint> g_out : register(u1);
cbuffer K : register(b0) { uint g_n; uint g_cap; uint g_lcap; uint g_row; };
groupshared uint gs_dst;
groupshared uint gs_at;
[numthreads(64, 1, 1)]
void main(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    uint r = g.z * g_row + g.y;
    if (r >= g_n)
        return;
    uint4 e = g_tab[r];
    uint i = g.x;
    uint rec = e.x + e.y;
    uint lanes = g_img[rec + WB_LANES];
    if (t.x == 0u) {
        gs_dst = 0xFFFFFFFFu;
        if (i < g_img[rec + WB_NWARP] && g_img[rec + WB_WARPS + i * WW_SIZE + WW_WAIT] != 0u) {
            uint at = e.z + g_img[rec + WB_WARPS + i * WW_SIZE + WW_LANE0];
            uint k = 0u;
            uint off = 0u;
            InterlockedAdd(g_out[0], 1u, k);
            InterlockedAdd(g_out[1], lanes, off);
            if (k < g_cap) {
                bool fits = off + lanes <= g_lcap;
                g_out[2u + 2u * k] = at;
                g_out[3u + 2u * k] = fits ? off : 0xFFFFFFFFu;
                if (fits) {
                    gs_dst = off;
                    gs_at = at;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint dst = gs_dst;
    if (dst == 0xFFFFFFFFu)
        return;
    uint words = lanes * 29u;
    uint src = gs_at * 29u, o = 2u + 2u * g_cap + dst * 29u;
    for (uint w = t.x; w < words; w += 64u)
        g_out[o + w] = g_heap[src + w];
}
)";

// A round in flight on its own queue (the overlap plan: two batches, so one
// group of nodes' kernel can run while another's is sent).
struct WarpBatch {
    Queue* q = nullptr; // batch 0's is the device's own queue; batch 1 has its own on the same device
    // The gathered lanes: their buffer and readback, the round's capacity in
    // warps and in lanes (each from the last round's count), and whether the
    // last round has them.
    ID3D12Resource *gout = nullptr, *grb = nullptr;
    u64 gout_cap = 0, grb_cap = 0;
    u32 gcap = 256, gcap_round = 0;
    u32 glcap = 256 * 32, glcap_round = 0;
    bool gathered = false;
    Array<u64> gpairs; // take_gathered's scratch
    // A round's buffers.
    ID3D12Resource *up = nullptr, *img = nullptr, *rb = nullptr;
    u64 up_cap = 0, img_cap = 0, rb_cap = 0;
    const u32* rb_map = nullptr; // the readback, left mapped by finish(..., false) until the next submit
    Array<u64> back_out;         // finish's scratch: per job, its steps, longest body and largest record
    ID3D12Resource* old_heap = nullptr; // a heap this round replaced: kept until it finishes
    // The round in flight, and whether it moved every node's lanes into a new
    // heap first (then nothing may touch the heap until it is done).
    bool busy = false, grew = false;
    const WarpJob* jobs = nullptr;
    u32 njobs = 0;
    u64 img_words = 0, nrec = 0, sent = 0, sent_lanes = 0; // (sent: warps; sent_lanes: their lanes)
    double in_s = 0, t_exec = 0;
    double prep_s = 0, post_s = 0; // (profiling) submit before the copy in, and after it
    Array<u32> table, at;
    Array<u64> img_at, send_at;
    Array<u64> keys;      // the table's order (submit)
    Array<u8> kinds;      // ... each entry's kind, or 0xFF for a long one
    Array<u32> ents, runs; // the table's entries in job order; its dispatches (kind, first, count)
    Array<u64> rbase; // the lane heap's layout when this round was sent (its gathered lanes are found by it)
    // timestamps: before the upload, after it, after the kernel, after the download
    ID3D12QueryHeap* qh = nullptr;
    ID3D12Resource* qrb = nullptr;
};

// A warp width's kernels (docs/inject-plan.md W3): the plain one and its
// groupshared tiers, DXC's or FXC's.
struct WarpWidth {
    ID3D12PipelineState* pso = nullptr;    // records in memory
    ID3D12PipelineState* pso_tier[4] = {}; // records of at most tier[k] words, in groupshared memory
    u32 tier[4] = {}, ntier = 0;
    bool dxc = false;
};
constexpr u32 kWidths = 11; // warps of 2^0 to 2^10 lanes (kMaxLanes)

u32 width_index(u32 lanes) { // log2 of a power of two
    u32 k = 0;
    while ((1u << k) < lanes)
        ++k;
    return k;
}

u32 popcount32(u32 v) {
    u32 n = 0;
    for (; v; v &= v - 1)
        ++n;
    return n;
}

struct WarpGpu::Impl {
    Queue q, q2; // the device's queue (batch 0) and a second on it (batch 1)
    char name[128] = {};
    u32 widths = 0; // the widths it has kernels for (bit k: 2^k lanes)
    ID3D12RootSignature* root = nullptr;
    WarpWidth width[kWidths];
    // Each node's lanes a warp, as its jobs gave them (a node's width is its
    // site's, fixed: docs/inject-plan.md W1).
    Array<u32> nwl;
    bool long_first = false; // WarpKernelOptions
    ID3D12PipelineState* pso_move = nullptr; // sending and fetching lanes
    ID3D12PipelineState* pso_gather = nullptr; // waiting warps' lanes into the round's readback
    WarpBatch batch[WarpGpu::kBatches];
    // Every node's lanes, kept between rounds: node n's warp slot s at lane
    // rbase[n] + s x lanes. Shared by the batches (their nodes are disjoint).
    ID3D12Resource* heap = nullptr;
    u64 heap_lanes = 0;
    Array<u64> rbase, rcap;
    // Fetching.
    ID3D12Resource *fup = nullptr, *fstage = nullptr, *frb = nullptr;
    u64 fup_cap = 0, fstage_cap = 0, frb_cap = 0;
    Array<u32> fat; // fetch's heap positions
    u64 freq = 0; // timestamp ticks a second (0: none)

    void dispatch_rows(ID3D12GraphicsCommandList* L, u64 groups, u32 c0, u32 c1, u32 c2) {
        const u32 row = static_cast<u32>(groups < 65535 ? groups : 65535);
        const u32 rows = static_cast<u32>((groups + row - 1) / row);
        const u32 k[4] = {c0, c1, c2, row};
        L->SetComputeRoot32BitConstants(4, 4, k, 0);
        L->Dispatch(row, rows, 1);
    }
};

bool warp_dxc_built() {
    return kDxilCount > 0;
}

bool WarpGpu::open(u32 index, u32 widths, TextBuf* err, double* compile_ms, const WarpKernelOptions* options) {
    close();
    dxc_ = false;
    kernel_[0] = 0;
    if (widths == 0 || widths >= (kMaxLanes << 1)) {
        say(err, "warp widths out of range");
        return false;
    }
    const WarpKernelOptions opt = options ? *options : WarpKernelOptions{};
    auto tiers_of = [](const u32* t) {
        u32 k = 0;
        while (k < 4 && t[k])
            ++k;
        return k;
    };
    const u32 ndxc = tiers_of(opt.lds), nfxc = tiers_of(opt.fxc_lds);
    for (u32 pass = 0; pass < 2; ++pass) {
        const u32* t = pass == 0 ? opt.lds : opt.fxc_lds;
        for (u32 k = 0; k < (pass == 0 ? ndxc : nfxc); ++k)
            if (t[k] > kWarpLdsMax || (k > 0 && t[k] <= t[k - 1])) {
                say(err, "groupshared tiers must ascend, each at most 5,632 words");
                return false;
            }
    }
    Found found[16];
    const u32 n = enumerate(found, 16);
    if (index >= n || !found[index].d.gpu) {
        release_all(found, n);
        say(err, "not a GPU");
        return false;
    }
    Impl* m = new Impl;
    m->widths = widths;
    m->long_first = opt.long_first;
    strncpy(m->name, found[index].d.name, sizeof m->name - 1);
    bool ok = m->q.init(found[index].adapter, true, err);
    release_all(found, n);
    ok = ok && m->q2.init_on(m->q.dev, err);
    m->batch[0].q = &m->q;
    m->batch[1].q = &m->q2;
    ID3DBlob *msg = nullptr, *sig = nullptr, *mv = nullptr, *ga = nullptr;
    HRESULT hr = S_OK;
    const double t0 = clock_seconds();
    if (ok) { // the root signature, which every kernel here shares
        D3D12_ROOT_PARAMETER params[5] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 1;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 1;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[4].Constants.ShaderRegister = 0;
        params[4].Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 5;
        rsd.pParameters = params;
        if (FAILED(hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &msg)) ||
            FAILED(hr = m->q.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                      IID_PPV_ARGS(&m->root)))) {
            say_hr(err, "root signature failed", hr);
            ok = false;
        }
        release(msg);
    }
    auto pipeline = [&](const void* code, usize size, ID3D12PipelineState** out) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m->root;
        pd.CS.pShaderBytecode = code;
        pd.CS.BytecodeLength = size;
        hr = m->q.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(out));
        return SUCCEEDED(hr);
    };
    // The warp kernels, a set for each width the world's nodes have
    // (docs/inject-plan.md W3): DXC's where they can run (docs/dxc-plan.md
    // D3), else FXC's.
    const char* why_fxc = nullptr; // (the first width that runs FXC's: why)
    u32 first_width = kWidths;
    if (ok) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {};
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
        const bool sm6 = SUCCEEDED(m->q.dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) &&
                         sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
        for (u32 wi = 0; wi < kWidths && ok; ++wi) {
            if (!(widths & (1u << wi)))
                continue;
            first_width = first_width < wi ? first_width : wi;
            const u32 lanes = 1u << wi;
            WarpWidth& W = m->width[wi];
            const char* why = nullptr;
            if (opt.fxc)
                why = "warp.kernel = fxc";
            else if (kDxilCount == 0)
                why = "this build has no DXC kernels";
            else if (!find_dxil(lanes, 0))
                why = "DXC's kernels are built for 16 to 1,024 lanes";
            else if (!sm6)
                why = "the device doesn't take Shader Model 6.0";
            for (u32 k = 0; k < ndxc && !why; ++k)
                if (!find_dxil(lanes, opt.lds[k]))
                    why = "a groupshared tier the build didn't compile";
            if (!why) {
                const DxilKernel* p = find_dxil(lanes, 0);
                bool made = pipeline(p->code, p->size, &W.pso);
                for (u32 k = 0; k < ndxc && made; ++k) {
                    p = find_dxil(lanes, opt.lds[k]);
                    made = pipeline(p->code, p->size, &W.pso_tier[k]);
                }
                if (made) {
                    W.dxc = true;
                    W.ntier = ndxc;
                    for (u32 k = 0; k < ndxc; ++k)
                        W.tier[k] = opt.lds[k];
                } else {
                    release(W.pso);
                    for (u32 k = 0; k < 4; ++k)
                        release(W.pso_tier[k]);
                    why = "the device refused DXC's kernels";
                }
            }
            if (why) {
                why_fxc = why_fxc ? why_fxc : why;
                Array<u8> code;
                ok = warp_fxc_bytecode(lanes, 0, &code, err);
                if (ok && !pipeline(code.data(), code.size(), &W.pso)) {
                    say_hr(err, "pipeline failed", hr);
                    ok = false;
                }
                for (u32 k = 0; k < nfxc && ok; ++k) {
                    ok = warp_fxc_bytecode(lanes, opt.fxc_lds[k], &code, err);
                    if (ok && !pipeline(code.data(), code.size(), &W.pso_tier[k])) {
                        say_hr(err, "pipeline failed", hr);
                        ok = false;
                    }
                }
                W.ntier = nfxc;
                for (u32 k = 0; k < nfxc; ++k)
                    W.tier[k] = opt.fxc_lds[k];
            }
        }
    }
    if (ok) { // what runs, for the warp notes and --profile
        dxc_ = why_fxc == nullptr;
        const WarpWidth& W0 = m->width[first_width < kWidths ? first_width : 0];
        TextBuf d;
        d.append(dxc_ ? "DXC's kernel" : "FXC's kernel");
        d.append(" for warps of ");
        for (u32 wi = 0, said = 0, all = popcount32(widths); wi < kWidths; ++wi) {
            if (!(widths & (1u << wi)))
                continue;
            char w[32];
            snprintf(w, sizeof w, "%s%u", said == 0 ? "" : (said + 1 == all ? " and " : ", "), 1u << wi);
            d.append(w);
            ++said;
        }
        d.append(" lanes");
        if (W0.ntier) {
            d.append(W0.ntier == 1 ? ", groupshared images up to " : ", groupshared tiers of ");
            for (u32 k = 0; k < W0.ntier; ++k) {
                char w[32];
                snprintf(w, sizeof w, "%s%u", k == 0 ? "" : (k + 1 == W0.ntier ? " and " : ", "), W0.tier[k]);
                d.append(w);
            }
            d.append(" words");
        }
        if (m->long_first)
            d.append(", long bodies first");
        if (why_fxc) {
            d.append(" (");
            d.append(why_fxc);
            d.append(")");
        }
        snprintf(kernel_, sizeof kernel_, "%s", d.c_str());
    }
    if (ok) {
        hr = D3DCompile(kLaneMoveHlsl, strlen(kLaneMoveHlsl), "lane_move", nullptr, nullptr, "main", "cs_5_1",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &mv, &msg);
        if (FAILED(hr)) {
            say_hr(err, "lane kernel failed to compile", hr);
            ok = false;
        }
        release(msg);
    }
    if (ok) { // the gather kernel reads the image's layout
        TextBuf gsrc;
        gsrc.append(reinterpret_cast<const char*>(kWarplayoutSource));
        gsrc.append(kLaneGatherHlsl);
        hr = D3DCompile(gsrc.c_str(), gsrc.size(), "lane_gather", nullptr, nullptr, "main", "cs_5_1",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ga, &msg);
        if (FAILED(hr)) {
            say_hr(err, "gather kernel failed to compile", hr);
            if (msg && err) {
                err->append(": ");
                err->append(static_cast<const char*>(msg->GetBufferPointer()));
            }
            ok = false;
        }
        release(msg);
    }
    if (ok && (!pipeline(mv->GetBufferPointer(), mv->GetBufferSize(), &m->pso_move) ||
               !pipeline(ga->GetBufferPointer(), ga->GetBufferSize(), &m->pso_gather))) {
        say_hr(err, "pipeline failed", hr);
        ok = false;
    }
    if (compile_ms)
        *compile_ms = (clock_seconds() - t0) * 1000.0;
    release(msg);
    release(sig);
    release(mv);
    release(ga);
    if (ok) { // timestamps for --profile, a set a batch; a device without them just has none
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 4;
        UINT64 f = 0, f2 = 0;
        bool stamps = SUCCEEDED(m->q.queue->GetTimestampFrequency(&f)) && f &&
                      SUCCEEDED(m->q2.queue->GetTimestampFrequency(&f2)) && f2 == f;
        for (u32 b = 0; b < kBatches && stamps; ++b) {
            WarpBatch& B = m->batch[b];
            stamps = SUCCEEDED(m->q.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&B.qh)));
            if (stamps)
                B.qrb = m->q.buffer(4 * sizeof(u64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
            stamps = stamps && B.qrb;
        }
        m->freq = stamps ? f : 0;
    }
    impl_ = m;
    if (!ok)
        close();
    return ok;
}

void WarpGpu::close() {
    if (!impl_)
        return;
    Impl* m = impl_;
    for (u32 b = 0; b < kBatches; ++b) {
        WarpBatch& B = m->batch[b];
        if (B.busy && B.q)
            B.q->wait(nullptr);
        if (B.rb_map) {
            D3D12_RANGE none = {0, 0};
            B.rb->Unmap(0, &none);
            B.rb_map = nullptr;
        }
        release(B.qh);
        release(B.qrb);
        release(B.up);
        release(B.img);
        release(B.rb);
        release(B.old_heap);
        release(B.gout);
        release(B.grb);
    }
    release(m->heap);
    release(m->fup);
    release(m->fstage);
    release(m->frb);
    for (u32 wi = 0; wi < kWidths; ++wi) {
        release(m->width[wi].pso);
        for (u32 k = 0; k < 4; ++k)
            release(m->width[wi].pso_tier[k]);
    }
    release(m->pso_move);
    release(m->pso_gather);
    release(m->root);
    m->q2.destroy();
    m->q.destroy();
    delete m;
    impl_ = nullptr;
}

const char* WarpGpu::name() const { return impl_ ? impl_->name : "none"; }

double WarpGpu::timestamp_hz() const { return impl_ ? static_cast<double>(impl_->freq) : 0.0; }

namespace {

// The round's copying in, a job (node) at a time, in parallel: its image and
// the lanes it sends into the upload buffer.
struct WarpCopy {
    const WarpJob* jobs;
    const u64* img_at;  // per job: its image's byte offset in the buffer
    const u64* send_at; // per job: where its sent lanes go, in bytes
    unsigned char* buf;
    u64 size;  // the buffer's bytes
};

void warp_copy_in(void* ctx, u32 j) {
    const WarpCopy& c = *static_cast<const WarpCopy*>(ctx);
    const WarpJob& job = c.jobs[j];
    const u32 wl = job.wl; // (its node's width: docs/inject-plan.md W1)
    NOVA_ASSERT(c.img_at[j] + static_cast<u64>(job.img_words) * 4 <= c.size);
    memcpy(c.buf + c.img_at[j], job.img, static_cast<usize>(job.img_words) * 4);
    const Lane* src = static_cast<const Lane*>(job.lanes);
    Lane* dst = reinterpret_cast<Lane*>(c.buf + c.send_at[j]);
    NOVA_ASSERT(c.send_at[j] + static_cast<u64>(job.nsend) * wl * sizeof(Lane) <= c.size);
    for (u32 i = 0; i < job.nsend; ++i, dst += wl) {
        NOVA_ASSERT(static_cast<u64>(job.send[i] + 1) * wl <= job.nlanes);
        memcpy(dst, src + static_cast<usize>(job.send[i]) * wl, wl * sizeof(Lane));
    }
}

// A job's round back, in parallel: its records' warp steps (the body's, its
// longest, and its largest record, for the stats and the split's model), and,
// if asked, the records copied back into its image. Only the records the GPU
// ran come back: the CPU threads may be running the node's others in the
// same image meanwhile (the per-body split).
struct WarpBack {
    const WarpJob* jobs;
    const u64* img_at; // per job: its image's word offset in the readback
    const u32* back;   // the readback
    bool copy;
    u64* out; // per job: steps, longest body's steps, largest record's words
};

void warp_back_task(void* ctx, u32 j) {
    const WarpBack& c = *static_cast<const WarpBack*>(ctx);
    const WarpJob& job = c.jobs[j];
    const u32* img = c.back + c.img_at[j];
    u64 steps = 0, longest = 0, rmax = 0;
    for (u32 r = 0; r < job.nrec; ++r) {
        const u32 words = warp_job_rec_words(job, r);
        const u32* rec = img + job.recs[r];
        if (c.copy)
            memcpy(job.img + job.recs[r], rec, static_cast<usize>(words) * 4);
        u64 body = 0;
        for (u32 i = 0; i < rec[WB_NWARP]; ++i)
            body += rec[WB_WARPS + i * WW_SIZE + WW_STEPS];
        steps += body;
        longest = body > longest ? body : longest;
        rmax = words > rmax ? words : rmax;
    }
    c.out[3 * j] = steps;
    c.out[3 * j + 1] = longest;
    c.out[3 * j + 2] = rmax;
}

} // namespace

bool WarpGpu::submit(const u32* cst, u32 ncst, const WarpJob* jobs, u32 njobs, TextBuf* err, ThreadPool* pool,
                     u32 batch) {
    if (!impl_ || batch >= kBatches || impl_->batch[batch].busy)
        return false;
    Impl* m = impl_;
    WarpBatch& B = m->batch[batch];
    Queue& Q = *B.q;
    const double t_sub = clock_seconds();
    if (B.rb_map) { // the last round's results, read in place, are done with
        D3D12_RANGE none = {0, 0};
        B.rb->Unmap(0, &none);
        B.rb_map = nullptr;
    }
    B.jobs = jobs;
    B.njobs = njobs;
    B.grew = false;
    // Each job's image, its node's width and room for lanes, and where its
    // sent warps' lanes go in the round's list (in lanes: each node's warps
    // are its own width, docs/inject-plan.md W1).
    B.img_at.clear();
    B.send_at.clear();
    u64 img_words = 0, nrec = 0, sent = 0, sent_lanes = 0;
    bool grow = m->heap == nullptr;
    for (u32 j = 0; j < njobs; ++j) {
        const u32 wl = jobs[j].wl;
        if (wl == 0 || wl > kMaxLanes || (wl & (wl - 1)) || !m->width[width_index(wl)].pso) {
            say(err, "a node's warps have a width the GPU has no kernel for");
            return false;
        }
        B.img_at.push(img_words);
        B.send_at.push(sent_lanes);
        img_words += jobs[j].img_words;
        nrec += jobs[j].nrec;
        sent += jobs[j].nsend;
        sent_lanes += static_cast<u64>(jobs[j].nsend) * wl;
        const u32 node = jobs[j].node;
        if (node >= m->rcap.size()) {
            const usize was = m->rcap.size();
            m->rcap.resize(node + 1);
            m->rbase.resize(node + 1);
            m->nwl.resize(node + 1);
            for (usize k = was; k <= node; ++k)
                m->rcap[k] = m->rbase[k] = m->nwl[k] = 0;
        }
        m->nwl[node] = wl;
        grow = grow || jobs[j].nlanes > m->rcap[node];
    }
    B.img_words = img_words;
    B.nrec = nrec;
    B.sent = sent;
    B.sent_lanes = sent_lanes;
    if (nrec == 0)
        return true;
    if (img_words > 0x3FFFFFFFull) {
        say(err, "warp round too big for one dispatch");
        return false;
    }
    ID3D12GraphicsCommandList* L = Q.list;
    // A node that outgrew its room: a new heap with room for twice what each
    // node needs, every node's lanes copied over on the GPU. The copy is every
    // node's, so it waits for a batch still running (which writes the heap),
    // and a batch sent while it is running waits for it (rare: rooms double);
    // their gathered lanes are found by their own copies of the layout.
    D3D12_RESOURCE_STATES heap_state = D3D12_RESOURCE_STATE_COMMON;
    for (u32 o = 0; o < kBatches; ++o)
        if (o != batch && m->batch[o].busy && (grow || m->batch[o].grew))
            Q.after(*m->batch[o].q);
    if (grow) {
        B.grew = true;
        Array<u64> cap, base;
        cap.resize(m->rcap.size());
        base.resize(m->rcap.size());
        for (usize k = 0; k < cap.size(); ++k)
            cap[k] = m->rcap[k];
        for (u32 j = 0; j < njobs; ++j) {
            u64& c = cap[jobs[j].node];
            const u64 least = 64ull * jobs[j].wl;
            if (jobs[j].nlanes > c)
                c = static_cast<u64>(jobs[j].nlanes) * 2 > least ? static_cast<u64>(jobs[j].nlanes) * 2 : least;
        }
        u64 total = 0;
        for (usize k = 0; k < cap.size(); ++k) {
            base[k] = total;
            total += cap[k];
        }
        ID3D12Resource* nh = Q.buffer((total ? total : 1) * sizeof(Lane), D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_COMMON, true);
        if (!nh) {
            say(err, "the warp lanes' heap could not be made (out of video memory?)");
            return false;
        }
        if (m->heap) {
            Q.barrier(m->heap, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Q.barrier(nh, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            for (usize k = 0; k < m->rcap.size(); ++k)
                if (m->rcap[k])
                    L->CopyBufferRegion(nh, base[k] * sizeof(Lane), m->heap, m->rbase[k] * sizeof(Lane),
                                        m->rcap[k] * sizeof(Lane));
            heap_state = D3D12_RESOURCE_STATE_COPY_DEST;
            release(B.old_heap);
            B.old_heap = m->heap;
        }
        m->heap = nh;
        m->heap_lanes = total;
        for (usize k = 0; k < cap.size(); ++k) {
            m->rcap[k] = cap[k];
            m->rbase[k] = base[k];
        }
    }
    B.rbase.assign(m->rbase.data(), m->rbase.size());
    // The table: per record its node's image base, the record, its node's
    // lanes' base, its size. Its kind is the smallest groupshared tier that
    // holds it, else the plain kernel; each kind is a dispatch, the tiers
    // first, records in job order. long_first: the records whose expected
    // chain is over a quarter of the longest go first, in dispatches of
    // their own, longest first.
    if (nrec > 0xFFFFFFull) {
        say(err, "warp round has too many records");
        return false;
    }
    u32 cmax = 0; // the longest expected chain
    if (m->long_first)
        for (u32 j = 0; j < njobs; ++j)
            for (u32 r = 0; r < jobs[j].nrec && jobs[j].rec_chain; ++r)
                cmax = jobs[j].rec_chain[r] > cmax ? jobs[j].rec_chain[r] : cmax;
    // A record's kind is its node's width (docs/inject-plan.md W3) and its
    // tier there: width index x 8 + tier (each width's kernels are their own
    // dispatches). The order is the keys' (long ? 0 : 1) << 63 | kind << 56 |
    // (~chain if long) << 24 | entry, ascending. Most records aren't long, and
    // their keys are already in entry order within their kind, so only the
    // long ones are sorted; the short ones go by kind in entry order (a
    // counting pass). The same order a sort of every key gives (2026-11-08:
    // sorting all of a mature round's 160,000 took 2.5 ms on one thread,
    // before both the GPU and the CPU threads could start).
    B.keys.clear();
    B.ents.resize(nrec * 4);
    B.kinds.resize(nrec);
    constexpr u32 kKinds = kWidths * 8;
    u64 per_kind[kKinds] = {}; // short records of each kind
    for (u32 j = 0, e = 0; j < njobs; ++j) {
        const u32 wi = width_index(jobs[j].wl);
        const WarpWidth& W = m->width[wi];
        for (u32 r = 0; r < jobs[j].nrec; ++r, ++e) {
            const u32 size = warp_job_rec_words(jobs[j], r);
            u32 tier = 0;
            while (tier < W.ntier && size > W.tier[tier])
                ++tier;
            const u32 kind = wi * 8 + tier;
            const u32 chain = jobs[j].rec_chain ? jobs[j].rec_chain[r] : 0;
            const bool lng = cmax && static_cast<u64>(chain) * 4 > cmax;
            if (lng)
                B.keys.push(static_cast<u64>(kind) << 56 | static_cast<u64>(0xFFFFFFFFu - chain) << 24 | e);
            else
                ++per_kind[kind];
            B.kinds[e] = static_cast<u8>(lng ? 0xFF : kind);
            B.ents[4 * e + 0] = static_cast<u32>(B.img_at[j]);
            B.ents[4 * e + 1] = jobs[j].recs[r];
            B.ents[4 * e + 2] = static_cast<u32>(m->rbase[jobs[j].node]);
            B.ents[4 * e + 3] = size;
        }
    }
    sort_u64(B.keys.data(), B.keys.size()); // (the long ones: by kind, longest chain first, then entry)
    const usize nlong = B.keys.size();
    u64 kind_at[kKinds] = {}; // where each kind's short records start, after the long ones
    for (u32 k = 0, at = static_cast<u32>(nlong); k < kKinds; ++k) {
        kind_at[k] = at;
        at += static_cast<u32>(per_kind[k]);
    }
    B.keys.resize(nrec);
    for (u64 e = 0; e < nrec; ++e) {
        const u8 k = B.kinds[e];
        if (k != 0xFF)
            B.keys[kind_at[k]++] = 1ull << 63 | static_cast<u64>(k) << 56 | e;
    }
    B.table.resize(nrec * 4);
    B.runs.clear(); // (kind, first entry, count) a dispatch
    for (usize i = 0; i < B.keys.size(); ++i) {
        const u64 e = B.keys[i] & 0xFFFFFFu;
        memcpy(&B.table[4 * i], &B.ents[4 * e], 16);
        const u32 group = static_cast<u32>(B.keys[i] >> 56); // long and kind
        if (B.runs.empty() || B.runs[B.runs.size() - 3] != group) {
            B.runs.push(group);
            B.runs.push(static_cast<u32>(i));
            B.runs.push(0);
        }
        ++B.runs[B.runs.size() - 1];
    }
    // Each sent warp, for the lane kernel: where its lanes go in the heap,
    // where they are in the round's list, and how many (its node's width).
    B.at.resize(sent ? 3 * sent : 3);
    for (u32 j = 0, k = 0; j < njobs; ++j) {
        const u32 wl = jobs[j].wl;
        for (u32 i = 0; i < jobs[j].nsend; ++i, ++k) {
            B.at[3 * k] = static_cast<u32>(m->rbase[jobs[j].node] + static_cast<u64>(jobs[j].send[i]) * wl);
            B.at[3 * k + 1] = static_cast<u32>(B.send_at[j] + static_cast<u64>(i) * wl);
            B.at[3 * k + 2] = wl;
        }
    }
    const u64 cst_b = align256(static_cast<u64>(ncst) * 4), tab_b = align256(nrec * 16);
    const u64 img_b = align256((img_words ? img_words : 1) * 4);
    const u64 send_b = align256((sent_lanes ? sent_lanes : 1) * sizeof(Lane)), at_b = align256((sent ? sent : 1) * 12);
    const u64 zero_at = cst_b + tab_b + img_b + send_b + at_b; // 256 zero bytes (the gather's count)
    // The gather (waiting warps' lanes back with the round): its capacity in
    // warps, and the most warps a record holds.
    u32 wmax = 0;
    for (u32 j = 0; j < njobs; ++j)
        for (u32 r = 0; r < jobs[j].nrec; ++r) {
            const u32 nw = jobs[j].img[jobs[j].recs[r] + WB_NWARP];
            wmax = nw > wmax ? nw : wmax;
        }
    B.gathered = false;
    B.gcap_round = m->pso_gather && wmax ? B.gcap : 0;
    B.glcap_round = B.gcap_round ? B.glcap : 0; // (room in lanes: each warp its own width)
    const u64 g_bytes = align256((2ull + 2ull * B.gcap_round + static_cast<u64>(B.glcap_round) * 29) * 4);
    if (!ensure_buffer(Q, B.up, B.up_cap, zero_at + 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                       false) ||
        !ensure_buffer(Q, B.img, B.img_cap, img_b, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true) ||
        !ensure_buffer(Q, B.rb, B.rb_cap, img_b, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false) ||
        (B.gcap_round &&
         (!ensure_buffer(Q, B.gout, B.gout_cap, g_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true) ||
          !ensure_buffer(Q, B.grb, B.grb_cap, g_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                         false)))) {
        say(err, "warp buffers could not be made (out of video memory?)");
        return false;
    }
    const double t_in = clock_seconds();
    B.prep_s = t_in - t_sub;
    unsigned char* up = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(B.up->Map(0, &none, reinterpret_cast<void**>(&up)))) {
        say(err, "warp upload buffer would not map");
        return false;
    }
    memcpy(up, cst, static_cast<usize>(ncst) * 4);
    memcpy(up + cst_b, B.table.data(), nrec * 16);
    memcpy(up + cst_b + tab_b + img_b + send_b, B.at.data(), sent * 12);
    memset(up + zero_at, 0, 256);
    Array<u64> img_off, send_off;
    for (u32 j = 0; j < njobs; ++j) {
        img_off.push(cst_b + tab_b + B.img_at[j] * 4);
        send_off.push(cst_b + tab_b + img_b + B.send_at[j] * sizeof(Lane));
    }
    WarpCopy cin = {jobs, img_off.data(), send_off.data(), up, B.up_cap};
    if (pool)
        pool->run(njobs, &warp_copy_in, &cin);
    else
        for (u32 j = 0; j < njobs; ++j)
            warp_copy_in(&cin, j);
    B.up->Unmap(0, nullptr);
    B.in_s = clock_seconds() - t_in;

    const bool stamps = m->freq != 0;
    const D3D12_GPU_VIRTUAL_ADDRESS va = B.up->GetGPUVirtualAddress();
    if (stamps)
        L->EndQuery(B.qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    Q.barrier(m->heap, heap_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Q.barrier(B.img, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    if (img_words)
        L->CopyBufferRegion(B.img, 0, B.up, cst_b + tab_b, img_words * 4);
    Q.barrier(B.img, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    L->SetComputeRootSignature(m->root);
    if (sent) { // the host's changed lanes into the heap
        L->SetPipelineState(m->pso_move);
        L->SetComputeRootShaderResourceView(0, va + cst_b + tab_b + img_b + send_b);
        L->SetComputeRootShaderResourceView(1, va + cst_b + tab_b + img_b);
        L->SetComputeRootUnorderedAccessView(2, m->heap->GetGPUVirtualAddress());
        L->SetComputeRootUnorderedAccessView(3, m->heap->GetGPUVirtualAddress());
        m->dispatch_rows(L, sent, static_cast<u32>(sent), 0, 0); // a group a warp (each its own width)
        Q.uav_barrier(m->heap);
    }
    if (stamps)
        L->EndQuery(B.qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    L->SetComputeRootShaderResourceView(0, va);
    L->SetComputeRootUnorderedAccessView(2, B.img->GetGPUVirtualAddress());
    L->SetComputeRootUnorderedAccessView(3, m->heap->GetGPUVirtualAddress());
    // The dispatches touch different records: no barrier between them.
    for (usize d = 0; d + 2 < B.runs.size(); d += 3) {
        const u32 kind = B.runs[d] & 0x7Fu; // (its width index x 8 + its tier)
        const WarpWidth& W = m->width[kind >> 3];
        const u32 tier = kind & 7u;
        const u64 first = B.runs[d + 1], count = B.runs[d + 2];
        L->SetPipelineState(tier < W.ntier ? W.pso_tier[tier] : W.pso);
        L->SetComputeRootShaderResourceView(1, va + cst_b + first * 16);
        const u32 row = static_cast<u32>(count < 65535 ? count : 65535);
        const u32 rows = static_cast<u32>((count + row - 1) / row);
        const u32 k[4] = {static_cast<u32>(count), row, 0, 0};
        L->SetComputeRoot32BitConstants(4, 4, k, 0);
        L->Dispatch(row, rows, 1);
    }
    if (stamps)
        L->EndQuery(B.qh, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    D3D12_RESOURCE_STATES img_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (B.gcap_round) { // the waiting warps' lanes, gathered to come back with the round
        Q.uav_barrier(B.img);
        Q.uav_barrier(m->heap);
        Q.barrier(B.img, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        img_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Q.barrier(B.gout, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        L->CopyBufferRegion(B.gout, 0, B.up, zero_at, 8); // the counts start at 0
        Q.barrier(B.gout, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        L->SetPipelineState(m->pso_gather);
        L->SetComputeRootShaderResourceView(0, va + cst_b);
        L->SetComputeRootShaderResourceView(1, B.img->GetGPUVirtualAddress());
        L->SetComputeRootUnorderedAccessView(2, m->heap->GetGPUVirtualAddress());
        L->SetComputeRootUnorderedAccessView(3, B.gout->GetGPUVirtualAddress());
        const u32 ry = nrec < 65535 ? static_cast<u32>(nrec) : 65535u; // records: y, then z
        const u32 rz = static_cast<u32>((nrec + ry - 1) / ry);
        const u32 k[4] = {static_cast<u32>(nrec), B.gcap_round, B.glcap_round, ry};
        L->SetComputeRoot32BitConstants(4, 4, k, 0);
        L->Dispatch(wmax, ry, rz);
        Q.barrier(B.gout, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        L->CopyBufferRegion(B.grb, 0, B.gout, 0, g_bytes);
    }
    Q.barrier(B.img, img_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (img_words)
        L->CopyBufferRegion(B.rb, 0, B.img, 0, img_words * 4);
    if (stamps) {
        L->EndQuery(B.qh, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        L->ResolveQueryData(B.qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, B.qrb, 0);
    }
    B.t_exec = clock_seconds();
    if (!Q.exec(err))
        return false;
    B.post_s = clock_seconds() - t_in - B.in_s;
    B.busy = true;
    return true;
}

const u32* WarpGpu::result(u32 j, u32 batch) const {
    if (!impl_ || batch >= kBatches)
        return nullptr;
    const WarpBatch& B = impl_->batch[batch];
    return B.rb_map && j < B.njobs ? B.rb_map + B.img_at[j] : nullptr;
}

bool WarpGpu::finish(TextBuf* err, ThreadPool* pool, u32 batch, bool copy_back) {
    if (!impl_ || batch >= kBatches)
        return false;
    Impl* m = impl_;
    WarpBatch& B = m->batch[batch];
    Queue& Q = *B.q;
    last_ms_ = last_kernel_ms_[batch] = 0;
    last_longest_[batch] = 0;
    if (!B.busy)
        return B.nrec == 0 || B.njobs == 0 ? true : false;
    B.busy = false;
    if (!Q.wait(err)) {
        dump_messages(Q.dev);
        return false;
    }
    release(B.old_heap);
    const double gpu_s = clock_seconds() - B.t_exec;
    const double t_out = clock_seconds();
    void* mapped = nullptr;
    const u64 img_b = align256((B.img_words ? B.img_words : 1) * 4);
    D3D12_RANGE all = {0, static_cast<SIZE_T>(img_b)};
    if (FAILED(B.rb->Map(0, &all, &mapped))) {
        say(err, "warp readback would not map");
        return false;
    }
    const u32* back = static_cast<const u32*>(mapped);
    B.back_out.resize(static_cast<usize>(B.njobs) * 3);
    WarpBack wb = {B.jobs, B.img_at.data(), back, copy_back, B.back_out.data()};
    if (pool)
        pool->run(B.njobs, &warp_back_task, &wb);
    else
        for (u32 j = 0; j < B.njobs; ++j)
            warp_back_task(&wb, j);
    D3D12_RANGE none = {0, 0};
    if (B.gcap_round) { // the gathered lanes are this round's; the next capacities follow the counts
        u32 count = 0, lanes = 0;
        void* gm = nullptr;
        D3D12_RANGE first = {0, 8};
        if (SUCCEEDED(B.grb->Map(0, &first, &gm))) {
            count = static_cast<const u32*>(gm)[0];
            lanes = static_cast<const u32*>(gm)[1];
            B.grb->Unmap(0, &none);
            B.gathered = true;
        }
        const u64 want = static_cast<u64>(count) * 5 / 4 + 64;
        B.gcap = static_cast<u32>(want < 256 ? 256 : (want > 8192 ? 8192 : want));
        // (room in lanes: from 8,192, at most 2,097,152, 243 MB of lanes)
        const u64 lwant = static_cast<u64>(lanes) * 5 / 4 + 2048;
        B.glcap = static_cast<u32>(lwant < 8192 ? 8192 : (lwant > 2097152 ? 2097152 : lwant));
    }
    u64 longest = 0; // the longest body's steps: a group runs its warps one after another
    stats_.img_words += B.img_words;
    for (u32 j = 0; j < B.njobs; ++j) {
        const u64* o = B.back_out.data() + 3 * static_cast<usize>(j);
        stats_.steps += o[0];
        longest = o[1] > longest ? o[1] : longest;
        stats_.rec_words_max = o[2] > stats_.rec_words_max ? o[2] : stats_.rec_words_max;
    }
    if (copy_back)
        B.rb->Unmap(0, &none);
    else
        B.rb_map = back;
    stats_.steps_longest += longest;
    last_longest_[batch] = longest;
    if (m->freq) {
        u64 ts[4] = {};
        if (read_back(B.qrb, ts, sizeof ts) && ts[3] >= ts[0]) {
            const double tk = 1000.0 / static_cast<double>(m->freq);
            stats_.ms_dev_up += (ts[1] - ts[0]) * tk;
            stats_.ms_dev_kernel += (ts[2] - ts[1]) * tk;
            stats_.ms_dev_down += (ts[3] - ts[2]) * tk;
            last_ms_ = (ts[3] - ts[0]) * tk;
            last_kernel_ms_[batch] = (ts[2] - ts[1]) * tk;
            last_span_[batch][0] = ts[0];
            last_span_[batch][1] = ts[3];
        }
    }
    if (last_ms_ == 0)
        last_ms_ = gpu_s * 1000.0;
    const u64 lane_bytes = B.sent_lanes * sizeof(Lane);
    ++stats_.rounds;
    stats_.records += B.nrec;
    stats_.lanes_sent += B.sent;
    stats_.bytes_up += B.img_words * 4 + lane_bytes;
    stats_.bytes_down += B.img_words * 4;
    stats_.ms_in += B.in_s * 1000.0;
    stats_.ms_prep += B.prep_s * 1000.0;
    stats_.ms_post += B.post_s * 1000.0;
    stats_.ms_gpu += gpu_s * 1000.0;
    stats_.ms_out += (clock_seconds() - t_out) * 1000.0;
    return true;
}

u32 WarpGpu::take_gathered(const WarpFetch* list, u32 n, u8* got, ThreadPool* pool, u32 batch) {
    for (u32 i = 0; i < n; ++i)
        got[i] = 0;
    if (!impl_ || batch >= kBatches || impl_->batch[batch].busy || !impl_->batch[batch].gathered || n == 0)
        return 0;
    Impl* m = impl_;
    WarpBatch& B = m->batch[batch];
    B.gathered = false; // once a round
    const u32 cap = B.gcap_round, lcap = B.glcap_round;
    void* mapped = nullptr;
    D3D12_RANGE head = {0, static_cast<SIZE_T>((2ull + 2ull * cap) * 4)};
    if (FAILED(B.grb->Map(0, &head, &mapped)))
        return 0;
    const u32* out = static_cast<const u32*>(mapped);
    const u32 ng = out[0] < cap ? out[0] : cap;
    const u32 nl = out[1] < lcap ? out[1] : lcap; // (the lanes' room in use, at most)
    Array<u64> keys; // (heap lane index << 32 | slot), sorted: the slots that got room for their lanes
    keys.reserve(ng);
    for (u32 k = 0; k < ng; ++k)
        if (out[3 + 2 * k] != 0xFFFFFFFFu)
            keys.push((static_cast<u64>(out[2 + 2 * k]) << 32) | k);
    Array<u32> first; // each slot's first lane in the lanes (none: no room)
    first.resize(ng);
    for (u32 k = 0; k < ng; ++k)
        first[k] = out[3 + 2 * k];
    D3D12_RANGE none = {0, 0};
    B.grb->Unmap(0, &none);
    if (keys.empty())
        return 0;
    sort_u64(keys.data(), keys.size());
    D3D12_RANGE all = {0, static_cast<SIZE_T>((2ull + 2ull * cap + static_cast<u64>(nl) * 29) * 4)};
    if (FAILED(B.grb->Map(0, &all, &mapped)))
        return 0;
    out = static_cast<const u32*>(mapped);
    u32 taken = 0;
    Array<u64>& pairs = B.gpairs; // (gathered slot << 32 | list index), then copied
    pairs.clear();
    for (u32 i = 0; i < n; ++i) { // (the heap as this round saw it: another batch may have grown it since)
        if (list[i].node >= B.rbase.size() || list[i].node >= m->nwl.size())
            continue;
        const u64 at = B.rbase[list[i].node] + static_cast<u64>(list[i].slot) * m->nwl[list[i].node];
        usize lo = 0, hi = keys.size();
        while (lo < hi) { // the first key >= at << 32
            const usize mid = (lo + hi) / 2;
            if ((keys[mid] >> 32) < at)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == keys.size() || (keys[lo] >> 32) != at)
            continue;
        pairs.push((keys[lo] << 32) | i);
        got[i] = 1;
        ++taken;
    }
    // The copying (about 7 MB a round in a late landscape), spread over the
    // threads in chunks.
    struct Copy {
        const WarpFetch* list;
        const u64* pairs;
        usize n;
        const u32* src;   // the lanes
        const u32* first; // each slot's first lane in them
        const u32* nwl;   // each node's lanes a warp (a warp's own lanes)
        static void task(void* ctx, u32 c) {
            const Copy& k = *static_cast<const Copy*>(ctx);
            const usize a = static_cast<usize>(c) * 64, b = a + 64 < k.n ? a + 64 : k.n;
            for (usize j = a; j < b; ++j) {
                const u64 p = k.pairs[j];
                const WarpFetch& f = k.list[p & 0xFFFFFFFFu];
                memcpy(f.dst, k.src + static_cast<u64>(k.first[p >> 32]) * 29,
                       static_cast<usize>(k.nwl[f.node]) * sizeof(Lane));
            }
        }
    };
    Copy copy = {list, pairs.data(), pairs.size(), out + 2 + 2 * static_cast<u64>(cap), first.data(), m->nwl.data()};
    const u32 chunks = static_cast<u32>((pairs.size() + 63) / 64);
    if (pool && chunks > 1)
        pool->run(chunks, &Copy::task, &copy);
    else
        for (u32 c = 0; c < chunks; ++c)
            Copy::task(&copy, c);
    B.grb->Unmap(0, &none);
    stats_.lanes_gathered += taken;
    stats_.bytes_down += static_cast<u64>(nl) * sizeof(Lane);
    return taken;
}

bool WarpGpu::fetch(const WarpFetch* list, u32 n, TextBuf* err, u32 batch) {
    if (!impl_ || batch >= kBatches || impl_->batch[batch].busy)
        return false;
    if (n == 0)
        return true;
    Impl* m = impl_;
    Queue& Q = *m->batch[batch].q; // (its queue: the other batch may be running meanwhile)
    const double t0 = clock_seconds();
    // Each warp for the lane kernel: its lanes in the heap, where they go in
    // the staging list, how many (its node's width: docs/inject-plan.md W1).
    m->fat.resize(static_cast<usize>(n) * 3);
    u64 lanes = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 node = list[i].node;
        const u32 wl = node < m->nwl.size() ? m->nwl[node] : 0;
        if (node >= m->rcap.size() || wl == 0 || static_cast<u64>(list[i].slot + 1) * wl > m->rcap[node]) {
            say(err, "fetching lanes the GPU never had");
            return false;
        }
        m->fat[3 * i] = static_cast<u32>(m->rbase[node] + static_cast<u64>(list[i].slot) * wl);
        m->fat[3 * i + 1] = static_cast<u32>(lanes);
        m->fat[3 * i + 2] = wl;
        lanes += wl;
    }
    const u64 bytes = lanes * sizeof(Lane);
    if (!ensure_buffer(Q, m->fup, m->fup_cap, align256(static_cast<u64>(n) * 12), D3D12_HEAP_TYPE_UPLOAD,
                       D3D12_RESOURCE_STATE_GENERIC_READ, false) ||
        !ensure_buffer(Q, m->fstage, m->fstage_cap, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                       true) ||
        !ensure_buffer(Q, m->frb, m->frb_cap, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                       false)) {
        say(err, "fetch buffers could not be made");
        return false;
    }
    void* up = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(m->fup->Map(0, &none, &up))) {
        say(err, "fetch upload buffer would not map");
        return false;
    }
    memcpy(up, m->fat.data(), static_cast<usize>(n) * 12);
    m->fup->Unmap(0, nullptr);
    for (u32 o = 0; o < kBatches; ++o) // (a running batch's heap copy must land first)
        if (o != batch && m->batch[o].busy && m->batch[o].grew)
            Q.after(*m->batch[o].q);
    ID3D12GraphicsCommandList* L = Q.list;
    Q.barrier(m->heap, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Q.barrier(m->fstage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    L->SetComputeRootSignature(m->root);
    L->SetPipelineState(m->pso_move);
    const D3D12_GPU_VIRTUAL_ADDRESS va = m->fup->GetGPUVirtualAddress();
    L->SetComputeRootShaderResourceView(0, va);
    L->SetComputeRootShaderResourceView(1, va);
    L->SetComputeRootUnorderedAccessView(2, m->heap->GetGPUVirtualAddress());
    L->SetComputeRootUnorderedAccessView(3, m->fstage->GetGPUVirtualAddress());
    m->dispatch_rows(L, n, n, 0, 1); // a group a warp
    Q.barrier(m->fstage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    L->CopyBufferRegion(m->frb, 0, m->fstage, 0, bytes);
    if (!Q.exec(err) || !Q.wait(err)) {
        dump_messages(Q.dev);
        return false;
    }
    void* mapped = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(bytes)};
    if (FAILED(m->frb->Map(0, &all, &mapped))) {
        say(err, "fetch readback would not map");
        return false;
    }
    const Lane* src = static_cast<const Lane*>(mapped);
    for (u32 i = 0; i < n; ++i)
        memcpy(list[i].dst, src + m->fat[3 * i + 1], static_cast<usize>(m->fat[3 * i + 2]) * sizeof(Lane));
    m->frb->Unmap(0, &none);
    stats_.lanes_fetched += n;
    ++stats_.fetches;
    stats_.bytes_down += bytes;
    stats_.ms_fetch += (clock_seconds() - t0) * 1000.0;
    return true;
}

// --- the NPU backend: dot, matvec and matmul as float16 digit products -------
//
// DirectML on the NPU runs float16 matrix multiplies compiled as graphs, and
// is exact only while every sum stays below 2048 (the probe, C6). So each
// value u = codon & 63 (the signed value is u - 32) is split into digits
// u = 8h + l (0..7), sums are cut into chunks of 32, and each chunk needs the
// four digit products h.h, h.l, l.h and l.l (each sum at most 32 x 49 = 1568).
// The CPU puts them back together exactly:
//   sum (ua - 32)(ub - 32) = sum ua.ub - 32 sum ua - 32 sum ub + 1024 len,
//   sum ua.ub = 64 sum ha.hb + 8 sum (ha.lb + la.hb) + sum la.lb.
// One shape does all the work: an *item* is [128 x 32] x [32 x 128], rows
// 0..63 holding h digits and 64..127 l digits of up to 64 row vectors, and
// columns likewise for up to 64 column vectors, one chunk long:
//   matmul (k <= 64): item ch = chunk ch of the inner index, all k x k outputs;
//   matvec (k <= 256): item (row group, chunk) = 64 rows of M against v
//     (column 0);
//   dot: item t = chunks 64t.. of a (rows) and of b (columns), chunk cc
//     pairing row cc with column cc.
// The Intel NPU compiles batched multiplies only with the batch in the second
// dimension (or as a third), its compile time grows fast with the batch
// (`nova devices`: 32 items 74 ms, 1024 items 6 s), and its driver faults
// when a batched multiply executes (device removed, 2026-11-06). So the
// multiply is compiled for one item and dispatched once per item, from lists
// recorded once and replayed (see Slot). argmax has no multiplies and runs on
// the CPU, as does anything past kMaxItems.
namespace {

const u32 kChunk = 32;
const u32 kMaxItems = 256;  // items per submission (one dispatch each)
const u32 kChunkItems = 16; // items per recorded list
const u32 kChunkLists = kMaxItems / kChunkItems;
const u32 kIA = 128 * kChunk, kIB = kChunk * 128, kIO = 128 * 128; // halves per item

struct Gemm {
    IDMLCompiledOperator* op = nullptr;
    ID3D12Resource* persist = nullptr;
    u64 persist_size = 0, temp_size = 0;
    UINT descriptors = 0;
    void destroy() {
        release(op);
        release(persist);
        persist_size = temp_size = 0;
        descriptors = 0;
    }
};

// Compiles [batch, m x 32] x [batch, 32 x n] in float16 as a one-node graph
// and runs its initialization as its own submission (the NPU needs that).
bool gemm_build(Queue& q, IDMLDevice* dml, IDMLCommandRecorder* rec, u32 batch, u32 m, u32 n, Gemm* g,
                TextBuf* err, u32 layout = 1) {
    // layout 0: [batch, 1, m, k]; 1: [1, batch, m, k]; 2: three dimensions
    // [batch, m, k]. The Intel NPU compiles only 1 and 2 (`nova devices`), so
    // the backend uses 1.
    UINT as[4] = {batch, 1, m, kChunk}, bs[4] = {batch, 1, kChunk, n}, os[4] = {batch, 1, m, n};
    if (layout == 1) {
        as[0] = bs[0] = os[0] = 1;
        as[1] = bs[1] = os[1] = batch;
    }
    DML_BUFFER_TENSOR_DESC ta, tb, to;
    buffer_tensor(&ta, DML_TENSOR_DATA_TYPE_FLOAT16, as, 2);
    buffer_tensor(&tb, DML_TENSOR_DATA_TYPE_FLOAT16, bs, 2);
    buffer_tensor(&to, DML_TENSOR_DATA_TYPE_FLOAT16, os, 2);
    const UINT as3[3] = {batch, m, kChunk}, bs3[3] = {batch, kChunk, n}, os3[3] = {batch, m, n};
    if (layout == 2) {
        ta.DimensionCount = tb.DimensionCount = to.DimensionCount = 3;
        ta.Sizes = as3;
        tb.Sizes = bs3;
        to.Sizes = os3;
    }
    DML_TENSOR_DESC da = {DML_TENSOR_TYPE_BUFFER, &ta}, db = {DML_TENSOR_TYPE_BUFFER, &tb},
                    dout = {DML_TENSOR_TYPE_BUFFER, &to};
    DML_GEMM_OPERATOR_DESC gemm = {};
    gemm.ATensor = &da;
    gemm.BTensor = &db;
    gemm.OutputTensor = &dout;
    gemm.TransA = DML_MATRIX_TRANSFORM_NONE;
    gemm.TransB = DML_MATRIX_TRANSFORM_NONE;
    gemm.Alpha = 1.0f;
    gemm.Beta = 0.0f;
    const DML_OPERATOR_DESC od = {DML_OPERATOR_GEMM, &gemm};
    IDMLOperator* op = nullptr;
    IDMLDevice1* dml1 = nullptr;
    IDMLOperatorInitializer* init = nullptr;
    IDMLBindingTable* table = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* temp = nullptr;
    bool ok = false;
    HRESULT hr;
    do {
        if (FAILED(hr = dml->CreateOperator(&od, IID_PPV_ARGS(&op))) ||
            FAILED(hr = dml->QueryInterface(IID_PPV_ARGS(&dml1)))) {
            say_hr(err, "multiply refused", hr);
            break;
        }
        DML_OPERATOR_GRAPH_NODE_DESC node = {op, "mm"};
        DML_GRAPH_NODE_DESC nodes[1] = {{DML_GRAPH_NODE_TYPE_OPERATOR, &node}};
        DML_INPUT_GRAPH_EDGE_DESC in0 = {0, 0, 0, "a"}, in1 = {1, 0, 1, "b"};
        DML_OUTPUT_GRAPH_EDGE_DESC out0 = {0, 0, 0, "out"};
        DML_GRAPH_EDGE_DESC ins[2] = {{DML_GRAPH_EDGE_TYPE_INPUT, &in0}, {DML_GRAPH_EDGE_TYPE_INPUT, &in1}};
        DML_GRAPH_EDGE_DESC outs[1] = {{DML_GRAPH_EDGE_TYPE_OUTPUT, &out0}};
        DML_GRAPH_DESC gd = {};
        gd.InputCount = 2;
        gd.OutputCount = 1;
        gd.NodeCount = 1;
        gd.Nodes = nodes;
        gd.InputEdgeCount = 2;
        gd.InputEdges = ins;
        gd.OutputEdgeCount = 1;
        gd.OutputEdges = outs;
        if (FAILED(hr = dml1->CompileGraph(&gd, DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
                                           IID_PPV_ARGS(&g->op)))) {
            say_hr(err, "multiply graph failed to compile", hr);
            break;
        }
        IDMLCompiledOperator* ops[] = {g->op};
        if (FAILED(hr = dml->CreateOperatorInitializer(1, ops, IID_PPV_ARGS(&init)))) {
            say_hr(err, "initializer failed", hr);
            break;
        }
        const DML_BINDING_PROPERTIES ip = init->GetBindingProperties(), ep = g->op->GetBindingProperties();
        g->descriptors = ep.RequiredDescriptorCount ? ep.RequiredDescriptorCount : 1;
        g->temp_size = ep.TemporaryResourceSize;
        g->persist_size = ep.PersistentResourceSize;
        const UINT idesc = ip.RequiredDescriptorCount ? ip.RequiredDescriptorCount : 1;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, idesc,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(hr = q.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
            say_hr(err, "descriptor heap failed", hr);
            break;
        }
        DML_BINDING_TABLE_DESC btd = {init, heap->GetCPUDescriptorHandleForHeapStart(),
                                      heap->GetGPUDescriptorHandleForHeapStart(), idesc};
        if (FAILED(hr = dml->CreateBindingTable(&btd, IID_PPV_ARGS(&table)))) {
            say_hr(err, "binding table failed", hr);
            break;
        }
        if (ip.TemporaryResourceSize) {
            temp = q.buffer(ip.TemporaryResourceSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            true);
            if (!temp) {
                say(err, "buffer allocation failed");
                break;
            }
            DML_BUFFER_BINDING tbb = {temp, 0, ip.TemporaryResourceSize};
            DML_BINDING_DESC tdsc = {DML_BINDING_TYPE_BUFFER, &tbb};
            table->BindTemporaryResource(&tdsc);
        }
        if (g->persist_size) {
            g->persist = q.buffer(g->persist_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            if (!g->persist) {
                say(err, "buffer allocation failed");
                break;
            }
            DML_BUFFER_BINDING pbb = {g->persist, 0, g->persist_size};
            DML_BINDING_DESC pdsc = {DML_BINDING_TYPE_BUFFER, &pbb};
            table->BindOutputs(1, &pdsc);
        }
        ID3D12DescriptorHeap* heaps[] = {heap};
        q.list->SetDescriptorHeaps(1, heaps);
        rec->RecordDispatch(q.list, init, table);
        q.uav_barrier(nullptr);
        if (!q.run(err))
            break;
        ok = true;
    } while (false);
    release(op);
    release(dml1);
    release(init);
    release(table);
    release(heap);
    release(temp);
    if (!ok)
        g->destroy();
    return ok;
}

bool grow(Queue& q, ID3D12Resource*& res, u64& cap, u64 need, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
          bool uav) {
    if (res && cap >= need)
        return true;
    release(res);
    u64 size = cap ? cap : 65536;
    while (size < need)
        size *= 2;
    res = q.buffer(size, heap, state, uav);
    cap = res ? size : 0;
    return res != nullptr;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

void uav_all(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    list->ResourceBarrier(1, &b);
}

u8 sat_codon64(i64 acc) {
    i64 q = acc >> 5;
    q = q < -32 ? -32 : (q > 31 ? 31 : q);
    return static_cast<u8>(q + 32);
}

u32 chunks(u32 len) { return (len + kChunk - 1) / kChunk; }
u32 groups(u32 rows) { return (rows + 63) / 64; }

// Items an op needs; argmax, empty ops and any too big for an item's shape
// need none (the CPU does them).
u32 items_of(const AccelOp& op) {
    const u32 k = op.n;
    if (k == 0)
        return 0;
    if (op.kind == kAccDot)
        return (chunks(k) + 63) / 64;
    if (op.kind == kAccMatvec)
        return k <= 256 ? groups(k) * chunks(k) : 0;
    if (op.kind == kAccMatmul)
        return k <= 64 ? chunks(k) : 0;
    return 0;
}

} // namespace

// Does device `index` compile (and initialize) one batched float16 multiply
// shape, and how long does that take?
bool accel_probe_shape(u32 index, u32 batch, u32 m, u32 n, u32 layout, double* ms) {
    Found found[16];
    const u32 count = enumerate(found, 16);
    if (index >= count) {
        release_all(found, count);
        return false;
    }
    Queue q;
    IDMLDevice* dml = nullptr;
    IDMLCommandRecorder* rec = nullptr;
    TextBuf err;
    bool ok = q.init(found[index].adapter, found[index].d.gpu != 0, &err);
    release_all(found, count);
    ok = ok && SUCCEEDED(DMLCreateDevice(q.dev, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml))) &&
         SUCCEEDED(dml->CreateCommandRecorder(IID_PPV_ARGS(&rec)));
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Gemm g;
    ok = ok && gemm_build(q, dml, rec, batch, m, n, &g, &err, layout);
    QueryPerformanceCounter(&t1);
    if (ms)
        *ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    g.destroy();
    release(rec);
    release(dml);
    q.destroy();
    return ok;
}

struct NpuBackend::Impl {
    Queue q;
    IDMLDevice* dml = nullptr;
    IDMLCommandRecorder* rec = nullptr;
    char name[128] = {};
    bool broken = false;
    const char* why = ""; // the last failure
    u16 digit[8] = {};    // float16 0..7
    Gemm g;               // one item: [128 x 32] x [32 x 128]
    struct OpRec {
        u32 kind, n;
        u32 npu;    // 1: on the NPU; 0: done on the CPU at submit
        u32 item;   // its first item
        u32 aux;    // its corrections in aux
        u32 out_off, out_n;
        u32 scalar; // CPU ops: the result
    };
    // A slot's buffers hold kMaxItems items at fixed places, so the lists
    // that dispatch them are recorded once (on first use, a chunk at a time)
    // and replayed: recording and closing a DirectML dispatch costs the NPU
    // driver about 1 ms of CPU, replaying it about 13 us. Only the small
    // copy-in and copy-out lists are recorded per submission.
    struct Slot {
        bool ready = false; // buffers made
        ID3D12CommandAllocator *alloc = nullptr, *chunk_alloc = nullptr;
        ID3D12GraphicsCommandList *pre = nullptr, *post = nullptr;
        ID3D12GraphicsCommandList* chunk[kChunkLists] = {};
        ID3D12Resource *up = nullptr, *ina = nullptr, *inb = nullptr, *outb = nullptr, *rb = nullptr, *temp = nullptr;
        ID3D12DescriptorHeap* heap = nullptr;
        IDMLBindingTable* table = nullptr;
        u64 fence = 0;
        bool busy = false;
        u32 items = 0;
        Array<OpRec> ops;
        Array<i64> aux;
        Array<u8> pre_out; // CPU ops' output codons, at their out_off
    } slots[kAccelSlots];

    // The slot's buffers and descriptor heap, at full size.
    bool make_slot(Slot& sl) {
        const u64 a = kMaxItems * 2ull * kIA, b = kMaxItems * 2ull * kIB, o = kMaxItems * 2ull * kIO;
        sl.up = q.buffer(a + b, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        sl.ina = q.buffer(a, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
        sl.inb = q.buffer(b, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
        sl.outb = q.buffer(o, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
        sl.rb = q.buffer(o, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (g.temp_size)
            sl.temp = q.buffer(g.temp_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxItems * g.descriptors,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (!sl.up || !sl.ina || !sl.inb || !sl.outb || !sl.rb || (g.temp_size && !sl.temp) ||
            FAILED(q.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sl.heap)))) {
            why = "buffer allocation failed";
            return false;
        }
        sl.ready = true;
        return true;
    }

    // Records chunk c's list: one dispatch per item, each with its own
    // descriptors and its item's place in the buffers.
    bool record_chunk(Slot& sl, u32 c) {
        ID3D12GraphicsCommandList* L = nullptr;
        if (FAILED(q.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, sl.chunk_alloc, nullptr,
                                            IID_PPV_ARGS(&L)))) {
            why = "chunk list failed";
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {sl.heap};
        L->SetDescriptorHeaps(1, heaps);
        const UINT inc = q.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const u64 a = 2ull * kIA, b = 2ull * kIB, o = 2ull * kIO;
        for (u32 k = 0; k < kChunkItems; ++k) {
            const u32 it = c * kChunkItems + k;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = sl.heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = sl.heap->GetGPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(it) * g.descriptors * inc;
            gpu.ptr += static_cast<u64>(it) * g.descriptors * inc;
            DML_BINDING_TABLE_DESC btd = {g.op, cpu, gpu, g.descriptors};
            const HRESULT hr = sl.table ? sl.table->Reset(&btd) : dml->CreateBindingTable(&btd, IID_PPV_ARGS(&sl.table));
            if (FAILED(hr)) {
                L->Close();
                L->Release();
                why = "binding table failed";
                return false;
            }
            DML_BUFFER_BINDING ab = {sl.ina, it * a, a}, bb = {sl.inb, it * b, b}, ob = {sl.outb, it * o, o};
            DML_BINDING_DESC ins[2] = {{DML_BINDING_TYPE_BUFFER, &ab}, {DML_BINDING_TYPE_BUFFER, &bb}};
            DML_BINDING_DESC od = {DML_BINDING_TYPE_BUFFER, &ob};
            sl.table->BindInputs(2, ins);
            sl.table->BindOutputs(1, &od);
            DML_BUFFER_BINDING tbb = {sl.temp, 0, g.temp_size}, pbb = {g.persist, 0, g.persist_size};
            DML_BINDING_DESC td = {DML_BINDING_TYPE_BUFFER, &tbb}, pd = {DML_BINDING_TYPE_BUFFER, &pbb};
            if (g.temp_size)
                sl.table->BindTemporaryResource(&td);
            if (g.persist_size)
                sl.table->BindPersistentResource(&pd);
            rec->RecordDispatch(L, g.op, sl.table);
            uav_all(L);
        }
        if (FAILED(L->Close())) {
            dump_messages(q.dev);
            L->Release();
            why = "chunk list failed to close";
            return false;
        }
        sl.chunk[c] = L;
        return true;
    }
};

namespace {

// S for output (r, col) of an item: the digit products put back together,
// sum ua.ub over the item's chunk.
inline i64 item_sum(const u16* o, u32 r, u32 col) {
    return 64 * static_cast<i64>(float_of(o[r * 128 + col])) +
           8 * (static_cast<i64>(float_of(o[r * 128 + 64 + col])) + static_cast<i64>(float_of(o[(64 + r) * 128 + col]))) +
           static_cast<i64>(float_of(o[(64 + r) * 128 + 64 + col]));
}

// Writes value u's digits: h at a[h_at], l at a[l_at].
inline void put(u16* a, u32 h_at, u32 l_at, u32 u, const u16* d) {
    a[h_at] = d[u >> 3];
    a[l_at] = d[u & 7];
}

// The self-test: known-answer batches through the real path, from one item
// to every item a submission takes and past it (those ops are done on the
// CPU), with extreme values (all 0, all 63).
bool npu_self_test(NpuBackend& b, TextBuf* err) {
    u32 seed = 2026;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    struct Spec {
        u32 kind, n, fill; // fill: 0 random, 1 all 0, 2 all 63
    };
    // t0: 6 items; t1: 108; t2: exactly kMaxItems (256), then ops that no
    // longer fit.
    const Spec t0[] = {{kAccDot, 5, 0}, {kAccDot, 1, 2}, {kAccMatmul, 64, 0}, {kAccMatvec, 7, 0},
                       {kAccMatmul, 1, 1}, {kAccArgmax, 10, 0}};
    const Spec t1[] = {{kAccMatvec, 256, 1}, {kAccMatvec, 256, 0}, {kAccMatmul, 64, 2}, {kAccMatmul, 33, 0},
                       {kAccDot, 2049, 0},   {kAccDot, 65536, 0},  {kAccMatvec, 65, 2}, {kAccDot, 0, 0}};
    const Spec t2[] = {{kAccMatvec, 256, 2}, {kAccMatvec, 256, 0}, {kAccMatvec, 256, 0}, {kAccMatvec, 256, 0},
                       {kAccMatvec, 256, 0}, {kAccMatvec, 256, 0}, {kAccMatvec, 256, 0}, {kAccMatvec, 256, 0},
                       {kAccMatvec, 200, 0}, {kAccMatmul, 64, 0},  {kAccDot, 100, 0},    {kAccArgmax, 300, 0},
                       {kAccMatvec, 1, 0}};
    const Spec* tests[3] = {t0, t1, t2};
    const u32 counts[3] = {sizeof t0 / sizeof t0[0], sizeof t1 / sizeof t1[0], sizeof t2 / sizeof t2[0]};
    Array<AccelOp> ops;
    Array<u8> in, got, want;
    Array<u32> pick, scal;
    for (u32 t = 0; t < 3; ++t) {
        ops.clear();
        in.clear();
        pick.clear();
        u32 outs = 0;
        for (u32 i = 0; i < counts[t]; ++i) {
            const Spec& s = tests[t][i];
            AccelOp op;
            memset(&op, 0, sizeof op);
            op.kind = s.kind;
            op.n = s.n;
            op.src_n = s.kind == kAccMatmul ? s.n * s.n : s.n;
            op.dst_n = s.kind == kAccDot ? s.n : 0;
            op.mat_n = s.kind == kAccMatvec || s.kind == kAccMatmul ? s.n * s.n : 0;
            op.out_n = accel_out_codons(s.kind, s.n);
            op.in_off = static_cast<u32>(in.size());
            const u32 reads = op.src_n + op.dst_n + op.mat_n;
            for (u32 j = 0; j < reads; ++j)
                in.push(static_cast<u8>(s.fill == 1 ? 0 : (s.fill == 2 ? 63 : next() % 64)));
            ops.push(op);
            pick.push(i);
            outs += op.out_n;
        }
        got.resize(outs ? outs : 1);
        scal.resize(ops.size());
        const int tk = b.submit(ops.data(), pick.data(), static_cast<u32>(pick.size()), in.data());
        if (tk < 0 || !b.collect(tk, got.data(), scal.data())) {
            char m[160];
            snprintf(m, sizeof m, "self-test batch %u failed to run: %s", t, b.why());
            say(err, m);
            return false;
        }
        u32 off = 0;
        for (usize i = 0; i < ops.size(); ++i) {
            const AccelOp& op = ops[i];
            want.resize(op.out_n ? op.out_n : 1);
            const u32 s = accel_reference(op, in.data() + op.in_off, want.data());
            const bool same = (!accel_has_scalar(op.kind) || s == scal[i]) &&
                              (op.out_n == 0 || memcmp(want.data(), got.data() + off, op.out_n) == 0);
            if (!same) {
                char m[160];
                snprintf(m, sizeof m, "self-test: batch %u op %u (kind %u, n %u) gave a wrong answer", t,
                         static_cast<u32>(i), op.kind, op.n);
                say(err, m);
                return false;
            }
            off += op.out_n;
        }
    }
    return true;
}

} // namespace

bool NpuBackend::open(u32 index, TextBuf* err, double* compile_ms) {
    close();
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    Found found[16];
    const u32 n = enumerate(found, 16);
    if (index >= n) {
        release_all(found, n);
        say(err, "no such device");
        return false;
    }
    Impl* m = new Impl;
    impl_ = m;
    strncpy(m->name, found[index].d.name, sizeof m->name - 1);
    bool ok = m->q.init(found[index].adapter, found[index].d.gpu != 0, err);
    release_all(found, n);
    HRESULT hr = S_OK;
    if (ok && (FAILED(hr = DMLCreateDevice(m->q.dev,
                                           debug_layers() ? DML_CREATE_DEVICE_FLAG_DEBUG : DML_CREATE_DEVICE_FLAG_NONE,
                                           IID_PPV_ARGS(&m->dml))) ||
               FAILED(hr = m->dml->CreateCommandRecorder(IID_PPV_ARGS(&m->rec))))) {
        say_hr(err, "DirectML failed to start", hr);
        ok = false;
    }
    for (u32 s = 0; ok && s < kAccelSlots; ++s) {
        Impl::Slot& sl = m->slots[s];
        if (FAILED(hr = m->q.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&sl.alloc))) ||
            FAILED(hr = m->q.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                         IID_PPV_ARGS(&sl.chunk_alloc))) ||
            FAILED(hr = m->q.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, sl.alloc, nullptr,
                                                    IID_PPV_ARGS(&sl.pre)))) {
            say_hr(err, "command list failed", hr);
            ok = false;
            break;
        }
        sl.pre->Close();
        if (FAILED(hr = m->q.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, sl.alloc, nullptr,
                                                    IID_PPV_ARGS(&sl.post)))) {
            say_hr(err, "command list failed", hr);
            ok = false;
            break;
        }
        sl.post->Close();
    }
    for (u32 d = 0; d < 8; ++d)
        m->digit[d] = half_of(static_cast<float>(d));
    ok = ok && gemm_build(m->q, m->dml, m->rec, 1, 128, 128, &m->g, err);
    ok = ok && npu_self_test(*this, err);
    QueryPerformanceCounter(&t1);
    if (compile_ms)
        *compile_ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f0.QuadPart);
    if (!ok)
        close();
    return ok;
}

void NpuBackend::close() {
    if (!impl_)
        return;
    Impl* m = impl_;
    if (m->q.queue && m->q.fence) {
        m->q.queue->Signal(m->q.fence, ++m->q.value);
        if (m->q.fence->GetCompletedValue() < m->q.value) {
            m->q.fence->SetEventOnCompletion(m->q.value, m->q.event);
            WaitForSingleObject(m->q.event, INFINITE);
        }
    }
    for (u32 s = 0; s < kAccelSlots; ++s) {
        Impl::Slot& sl = m->slots[s];
        for (u32 c = 0; c < kChunkLists; ++c)
            release(sl.chunk[c]);
        release(sl.table);
        release(sl.heap);
        release(sl.pre);
        release(sl.post);
        release(sl.alloc);
        release(sl.chunk_alloc);
        release(sl.up);
        release(sl.ina);
        release(sl.inb);
        release(sl.outb);
        release(sl.rb);
        release(sl.temp);
    }
    m->g.destroy();
    release(m->rec);
    release(m->dml);
    m->q.destroy();
    delete m;
    impl_ = nullptr;
}

const char* NpuBackend::name() const { return impl_ ? impl_->name : "none"; }

const char* NpuBackend::why() const { return impl_ ? impl_->why : "not open"; }

int NpuBackend::submit(const AccelOp* ops, const u32* pick, u32 n, const u8* in) {
    if (!impl_ || impl_->broken || n == 0)
        return -1;
    Impl* m = impl_;
    int slot = -1;
    for (u32 s = 0; s < kAccelSlots; ++s)
        if (!m->slots[s].busy) {
            slot = static_cast<int>(s);
            break;
        }
    if (slot < 0)
        return -1;
    Impl::Slot& sl = m->slots[slot];
    if (!sl.ready && !m->make_slot(sl))
        return -1;
    // Which ops go to the NPU, and their items.
    sl.ops.resize(n);
    u32 items = 0, outs = 0;
    for (u32 i = 0; i < n; ++i) {
        const AccelOp& op = ops[pick[i]];
        Impl::OpRec& r = sl.ops[i];
        r.kind = op.kind;
        r.n = op.n;
        r.out_off = outs;
        r.out_n = op.out_n;
        outs += op.out_n;
        r.npu = 0;
        r.item = 0;
        r.scalar = 0;
        const u32 need = items_of(op);
        if (need && items + need <= kMaxItems) {
            r.npu = 1;
            r.item = items;
            items += need;
        }
    }
    const u32 chunks_used = (items + kChunkItems - 1) / kChunkItems;
    for (u32 c = 0; c < chunks_used; ++c)
        if (!sl.chunk[c] && !m->record_chunk(sl, c)) {
            m->broken = true;
            return -1;
        }
    const u64 a_item = 2ull * kIA, b_item = 2ull * kIB, o_item = 2ull * kIO;
    const u64 a_region = kMaxItems * a_item; // in the upload buffer, B follows A
    // The digits, and each op's corrections.
    sl.aux.clear();
    sl.pre_out.resize(outs ? outs : 1);
    unsigned char* base = nullptr;
    if (items) {
        D3D12_RANGE none = {0, 0};
        if (FAILED(sl.up->Map(0, &none, reinterpret_cast<void**>(&base)))) {
            m->why = "upload map failed";
            return -1;
        }
        // Zero every item the chunks will run (the padding inside an item
        // must be zero; whole unused items just keep the multiplies finite).
        memset(base, 0, static_cast<size_t>(chunks_used * kChunkItems * a_item));
        memset(base + a_region, 0, static_cast<size_t>(chunks_used * kChunkItems * b_item));
    }
    u16* A = reinterpret_cast<u16*>(base);
    u16* B = base ? reinterpret_cast<u16*>(base + a_region) : nullptr;
    const u16* D = m->digit;
    for (u32 i = 0; i < n; ++i) {
        const AccelOp& op = ops[pick[i]];
        Impl::OpRec& r = sl.ops[i];
        const u8* src = in + op.in_off;
        const u8* dst = src + op.src_n;
        const u8* mat = dst + op.dst_n;
        const u32 k = op.n;
        r.aux = static_cast<u32>(sl.aux.size());
        if (!r.npu) {
            r.scalar = accel_reference(op, src, sl.pre_out.data() + r.out_off);
        } else if (op.kind == kAccDot) {
            // Item t holds chunks 64t.. of a (rows) and of b (columns); chunk
            // cc of the item pairs row cc with column cc.
            i64 sa = 0, sb = 0;
            for (u32 x = 0; x < k; ++x) {
                const u32 ua = src[x] & 63, ub = dst[x] & 63;
                sa += ua;
                sb += ub;
                const u32 ch = x / kChunk, c = x % kChunk;
                const u32 it = r.item + ch / 64, cc = ch % 64;
                u16* a = A + static_cast<u64>(it) * kIA;
                u16* b = B + static_cast<u64>(it) * kIB;
                put(a, cc * kChunk + c, (64 + cc) * kChunk + c, ua, D);
                put(b, c * 128 + cc, c * 128 + 64 + cc, ub, D);
            }
            sl.aux.push(-32 * sa - 32 * sb + 1024 * static_cast<i64>(k));
        } else if (op.kind == kAccMatvec) {
            // Item (row group gr, chunk ch): rows 64gr.. of M against chunk ch
            // of v (column 0).
            const u32 nc = chunks(k);
            i64 sv = 0;
            for (u32 x = 0; x < k; ++x) {
                const u32 ub = src[x] & 63;
                sv += ub;
                const u32 ch = x / kChunk, c = x % kChunk;
                for (u32 gr = 0; gr < groups(k); ++gr) {
                    u16* b = B + static_cast<u64>(r.item + gr * nc + ch) * kIB;
                    put(b, c * 128, c * 128 + 64, ub, D);
                }
            }
            for (u32 j = 0; j < k; ++j) {
                i64 sr = 0;
                const u32 gr = j / 64, jj = j % 64;
                for (u32 x = 0; x < k; ++x) {
                    const u32 ua = mat[j * k + x] & 63;
                    sr += ua;
                    u16* a = A + static_cast<u64>(r.item + gr * nc + x / kChunk) * kIA;
                    const u32 c = x % kChunk;
                    put(a, jj * kChunk + c, (64 + jj) * kChunk + c, ua, D);
                }
                sl.aux.push(-32 * sr - 32 * sv + 1024 * static_cast<i64>(k));
            }
        } else { // matmul: A = src, B = mat; item ch holds chunk ch of the inner index
            for (u32 j = 0; j < k; ++j) { // A's row sums
                i64 s = 0;
                for (u32 x = 0; x < k; ++x)
                    s += src[j * k + x] & 63;
                sl.aux.push(s);
            }
            for (u32 l = 0; l < k; ++l) { // B's column sums
                i64 s = 0;
                for (u32 x = 0; x < k; ++x)
                    s += mat[x * k + l] & 63;
                sl.aux.push(s);
            }
            for (u32 x = 0; x < k; ++x) {
                const u32 c = x % kChunk;
                u16* a = A + static_cast<u64>(r.item + x / kChunk) * kIA;
                u16* b = B + static_cast<u64>(r.item + x / kChunk) * kIB;
                for (u32 j = 0; j < k; ++j)
                    put(a, j * kChunk + c, (64 + j) * kChunk + c, src[j * k + x] & 63, D);
                for (u32 l = 0; l < k; ++l)
                    put(b, c * 128 + l, c * 128 + 64 + l, mat[x * k + l] & 63, D);
            }
        }
    }
    if (base)
        sl.up->Unmap(0, nullptr);
    sl.items = items;
    if (items) {
        // Copy in, replay the chunks' dispatches, copy out. (Buffers decay to
        // the common state when a submission finishes.)
        const u64 run = chunks_used * kChunkItems;
        sl.alloc->Reset();
        sl.pre->Reset(sl.alloc, nullptr);
        ID3D12GraphicsCommandList* L = sl.pre;
        transition(L, sl.ina, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(L, sl.inb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        L->CopyBufferRegion(sl.ina, 0, sl.up, 0, run * a_item);
        L->CopyBufferRegion(sl.inb, 0, sl.up, a_region, run * b_item);
        transition(L, sl.ina, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(L, sl.inb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(L, sl.outb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (sl.temp)
            transition(L, sl.temp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        HRESULT hr = L->Close();
        sl.post->Reset(sl.alloc, nullptr);
        transition(sl.post, sl.outb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        sl.post->CopyBufferRegion(sl.rb, 0, sl.outb, 0, items * o_item);
        if (SUCCEEDED(hr))
            hr = sl.post->Close();
        if (FAILED(hr)) {
            m->why = "copy list failed to close";
            return -1;
        }
        ID3D12CommandList* lists[kChunkLists + 2];
        u32 nl = 0;
        lists[nl++] = sl.pre;
        for (u32 c = 0; c < chunks_used; ++c)
            lists[nl++] = sl.chunk[c];
        lists[nl++] = sl.post;
        m->q.queue->ExecuteCommandLists(nl, lists);
    }
    m->q.queue->Signal(m->q.fence, ++m->q.value);
    sl.fence = m->q.value;
    sl.busy = true;
    return slot;
}

bool NpuBackend::collect(int ticket, u8* codons, u32* scalars) {
    if (!impl_ || ticket < 0 || ticket >= static_cast<int>(kAccelSlots))
        return false;
    Impl* m = impl_;
    Impl::Slot& sl = m->slots[ticket];
    if (!sl.busy)
        return false;
    if (m->q.fence->GetCompletedValue() < sl.fence) {
        m->q.fence->SetEventOnCompletion(sl.fence, m->q.event);
        WaitForSingleObject(m->q.event, INFINITE);
    }
    sl.busy = false;
    if (FAILED(m->q.dev->GetDeviceRemovedReason())) {
        m->why = "device removed";
        return false;
    }
    void* mapped = nullptr;
    if (sl.items) {
        D3D12_RANGE all = {0, static_cast<SIZE_T>(sl.items * 2ull * kIO)};
        if (FAILED(sl.rb->Map(0, &all, &mapped))) {
            m->why = "readback map failed";
            return false;
        }
    }
    const u16* O = static_cast<const u16*>(mapped);
    for (u32 i = 0; i < sl.ops.size(); ++i) {
        const Impl::OpRec& r = sl.ops[i];
        u8* out = codons + r.out_off;
        const i64* aux = sl.aux.data() + r.aux;
        const u32 k = r.n;
        scalars[i] = 0;
        if (!r.npu) {
            if (r.out_n)
                memcpy(out, sl.pre_out.data() + r.out_off, r.out_n);
            scalars[i] = r.scalar;
        } else if (r.kind == kAccDot) {
            i64 s = aux[0];
            for (u32 ch = 0; ch < chunks(k); ++ch)
                s += item_sum(O + static_cast<u64>(r.item + ch / 64) * kIO, ch % 64, ch % 64);
            scalars[i] = static_cast<u32>(static_cast<i32>(s));
        } else if (r.kind == kAccMatvec) {
            const u32 nc = chunks(k);
            for (u32 j = 0; j < k; ++j) {
                i64 s = aux[j];
                for (u32 ch = 0; ch < nc; ++ch)
                    s += item_sum(O + static_cast<u64>(r.item + (j / 64) * nc + ch) * kIO, j % 64, 0);
                out[j] = sat_codon64(s);
            }
        } else { // matmul
            const u32 nc = chunks(k);
            for (u32 j = 0; j < k; ++j)
                for (u32 l = 0; l < k; ++l) {
                    i64 s = -32 * aux[j] - 32 * aux[k + l] + 1024 * static_cast<i64>(k);
                    for (u32 ch = 0; ch < nc; ++ch)
                        s += item_sum(O + static_cast<u64>(r.item + ch) * kIO, j, l);
                    out[j * k + l] = sat_codon64(s);
                }
        }
    }
    if (mapped) {
        D3D12_RANGE none = {0, 0};
        sl.rb->Unmap(0, &none);
    }
    return true;
}

u32 accel_list(AccelDevice* out, u32 max) {
    Found found[16];
    const u32 n = enumerate(found, max < 16 ? max : 16);
    for (u32 i = 0; i < n; ++i)
        out[i] = found[i].d;
    release_all(found, n);
    return n;
}

bool accel_probe_compute(u32 index, TextBuf* r) {
    Found found[16];
    const u32 n = enumerate(found, 16);
    if (index >= n) {
        release_all(found, n);
        say(r, "no such device\n");
        return false;
    }
    if (!found[index].d.gpu) {
        release_all(found, n);
        say(r, "  compute shader: not a GPU, skipped\n");
        return false;
    }
    Queue q;
    bool ok = false;
    ID3DBlob *cs = nullptr, *err = nullptr, *sig = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource *buf = nullptr, *rb = nullptr;
    say(r, "  compute shader: ");
    do {
        if (!q.init(found[index].adapter, true, r))
            break;
        const char* src = "RWStructuredBuffer<uint> o : register(u0);\n"
                          "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
                          "    o[id.x] = id.x * 3u + 7u;\n"
                          "}\n";
        HRESULT hr = D3DCompile(src, strlen(src), "probe", nullptr, nullptr, "main", "cs_5_1",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
        if (FAILED(hr)) {
            say_hr(r, "shader failed to compile", hr);
            break;
        }
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        param.Descriptor.ShaderRegister = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        if (FAILED(hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(hr = q.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&root)))) {
            say_hr(r, "root signature failed", hr);
            break;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = root;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(hr = q.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) {
            say_hr(r, "pipeline failed", hr);
            break;
        }
        const u32 count = 4096;
        buf = q.buffer(count * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        rb = q.buffer(count * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (!buf || !rb) {
            say(r, "buffer allocation failed");
            break;
        }
        q.list->SetComputeRootSignature(root);
        q.list->SetPipelineState(pso);
        q.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
        q.list->Dispatch(count / 64, 1, 1);
        q.barrier(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        q.list->CopyBufferRegion(rb, 0, buf, 0, count * 4);
        if (!q.run(r))
            break;
        static u32 got[4096];
        if (!read_back(rb, got, count * 4)) {
            say(r, "readback failed");
            break;
        }
        u32 wrong = 0;
        for (u32 i = 0; i < count; ++i)
            wrong += got[i] != i * 3u + 7u;
        char b[80];
        snprintf(b, sizeof b, wrong ? "%u of %u wrong" : "ran, all %u values right", wrong ? wrong : count, count);
        say(r, b);
        ok = wrong == 0;
    } while (false);
    say(r, "\n");
    release(cs);
    release(err);
    release(sig);
    release(root);
    release(pso);
    release(buf);
    release(rb);
    q.destroy();
    release_all(found, n);
    return ok;
}

// --- Phase 9: the GPU's own operations (nova bench costs) ---------------------

namespace {

// One source, a kernel per MODE: 0 add, 1 multiply, 2 divide (dependent
// chains, one per lane), 3 a load (each lane chasing its own pointer chain
// through `chase` from start + lane), 4 independent adds (four chains a lane,
// for throughput), 5 a search (each lane looking through 4 KB for a value
// that isn't there, place by place, as a template search does). Every
// result is written, so nothing is optimised away.
const char* const kMicroHlsl =
    "RWStructuredBuffer<uint> chase : register(u0);\n"
    "RWStructuredBuffer<uint> outb : register(u1);\n"
    "cbuffer K : register(b0) { uint iters; uint start; uint seed; uint pad; };\n"
    "[numthreads(32, 1, 1)]\n"
    "void main(uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID) {\n"
    "    uint x = seed + tid.x;\n"
    "    uint i = 0u;\n"
    "#if MODE == 0\n"
    "    uint y = seed | 1u; uint z = seed >> 3;\n"
    "    for (i = 0u; i < iters; i++) { x += y; x ^= z; x += y; x ^= z; x += y; x ^= z; x += y; x ^= z; }\n"
    "#elif MODE == 1\n"
    "    uint k = seed | 1u; uint y = seed >> 5;\n"
    "    for (i = 0u; i < iters; i++) { x = (x * k) ^ y; x = (x * k) ^ y; x = (x * k) ^ y; x = (x * k) ^ y;\n"
    "                                   x = (x * k) ^ y; x = (x * k) ^ y; x = (x * k) ^ y; x = (x * k) ^ y; }\n"
    "#elif MODE == 2\n"
    "    uint d = (seed & 7u) + 3u; uint b = 0x40000000u; x = b + tid.x;\n"
    "    for (i = 0u; i < iters; i++) { x = x / d + b; x = x / d + b; x = x / d + b; x = x / d + b; }\n"
    "#elif MODE == 3\n"
    "    x = chase[start + tid.x];\n"
    "    for (i = 0u; i < iters; i++) { x = chase[x]; x = chase[x]; x = chase[x]; x = chase[x];\n"
    "                                   x = chase[x]; x = chase[x]; x = chase[x]; x = chase[x]; }\n"
    "#elif MODE == 5\n"
    "    uint at = start + tid.x;\n"
    "    for (i = 0u; i < iters; i++) { if (chase[(at + i) & 1023u] == 0xFFFFFFFFu) break; }\n"
    "    x = i;\n"
    "#else\n"
    "    uint a = x; uint b2 = x + 1u; uint c = x + 2u; uint e = x + 3u; uint y = seed | 1u; uint z = seed >> 3;\n"
    "    for (i = 0u; i < iters; i++) { a += y; b2 += y; c += y; e += y; a ^= z; b2 ^= z; c ^= z; e ^= z;\n"
    "                                   a += y; b2 += y; c += y; e += y; a ^= z; b2 ^= z; c ^= z; e ^= z; }\n"
    "    x = a ^ b2 ^ c ^ e;\n"
    "#endif\n"
    "    outb[dtid.x] = x;\n"
    "}\n";

int cmp_micro(const void* a, const void* b) {
    const double x = *static_cast<const double*>(a), y = *static_cast<const double*>(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// A random single cycle through [base, base + n) of `a`, as absolute indices.
void make_cycle(u32* a, u32 base, u32 n, u64 seed) {
    for (u32 i = 0; i < n; ++i)
        a[base + i] = base + i;
    u64 r = seed;
    for (u32 i = n - 1; i > 0; --i) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        const u32 j = static_cast<u32>(r % i);
        const u32 t = a[base + i];
        a[base + i] = a[base + j];
        a[base + j] = t;
    }
}

} // namespace

bool gpu_microbench(u32 index, GpuMicro* out, TextBuf* r) {
    *out = GpuMicro{};
    Found found[16];
    const u32 n = enumerate(found, 16);
    if (index >= n || !found[index].d.gpu) {
        release_all(found, n);
        say(r, "not a GPU");
        return false;
    }
    const u32 kL1 = 1024, kMem = 262144, kVram = 67108864; // 4 KB, 1 MB, 256 MB
    const u64 words = static_cast<u64>(kL1) + kMem + kVram, out_words = 8192 * 32;
    Queue q;
    bool ok = false;
    ID3DBlob *sig = nullptr, *err = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso[6] = {};
    ID3D12Resource *chase = nullptr, *outb = nullptr, *up = nullptr, *qrb = nullptr;
    ID3D12QueryHeap* qh = nullptr;
    do {
        if (!q.init(found[index].adapter, true, r))
            break;
        D3D12_ROOT_PARAMETER ps[3] = {};
        ps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        ps[0].Descriptor.ShaderRegister = 0;
        ps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        ps[1].Descriptor.ShaderRegister = 1;
        ps[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        ps[2].Constants.ShaderRegister = 0;
        ps[2].Constants.Num32BitValues = 4;
        for (auto& p : ps)
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = ps;
        HRESULT hr = E_FAIL;
        if (FAILED(hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(hr = q.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&root)))) {
            say_hr(r, "root signature failed", hr);
            break;
        }
        bool built = true;
        for (u32 m = 0; m < 6 && built; ++m) {
            char mode[2] = {static_cast<char>('0' + m), 0};
            const D3D_SHADER_MACRO defs[] = {{"MODE", mode}, {nullptr, nullptr}};
            ID3DBlob *cs = nullptr, *msg = nullptr;
            hr = D3DCompile(kMicroHlsl, strlen(kMicroHlsl), "nova_micro", defs, nullptr, "main", "cs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &msg);
            if (SUCCEEDED(hr)) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
                pd.pRootSignature = root;
                pd.CS.pShaderBytecode = cs->GetBufferPointer();
                pd.CS.BytecodeLength = cs->GetBufferSize();
                hr = q.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso[m]));
            }
            if (FAILED(hr)) {
                say_hr(r, "microbenchmark kernel failed", hr);
                if (msg)
                    say(r, static_cast<const char*>(msg->GetBufferPointer()));
                built = false;
            }
            release(cs);
            release(msg);
        }
        if (!built)
            break;
        // The chase: three cycles (4 KB, 1 MB, 256 MB), uploaded once.
        chase = q.buffer(words * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        outb = q.buffer(out_words * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        up = q.buffer(words * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        qrb = q.buffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        D3D12_QUERY_HEAP_DESC qd = {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2;
        if (!chase || !outb || !up || !qrb || FAILED(q.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qh)))) {
            say(r, "microbenchmark buffers could not be made");
            break;
        }
        u32* m = nullptr;
        D3D12_RANGE none = {0, 0};
        if (FAILED(up->Map(0, &none, reinterpret_cast<void**>(&m)))) {
            say(r, "upload buffer would not map");
            break;
        }
        make_cycle(m, 0, kL1, 0x9E3779B97F4A7C15ull);
        make_cycle(m, kL1, kMem, 0xD1B54A32D192ED03ull);
        make_cycle(m, kL1 + kMem, kVram, 0x8CB92BA72F3D8DD7ull);
        up->Unmap(0, nullptr);
        q.list->CopyBufferRegion(chase, 0, up, 0, words * 4);
        q.barrier(chase, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!q.run(r))
            break;
        u64 freq = 0;
        if (FAILED(q.queue->GetTimestampFrequency(&freq)) || freq == 0) {
            say(r, "no GPU timestamps");
            break;
        }
        // Seconds a dispatch takes on the device's own clock.
        auto timed = [&](u32 mode, u32 groups, u32 iters, u32 start, double* sec) {
            q.list->SetComputeRootSignature(root);
            q.list->SetPipelineState(pso[mode]);
            q.list->SetComputeRootUnorderedAccessView(0, chase->GetGPUVirtualAddress());
            q.list->SetComputeRootUnorderedAccessView(1, outb->GetGPUVirtualAddress());
            const u32 k[4] = {iters, start, 0x2545F491u, 0};
            q.list->SetComputeRoot32BitConstants(2, 4, k, 0);
            q.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
            q.list->Dispatch(groups, 1, 1);
            q.list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
            q.list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb, 0);
            if (!q.run(r))
                return false;
            u64 ts[2] = {};
            if (!read_back(qrb, ts, sizeof ts) || ts[1] < ts[0])
                return false;
            *sec = static_cast<double>(ts[1] - ts[0]) / static_cast<double>(freq);
            return true;
        };
        // Picoseconds an op: iterations grown until a dispatch takes 20 ms,
        // then the median of 5.
        auto measure = [&](u32 mode, u32 groups, u32 start, double ops_per_iter, double* ps) {
            u32 iters = 64;
            double s = 0;
            for (;;) {
                if (!timed(mode, groups, iters, start, &s))
                    return false;
                if (s >= 0.02 || iters >= (1u << 28))
                    break;
                const double grow = s > 0 ? 0.02 / s : 64.0;
                const double next = static_cast<double>(iters) * (grow > 64.0 ? 64.0 : grow * 1.1);
                iters = next > static_cast<double>(1u << 28) ? (1u << 28) : static_cast<u32>(next) + 1;
            }
            double v[5];
            for (int rep = 0; rep < 5; ++rep) {
                if (!timed(mode, groups, iters, start, &s))
                    return false;
                v[rep] = s;
            }
            qsort(v, 5, sizeof v[0], cmp_micro);
            *ps = v[2] * 1e12 / (static_cast<double>(iters) * ops_per_iter);
            return true;
        };
        double alu = 0, mul = 0, div = 0, l1 = 0, mem = 0, vram = 0, thr = 0, scan = 0;
        const u32 tgroups = 8192;
        if (!measure(0, 1, 0, 8, &alu) || !measure(1, 1, 0, 8, &mul) || !measure(2, 1, 0, 4, &div) ||
            !measure(3, 1, 0, 8, &l1) || !measure(3, 1, kL1, 8, &mem) || !measure(3, 1, kL1 + kMem, 8, &vram) ||
            !measure(4, tgroups, 0, 16, &thr) || !measure(5, 1, 0, 1, &scan))
            break;
        out->scan = scan;
        out->alu = alu;
        out->mul = mul > alu ? mul - alu : mul; // less the xor between
        out->div = div > alu ? div - alu : div; // less the add between
        out->l1 = l1;
        out->mem = mem;
        out->vram = vram;
        // thr: picoseconds per op per lane-chain over the whole dispatch
        const double total_ops_per_ps = static_cast<double>(tgroups) * 32.0 / thr;
        out->ops_per_s = total_ops_per_ps * 1e12;
        ok = true;
    } while (false);
    release(sig);
    release(err);
    release(root);
    for (auto* p : pso)
        release(p);
    release(chase);
    release(outb);
    release(up);
    release(qrb);
    release(qh);
    q.destroy();
    release_all(found, n);
    return ok;
}

u32 accel_probe_dml(u32 index, TextBuf* r) {
    Found found[16];
    const u32 n = enumerate(found, 16);
    if (index >= n) {
        release_all(found, n);
        say(r, "no such device\n");
        return 0;
    }
    Queue q;
    IDMLDevice* dml = nullptr;
    u32 exact = 0;
    say(r, "  DirectML:");
    do {
        if (!q.init(found[index].adapter, found[index].d.gpu != 0, r)) {
            say(r, "\n");
            break;
        }
        HRESULT hr = DMLCreateDevice(q.dev, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml));
        if (FAILED(hr)) {
            say_hr(r, " no DirectML device", hr);
            say(r, "\n");
            break;
        }
        DML_FEATURE_LEVEL levels[] = {DML_FEATURE_LEVEL_1_0, DML_FEATURE_LEVEL_2_0, DML_FEATURE_LEVEL_2_1,
                                      DML_FEATURE_LEVEL_3_0, DML_FEATURE_LEVEL_4_0, DML_FEATURE_LEVEL_5_0,
                                      DML_FEATURE_LEVEL_6_0, DML_FEATURE_LEVEL_6_4};
        DML_FEATURE_QUERY_FEATURE_LEVELS query = {static_cast<UINT>(sizeof levels / sizeof levels[0]), levels};
        DML_FEATURE_DATA_FEATURE_LEVELS data = {};
        char b[96];
        if (SUCCEEDED(dml->CheckFeatureSupport(DML_FEATURE_FEATURE_LEVELS, sizeof query, &query, sizeof data, &data)))
            snprintf(b, sizeof b, " feature level %u.%u\n", (data.MaxSupportedFeatureLevel >> 12) & 0xF,
                     (data.MaxSupportedFeatureLevel >> 8) & 0xF);
        else
            snprintf(b, sizeof b, " (feature level unknown)\n");
        say(r, b);
        copy_round_trip(q, r);
        dml_identity(q, dml, r);
        const u32 forms[3] = {kProbeInt8, kProbeFloat32, kProbeFloat16};
        for (u32 f : forms)
            if (dml_multiply(q, dml, f, r))
                exact |= f;
        // The forms NPUs usually take: one-node graphs, half precision allowed,
        // and float16 with every sum below 2048 (exact if it runs at all).
        if (dml_multiply(q, dml, kProbeFloat16, r, true, false))
            exact |= kProbeFloat16Small;
        if (dml_multiply(q, dml, kProbeFloat16, r, true, true))
            exact |= kProbeFloat16Small | kProbeGraph;
        if (dml_multiply(q, dml, kProbeInt8, r, false, true))
            exact |= kProbeInt8 | kProbeGraph;
        // NPUs are built for convolutions: the same multiply as a 1x1 one.
        if (dml_conv(q, dml, r, false))
            exact |= kProbeFloat16Small | kProbeGraph | kProbeConv;
        if (dml_conv(q, dml, r, true))
            exact |= kProbeFloat16Small | kProbeGraph | kProbeConv | kProbeWeights;
        // NPUs usually take B as fixed weights compiled in.
        if (dml_multiply(q, dml, kProbeFloat16, r, true, true, true))
            exact |= kProbeFloat16Small | kProbeGraph | kProbeWeights;
        if (dml_multiply(q, dml, kProbeInt8, r, false, true, true))
            exact |= kProbeInt8 | kProbeGraph | kProbeWeights;
    } while (false);
    release(dml);
    q.destroy();
    release_all(found, n);
    return exact;
}

} // namespace nova
