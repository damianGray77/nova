// The CPU reference for asynchronous accelerator ops (Phase 5, design.md 6.11):
// the definition every device must match bit for bit. The same arithmetic as
// the instant ops (6.7, 6.8), on inputs gathered at a barrier.
#include "nova/accelop.h"

namespace nova {

namespace {

i64 sval(u8 v) { return static_cast<i64>(v & 63) - 32; }

u8 sat_codon(i64 acc) {
    i64 q = acc >> 5;
    q = q < -32 ? -32 : (q > 31 ? 31 : q);
    return static_cast<u8>(q + 32);
}

} // namespace

u32 accel_out_codons(u32 kind, u32 n) {
    if (kind >= kAccCopy && kind <= kAccMul)
        return n;
    if (kind == kAccMatvec)
        return n;
    if (kind == kAccMatmul)
        return n * n;
    return 0;
}

u32 accel_reference(const AccelOp& op, const u8* in, u8* out) {
    const u8* src = in;
    const u8* dst = in + op.src_n;
    const u8* mat = in + op.src_n + op.dst_n;
    const u32 n = op.n, key = op.key & 63;
    switch (op.kind) {
    case kAccCompare: {
        u32 r = 0;
        for (u32 i = 0; i < n; ++i)
            r += src[i] != dst[i];
        return r;
    }
    case kAccCount: {
        u32 r = 0;
        for (u32 i = 0; i < n; ++i)
            r += src[i] == key;
        return r;
    }
    case kAccFind:
        for (u32 i = 0; i < n; ++i)
            if (src[i] == key)
                return i;
        return kAccNotFound;
    case kAccSum: {
        u32 r = 0;
        for (u32 i = 0; i < n; ++i)
            r += src[i];
        return r;
    }
    case kAccCopy:
    case kAccRcopy: // the source addresses were taken in reverse at issue
        for (u32 i = 0; i < n; ++i)
            out[i] = src[i];
        return 0;
    case kAccFill:
        for (u32 i = 0; i < n; ++i)
            out[i] = static_cast<u8>(key);
        return 0;
    case kAccNot:
        for (u32 i = 0; i < n; ++i)
            out[i] = static_cast<u8>(63 - src[i]);
        return 0;
    case kAccAdd:
    case kAccSub:
    case kAccXor:
    case kAccAnd:
    case kAccOr:
    case kAccMin:
    case kAccMax:
    case kAccMul:
        for (u32 i = 0; i < n; ++i) {
            const u32 d = dst[i], s = src[i];
            u32 v;
            switch (op.kind) {
            case kAccAdd: v = d + s; break;
            case kAccSub: v = d - s; break;
            case kAccXor: v = d ^ s; break;
            case kAccAnd: v = d & s; break;
            case kAccOr: v = d | s; break;
            case kAccMin: v = d < s ? d : s; break;
            case kAccMax: v = d > s ? d : s; break;
            default: v = d * s; break;
            }
            out[i] = static_cast<u8>(v & 63);
        }
        return 0;
    case kAccDot: {
        i64 acc = 0;
        for (u32 i = 0; i < n; ++i)
            acc += sval(src[i]) * sval(dst[i]);
        return static_cast<u32>(static_cast<i32>(acc));
    }
    case kAccArgmax: {
        u32 best = 0;
        i32 top = -1;
        for (u32 i = 0; i < n; ++i) {
            if (static_cast<i32>(src[i]) > top) {
                top = src[i];
                best = i;
            }
        }
        return best;
    }
    case kAccMatvec: // v = src (k), M = mat (k x k, row-major)
        for (u32 j = 0; j < n; ++j) {
            i64 acc = 0;
            for (u32 i = 0; i < n; ++i)
                acc += sval(mat[j * n + i]) * sval(src[i]);
            out[j] = sat_codon(acc);
        }
        return 0;
    case kAccMatmul: // A = src (k x k), B = mat (k x k)
        for (u32 j = 0; j < n; ++j)
            for (u32 l = 0; l < n; ++l) {
                i64 acc = 0;
                for (u32 i = 0; i < n; ++i)
                    acc += sval(src[j * n + i]) * sval(mat[i * n + l]);
                out[j * n + l] = sat_codon(acc);
            }
        return 0;
    default:
        return 0;
    }
}

} // namespace nova
