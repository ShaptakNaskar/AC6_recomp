#include "ac6_pac_decoder_probe.h"
#include "ac6_pac_decode_dump.h"
#include "ac6_keyboard_glyphs.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/memory.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, ac6_keyboard_glyphs);

namespace {

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] && std::string_view(value) != "0";
}

bool TraceEnabled() {
    static const bool enabled = [] {
        const bool work_items = EnvFlag("AC6_TRACE_PAC_WORK_ITEMS");
        const bool stacks = EnvFlag("AC6_TRACE_PAC_STACKS");
        // ac6_performance_mode forces log_level=error, which silences the
        // probe's REXFS_INFO output, the PAC dumper's diagnostic lines, and
        // the kernel hook's stack-trace REXKRNL_INFO lines. Lift the relevant
        // categories so those reach the log file when their env-var gate is on.
        if (work_items) {
            rex::SetCategoryLevel(rex::log::fs(), spdlog::level::info);
        }
        if (stacks) {
            rex::SetCategoryLevel(rex::log::krnl(), spdlog::level::info);
        }
        return work_items;
    }();
    return enabled;
}

struct TargetState {
    uint32_t first_work_item = 0;
    uint64_t hit_count = 0;
};

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<uint32_t, TargetState>& Targets() {
    static std::unordered_map<uint32_t, TargetState> m;
    return m;
}

}  // namespace

void ac6PacWorkerDispatchHook(PPCRegister& r28, PPCRegister& ctr) {
    if (!TraceEnabled()) return;

    const uint32_t target = ctr.u32;
    const uint32_t work_item = r28.u32;

    bool first_sighting = false;
    uint64_t total = 0;
    size_t distinct = 0;
    {
        std::scoped_lock lock(Mutex());
        auto& targets = Targets();
        auto& slot = targets[target];
        if (slot.hit_count == 0) {
            slot.first_work_item = work_item;
            first_sighting = true;
        }
        slot.hit_count++;
        total = slot.hit_count;
        distinct = targets.size();
    }

    if (first_sighting) {
        REXFS_INFO(
            "[AC6 PAC WORKER] new dispatch target=0x{:08X} first_work_item=0x{:08X} "
            "(distinct_targets={})",
            target, work_item, distinct);
    } else if (total == 100 || total == 1000 || (total % 10000) == 0) {
        REXFS_INFO("[AC6 PAC WORKER] target=0x{:08X} hits={}", target, total);
    }
}

namespace {

struct L2TargetState {
    uint32_t first_r3 = 0;
    uint32_t first_r4 = 0;
    uint32_t first_r5 = 0;
    uint32_t first_r6 = 0;
    uint64_t hit_count = 0;
    // Up to N distinct (r5, r6) tuples observed for this target; the decoder's
    // slot will eventually be called with an r5 matching a known csize/usize.
    static constexpr size_t kMaxSamples = 12;
    std::vector<std::pair<uint32_t, uint32_t>> samples;
};

std::mutex& L2Mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<uint32_t, L2TargetState>& L2Targets() {
    static std::unordered_map<uint32_t, L2TargetState> m;
    return m;
}

}  // namespace

void ac6PacWorkerL2DispatchHook(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                                PPCRegister& r6, PPCRegister& ctr) {
    if (!TraceEnabled()) return;

    const uint32_t target = ctr.u32;

    bool first_sighting = false;
    bool new_sample = false;
    uint64_t total = 0;
    size_t distinct = 0;
    size_t samples_count = 0;
    L2TargetState snapshot{};
    {
        std::scoped_lock lock(L2Mutex());
        auto& targets = L2Targets();
        auto& slot = targets[target];
        if (slot.hit_count == 0) {
            slot.first_r3 = r3.u32;
            slot.first_r4 = r4.u32;
            slot.first_r5 = r5.u32;
            slot.first_r6 = r6.u32;
            first_sighting = true;
        }
        slot.hit_count++;

        // Bounded distinct-(r5,r6) capture so the decoder's argument signature
        // becomes observable across later calls (first-sighting often catches
        // state-init zeros).
        if (slot.samples.size() < L2TargetState::kMaxSamples) {
            const std::pair<uint32_t, uint32_t> key{r5.u32, r6.u32};
            if (std::find(slot.samples.begin(), slot.samples.end(), key) ==
                slot.samples.end()) {
                slot.samples.push_back(key);
                new_sample = true;
            }
        }

        total = slot.hit_count;
        distinct = targets.size();
        samples_count = slot.samples.size();
        snapshot = slot;
    }

    if (first_sighting) {
        REXFS_INFO(
            "[AC6 PAC L2] new target=0x{:08X} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} "
            "r6=0x{:08X} (distinct_l2_targets={})",
            target, snapshot.first_r3, snapshot.first_r4, snapshot.first_r5,
            snapshot.first_r6, distinct);
    } else if (new_sample) {
        REXFS_INFO(
            "[AC6 PAC L2 sample] target=0x{:08X} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} "
            "r6=0x{:08X} (sample {} / {}, hits={})",
            target, r3.u32, r4.u32, r5.u32, r6.u32, samples_count,
            L2TargetState::kMaxSamples, total);
    } else if (total == 100 || total == 1000 || (total % 10000) == 0) {
        REXFS_INFO("[AC6 PAC L2] target=0x{:08X} hits={}", target, total);
    }
}

namespace {

bool DumpEnabledEnv() {
    static const bool enabled = [] {
        const char* v = std::getenv("AC6_DUMP_PAC_DECODED");
        return v && v[0] && std::string_view(v) != "0";
    }();
    return enabled;
}

std::mutex& DecoderSeenMutex() {
    static std::mutex m;
    return m;
}

std::unordered_set<uint64_t>& DecoderSeenEntries() {
    static std::unordered_set<uint64_t> s;
    return s;
}

std::string HexPreviewBytes(const uint8_t* data, std::size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

}  // namespace

void ac6PacDecoderDumpHook(PPCRegister& r4, PPCRegister& r10, PPCRegister& r11,
                           PPCRegister& r31) {
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;

    auto load_u8 = [memory](uint32_t va) -> uint8_t {
        if (!memory->LookupHeap(va)) return 0;
        return *static_cast<const uint8_t*>(memory->TranslateVirtual(va));
    };
    auto load_u32_be = [memory](uint32_t va) -> uint32_t {
        if (va > UINT32_MAX - 3) return 0;
        if (!memory->LookupHeap(va) || !memory->LookupHeap(va + 3)) return 0;
        return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(va));
    };

    const uint8_t codec = load_u8(r11.u32 + 1);
    const uint32_t csize = load_u32_be(r11.u32 + 8);
    const uint32_t usize = load_u32_be(r11.u32 + 12);
    const uint32_t source_offset = load_u32_be(r31.u32 + 22888);

    if (usize == 0 || r4.u32 == 0 || r4.u32 > UINT32_MAX - usize) return;
    if (!memory->LookupHeap(r4.u32) || !memory->LookupHeap(r4.u32 + usize - 1)) return;
    const auto* host = memory->TranslateVirtual<const uint8_t*>(r4.u32);
    if (!host) return;

    const uint16_t entry_index = static_cast<uint16_t>(r10.u32 & 0xFFFFu);

    // Per-distinct-entry RE log. Dedup on (csize,usize) because r10&0xFFFF is a
    // runtime tag that collides across distinct TOC entries. Captures the
    // descramble pad and its seed so we can crack the per-entry XOR key:
    //   codec_ctx   = streamer(r31) + 344
    //   pad (8B)    @ codec_ctx + 22380  ==  r31 + 22724
    //   seed (u32)  @ codec_ctx + 22392  ==  r31 + 22736
    // The pad is generated by sub_822CF018(seed) and applied as a repeating
    // 8-byte XOR over the codec input. Logging (csize,usize,seed,pad) for every
    // distinct entry in one play session gives us the seed->pad table and the
    // seed=f(entry) relationship needed for a fully offline decoder.
    if (DumpEnabledEnv()) {
        const uint64_t dedup_key = (static_cast<uint64_t>(csize) << 32) | usize;
        bool first = false;
        {
            std::scoped_lock lock(DecoderSeenMutex());
            first = DecoderSeenEntries().insert(dedup_key).second;
        }
        if (first) {
            constexpr std::size_t kPreviewBytes = 32;

            std::string record_hex;
            const uint32_t record_end_check =
                r11.u32 > UINT32_MAX - kPreviewBytes ? 0 : r11.u32 + kPreviewBytes - 1;
            if (record_end_check && memory->LookupHeap(r11.u32) &&
                memory->LookupHeap(record_end_check)) {
                const auto* rec_host = memory->TranslateVirtual<const uint8_t*>(r11.u32);
                if (rec_host) record_hex = HexPreviewBytes(rec_host, kPreviewBytes);
            }

            const std::size_t out_preview =
                usize < kPreviewBytes ? static_cast<std::size_t>(usize) : kPreviewBytes;
            const std::string out_hex = HexPreviewBytes(host, out_preview);

            std::vector<uint8_t> compressed_head =
                Ac6PeekCompressedHead(entry_index, kPreviewBytes);
            const std::string in_hex =
                compressed_head.empty()
                    ? std::string{}
                    : HexPreviewBytes(compressed_head.data(), compressed_head.size());

            // Descramble pad + seed, read from the codec sub-struct at r31+344.
            const uint32_t seed = load_u32_be(r31.u32 + 22736);
            std::string pad_hex;
            const uint32_t pad_addr = r31.u32 + 22724;
            if (memory->LookupHeap(pad_addr) && memory->LookupHeap(pad_addr + 7)) {
                const auto* pad = memory->TranslateVirtual<const uint8_t*>(pad_addr);
                if (pad) pad_hex = HexPreviewBytes(pad, 8);
            }

            REXFS_INFO(
                "[AC6 PAC DECODER] entry={} mode={} csize=0x{:x} usize=0x{:x} src_off=0x{:x} "
                "seed=0x{:08x} pad={} r4=0x{:08X} r11=0x{:08X} record_hex={} in_head={} out_head={}",
                entry_index, uint32_t(codec), csize, usize, source_offset, seed,
                pad_hex.empty() ? "<unmapped>" : pad_hex, r4.u32, r11.u32,
                record_hex.empty() ? "<unmapped>" : record_hex,
                in_hex.empty() ? "<not_buffered>" : in_hex,
                out_hex.empty() ? "<empty>" : out_hex);
        }
    }

    Ac6DumpPacDecodedEntry(entry_index, codec, csize, usize, source_offset, host);

    // Patch after dumping, before the game relocates/consumes the resources.
    // This operates on guest texture bytes, so both Vulkan and D3D12 see the
    // same fixed artwork without enabling the texture replacement subsystem.
    if (REXCVAR_GET(ac6_keyboard_glyphs)) {
        auto* writable = memory->TranslateVirtual<uint8_t*>(r4.u32);
        const size_t patched = ac6::PatchKeyboardGlyphs({writable, usize});
        if (patched) {
            REXLOG_INFO("[KBM glyphs] replaced {} button textures in decoded PAC buffer", patched);
        }
    }
}

// ============================================================================
// Codec internal probes
// ----------------------------------------------------------------------------
// Gated by AC6_TRACE_CODEC_INTERNALS=1. Rate-limited to the first
// kMaxTracedMode1Calls invocations of the mode-1 decoder so log volume stays
// bounded.
// ============================================================================

namespace {

bool CodecTraceEnabledEnv() {
    static const bool enabled = [] {
        const char* v = std::getenv("AC6_TRACE_CODEC_INTERNALS");
        return v && v[0] && std::string_view(v) != "0";
    }();
    return enabled;
}

std::atomic<int>& Mode1CallCount() {
    static std::atomic<int> c{0};
    return c;
}

// First-N detailed-internals trace: bit-fetcher / tree-builder / dynamic-header
// / block-consumer probes only fire while Mode1CallCount <= this. Three
// invocations is enough to capture one entry's worth of internal activity.
constexpr int kMaxInternalTracedCalls = 3;

// Mode-1 entry probe (including codec_input dump) fires up to this many times,
// regardless of the internal-trace gate. Lets us collect input buffers from
// many entries in a single play session to derive the per-entry XOR key.
constexpr int kMaxMode1EntryProbeCalls = 100;

bool CodecTraceActive() {
    if (!CodecTraceEnabledEnv()) return false;
    return Mode1CallCount().load(std::memory_order_relaxed) <= kMaxInternalTracedCalls;
}

uint32_t LoadGuestU32BE(rex::memory::Memory* memory, uint32_t va) {
    if (!memory || va > UINT32_MAX - 3) return 0;
    if (!memory->LookupHeap(va) || !memory->LookupHeap(va + 3)) return 0;
    return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(va));
}

uint64_t LoadGuestU64BE(rex::memory::Memory* memory, uint32_t va) {
    if (!memory || va > UINT32_MAX - 7) return 0;
    if (!memory->LookupHeap(va) || !memory->LookupHeap(va + 7)) return 0;
    return rex::memory::load_and_swap<uint64_t>(memory->TranslateVirtual(va));
}

uint16_t LoadGuestU16BE(rex::memory::Memory* memory, uint32_t va) {
    if (!memory || va > UINT32_MAX - 1) return 0;
    if (!memory->LookupHeap(va) || !memory->LookupHeap(va + 1)) return 0;
    return rex::memory::load_and_swap<uint16_t>(memory->TranslateVirtual(va));
}

void DumpStaticHuffmanTablesOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        auto* memory = REX_KERNEL_MEMORY();
        if (!memory) return;
        // Addresses confirmed via IDA Pro disassembly of sub_822CF2F8.
        const uint32_t kAddrs[2] = {0x827BD1F8u, 0x827BDDF8u};
        for (uint32_t addr : kAddrs) {
            constexpr uint32_t kSize = 256;
            if (!memory->LookupHeap(addr) || !memory->LookupHeap(addr + kSize - 1)) continue;
            const auto* p = memory->TranslateVirtual<const uint8_t*>(addr);
            if (!p) continue;
            REXFS_INFO("[AC6 PAC CODEC] ROM table @ 0x{:08X} ({} bytes): {}",
                       addr, kSize, HexPreviewBytes(p, kSize));
        }
    });
}

// Dumps up to `max_bytes` from the codec's input buffer (base = *(ctx + 56))
// to disk as codec_input_<call_n>.bin. Lets us compare the bytes the codec
// actually decompresses against the kernel-recorded .compressed.bin to
// identify any pre-codec transformation (descrambling, XOR, etc.).
void DumpCodecInputBuffer(uint32_t ctx, int call_n, std::size_t max_bytes) {
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;
    const uint32_t base = LoadGuestU32BE(memory, ctx + 56);
    if (base == 0) return;
    if (!memory->LookupHeap(base)) return;
    const uint32_t avail = LoadGuestU32BE(memory, ctx + 16);  // in_end
    const std::size_t want =
        std::min<std::size_t>(max_bytes, avail > 0 ? static_cast<std::size_t>(avail) : max_bytes);
    if (want == 0) return;
    if (!memory->LookupHeap(base + static_cast<uint32_t>(want) - 1)) return;
    const auto* p = memory->TranslateVirtual<const uint8_t*>(base);
    if (!p) return;

    // Anchor output next to the existing PAC runtime dumps so analysis can
    // compare byte-by-byte against entry_*.compressed.bin in the same dir.
    static const std::filesystem::path kDumpRoot = [] {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        std::filesystem::path candidate = (ec ? std::filesystem::path() : cwd) /
                                          "out" / "ac6_pac_runtime_dump";
        // Walk up to find repo root if cwd is somewhere deeper.
        for (auto cur = candidate.parent_path().parent_path();
             !cur.empty(); cur = cur.parent_path()) {
            if (std::filesystem::exists(cur / "tools" / "run_ac6_asset_pipeline.py", ec)) {
                candidate = cur / "out" / "ac6_pac_runtime_dump";
                break;
            }
            if (cur.has_parent_path() && cur == cur.parent_path()) break;
        }
        return candidate;
    }();

    std::error_code ec;
    std::filesystem::create_directories(kDumpRoot, ec);
    std::ostringstream name;
    name << "codec_input_" << call_n << "_base0x" << std::hex << base << "_size0x" << avail
         << ".bin";
    const auto path = kDumpRoot / name.str();

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        REXFS_ERROR("[AC6 PAC CODEC] failed to open codec input dump {}", path.string());
        return;
    }
    f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(want));
    REXFS_INFO("[AC6 PAC CODEC] dumped codec input #{} base=0x{:08X} size=0x{:x} "
               "(first {} bytes) path={}", call_n, base, avail, want, path.string());
}

}  // namespace

void ac6PacCodecDispatchProbe(PPCRegister& r3) {
    if (!CodecTraceActive()) return;
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;
    const uint32_t ctx = r3.u32;
    uint8_t codec_mode = 0;
    if (memory->LookupHeap(ctx + 68)) {
        codec_mode = *memory->TranslateVirtual<const uint8_t*>(ctx + 68);
    }
    REXFS_INFO("[AC6 PAC CODEC] dispatch ctx=0x{:08X} codec_mode={} disp_state=0x{:x}",
               ctx, uint32_t(codec_mode), LoadGuestU32BE(memory, ctx + 88));
}

void ac6PacMode1EntryProbe(PPCRegister& r3) {
    if (!CodecTraceEnabledEnv()) return;
    const int call_n = Mode1CallCount().fetch_add(1, std::memory_order_relaxed) + 1;
    if (call_n > kMaxMode1EntryProbeCalls) return;

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;

    DumpStaticHuffmanTablesOnce();

    const uint32_t ctx = r3.u32;
    REXFS_INFO("[AC6 PAC CODEC] mode1_entry #{} ctx=0x{:08X} state=0x{:x} "
               "in_ptr=0x{:x} in_end=0x{:x} out_pos=0x{:x} bit_buf=0x{:016x} bit_count={} "
               "base_ptr=0x{:08X}",
               call_n, ctx,
               LoadGuestU32BE(memory, ctx + 84),
               LoadGuestU64BE(memory, ctx + 8),
               LoadGuestU64BE(memory, ctx + 16),
               LoadGuestU64BE(memory, ctx + 32),
               LoadGuestU64BE(memory, ctx + 72),
               LoadGuestU32BE(memory, ctx + 80),
               LoadGuestU32BE(memory, ctx + 56));

    // Dump the codec input buffer on state==0 (initial chunk of a new entry)
    // so we can diff it against the kernel-recorded .compressed.bin to find
    // any transformation between PAC archive bytes and the decoder's input.
    const uint32_t state = LoadGuestU32BE(memory, ctx + 84);
    if (state == 0) {
        DumpCodecInputBuffer(ctx, call_n, 4096);
    }
}

void ac6PacBitFetcherProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    if (!CodecTraceActive()) return;
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;
    const uint32_t ctx = r3.u32;
    const uint32_t cursor_ptr = r4.u32;
    REXFS_INFO("[AC6 PAC CODEC] bit_fetch bits_req={} cursor_ptr=0x{:x} "
               "cursor=0x{:x} bit_buf=0x{:016x} bit_count={}",
               r5.u32, cursor_ptr,
               LoadGuestU64BE(memory, cursor_ptr),
               LoadGuestU64BE(memory, ctx + 72),
               LoadGuestU32BE(memory, ctx + 80));
}

void ac6PacDynamicHeaderProbe(PPCRegister& r3, PPCRegister& r4) {
    if (!CodecTraceActive()) return;
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;
    const uint32_t ctx = r3.u32;
    REXFS_INFO("[AC6 PAC CODEC] dynamic_header_entry ctx=0x{:08X} cursor_arg=0x{:x} "
               "bit_buf=0x{:016x} bit_count={}",
               ctx, r4.u32,
               LoadGuestU64BE(memory, ctx + 72),
               LoadGuestU32BE(memory, ctx + 80));
}

void ac6PacTreeBuilderProbe(PPCRegister& r3) {
    if (!CodecTraceActive()) return;
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;
    const uint32_t ctx = r3.u32;
    const uint32_t arr_base = ctx + 108;
    constexpr uint32_t kEntries = 19;
    std::string lens;
    lens.reserve(kEntries * 4);
    for (uint32_t i = 0; i < kEntries; ++i) {
        if (i) lens.push_back(',');
        lens += std::to_string(LoadGuestU16BE(memory, arr_base + i * 2));
    }
    REXFS_INFO("[AC6 PAC CODEC] tree_builder_entry ctx=0x{:08X} cl_array=[{}]", ctx, lens);
}

void ac6PacBlockConsumerProbe(PPCRegister& r3) {
    if (!CodecTraceActive()) return;
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory) return;
    const uint32_t ctx = r3.u32;
    REXFS_INFO("[AC6 PAC CODEC] block_consumer_entry ctx=0x{:08X} state=0x{:x} "
               "out_pos=0x{:x} bit_buf=0x{:016x} bit_count={}",
               ctx,
               LoadGuestU32BE(memory, ctx + 84),
               LoadGuestU64BE(memory, ctx + 32),
               LoadGuestU64BE(memory, ctx + 72),
               LoadGuestU32BE(memory, ctx + 80));
}
