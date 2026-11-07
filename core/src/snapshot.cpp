#include "nova/snapshot.h"

#include "nova/hash.h"

namespace nova {

namespace {

const char kMagic[8] = {'N', 'O', 'V', 'A', 'S', 'N', 'A', 'P'};
constexpr u64 kChecksumDomain = 0x6E6F76612D736E70ull; // "nova-snp"

} // namespace

void snapshot_write(const World& world, const Config& cfg, ByteWriter& out) {
    TextBuf text;
    cfg.write(text);
    out.put_bytes(kMagic, sizeof kMagic);
    out.put_u32(kSnapshotVersion);
    out.put_u32(0);
    out.put_u64(world.seed());
    out.put_u64(world.tick());
    out.put_str(text.view());
    world.save_state(out);
    out.put_u64(hash_bytes(out.data(), out.size(), kChecksumDomain));
}

bool snapshot_parse(const u8* data, usize n, SnapshotHeader* h, TextBuf* err) {
    if (n < sizeof kMagic + 8 || memcmp(data, kMagic, sizeof kMagic) != 0) {
        err->append("snapshot: not a Nova snapshot (bad magic)");
        return false;
    }
    ByteReader r(data, n - 8);
    u8 magic[8];
    u32 flags = 0;
    r.get_bytes(magic, sizeof magic);
    r.get_u32(&h->version);
    if (r.ok() && (h->version < kSnapshotOldest || h->version > kSnapshotVersion)) {
        err->append("snapshot: unsupported format version ");
        err->append_u64(h->version);
        err->append(" (this build reads versions ");
        err->append_u64(kSnapshotOldest);
        err->append(" to ");
        err->append_u64(kSnapshotVersion);
        err->append_char(')');
        return false;
    }
    r.get_u32(&flags);
    r.get_u64(&h->seed);
    r.get_u64(&h->tick);
    r.get_str(&h->config_text);
    if (!r.ok()) {
        err->append("snapshot: header truncated");
        return false;
    }
    ByteReader tail(data + n - 8, 8);
    u64 stored = 0;
    tail.get_u64(&stored);
    if (stored != hash_bytes(data, n - 8, kChecksumDomain)) {
        err->append("snapshot: checksum mismatch (file is corrupt or truncated)");
        return false;
    }
    if (flags != 0) {
        err->append("snapshot: unknown flags");
        return false;
    }
    h->world_offset = r.pos();
    h->world_size = r.remaining();
    return true;
}

} // namespace nova
