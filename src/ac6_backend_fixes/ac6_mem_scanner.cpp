// Developer memory scanner: find where the game keeps a value that is visible
// on screen (missile count, credits, damage), the first step to finding - and
// hooking - the code that changes it.
//
// Off unless ac6_dev_scanner is set. A background thread polls `ac6_scan_cmd`
// in the working directory; every line written there is a command, and the
// results are appended to `ac6_scan_out.txt` next to it. For example:
//
//   echo "scan u16 64" > ac6_scan_cmd      # every u16 == 64 in guest memory
//   echo "next 63" > ac6_scan_cmd          # ...of those, the ones now 63
//
// Commands (numbers are decimal or 0x-hex; guest memory is big-endian and is
// compared as such):
//   scan <u8|u16|u32|f32> <value> [tolerance]  new search of committed memory
//   next <value>|dec|inc|same|changed          narrow the current candidates
//   list [n]                                   print up to n candidates
//   peek <addr> [words]                        dump big-endian words
//   ptrto <addr> [range]                       find u32s pointing into
//                                              [addr - range, addr]
//   poke <type> <addr> <value>                 write once
//   freeze <type> <addr> <value>               rewrite every 50 ms
//   unfreeze <addr>|all
//   ortho <addr> [bytes]                       find rotation matrices
//
// Reads go through process_vm_readv (ReadProcessMemory on Windows), so a page
// that is decommitted mid-scan fails the read instead of faulting, and only
// committed guest regions are ever read - reading the rest of the mapping would
// fault in fresh shared-memory backing for nothing.

#include "ac6_mem_scanner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(ac6_dev_scanner, false, "AC6/Debug",
                    "Developer memory scanner, driven through the ac6_scan_cmd file in the "
                    "working directory (reverse-engineering aid)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly)
    .debug_only();

namespace ac6::devtools {
namespace {

constexpr const char* kCommandFile = "ac6_scan_cmd";
constexpr const char* kOutputFile = "ac6_scan_out.txt";
constexpr size_t kChunkBytes = 1u << 20;
constexpr size_t kMaxCandidates = 20'000'000;

enum class ValueType { kU8, kU16, kU32, kF32 };

size_t SizeOf(ValueType type) {
  switch (type) {
    case ValueType::kU8:
      return 1;
    case ValueType::kU16:
      return 2;
    default:
      return 4;
  }
}

const char* NameOf(ValueType type) {
  switch (type) {
    case ValueType::kU8:
      return "u8";
    case ValueType::kU16:
      return "u16";
    case ValueType::kU32:
      return "u32";
    default:
      return "f32";
  }
}

bool ParseType(const std::string& text, ValueType* out) {
  if (text == "u8") {
    *out = ValueType::kU8;
  } else if (text == "u16") {
    *out = ValueType::kU16;
  } else if (text == "u32") {
    *out = ValueType::kU32;
  } else if (text == "f32") {
    *out = ValueType::kF32;
  } else {
    return false;
  }
  return true;
}

bool ParseNumber(const std::string& text, double* out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    *out = double(std::strtoull(text.c_str() + 2, &end, 16));
  } else {
    *out = std::strtod(text.c_str(), &end);
  }
  return end && *end == '\0';
}

// Copies guest-backing host memory without faulting; returns the bytes read.
size_t SafeRead(const void* src, void* dst, size_t bytes) {
#if defined(_WIN32)
  SIZE_T got = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), src, dst, bytes, &got)) {
    return 0;
  }
  return size_t(got);
#else
  iovec local{dst, bytes};
  iovec remote{const_cast<void*>(src), bytes};
  const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  return got < 0 ? 0 : size_t(got);
#endif
}

// Decodes a big-endian guest value.
double Decode(ValueType type, const uint8_t* p) {
  switch (type) {
    case ValueType::kU8:
      return p[0];
    case ValueType::kU16:
      return double((uint32_t(p[0]) << 8) | p[1]);
    case ValueType::kU32:
      return double((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]);
    default: {
      const uint32_t bits =
          (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      return std::isfinite(value) ? double(value) : std::nan("");
    }
  }
}

void Encode(ValueType type, double value, uint8_t* p) {
  uint32_t bits;
  if (type == ValueType::kF32) {
    const float f = float(value);
    std::memcpy(&bits, &f, sizeof(bits));
  } else {
    bits = uint32_t(int64_t(value));
  }
  const size_t size = SizeOf(type);
  for (size_t i = 0; i < size; ++i) {
    p[i] = uint8_t(bits >> (8 * (size - 1 - i)));
  }
}

struct Frozen {
  ValueType type;
  uint32_t address;
  double value;
};

class Scanner {
 public:
  explicit Scanner(rex::memory::Memory* memory) : memory_(memory) {}

  void Run() {
    const auto command_path = std::filesystem::absolute(kCommandFile);
    REXLOG_ERROR("[AC6-SCAN] developer scanner active: commands in {}, results in {}",
                 command_path.string(), std::filesystem::absolute(kOutputFile).string());
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ApplyFreezes();
      std::error_code ec;
      if (!std::filesystem::exists(command_path, ec) ||
          std::filesystem::file_size(command_path, ec) == 0) {
        continue;
      }
      std::vector<std::string> lines;
      {
        std::ifstream in(command_path);
        for (std::string line; std::getline(in, line);) {
          lines.push_back(line);
        }
      }
      std::filesystem::remove(command_path, ec);
      for (const auto& line : lines) {
        Execute(line);
      }
    }
  }

 private:
  // Calls fn(guest_base, bytes) for every committed, readable guest region -
  // the guest heaps' own view, not the host mapping, which is read-write over
  // all of it.
  template <typename Fn>
  void ForEachCommittedRegion(Fn&& fn) {
    uint64_t guest = 0x00010000;
    constexpr uint64_t kEnd = 0x100000000ull;
    while (guest < kEnd) {
      rex::memory::BaseHeap* heap = memory_->LookupHeap(uint32_t(guest));
      if (!heap) {
        guest = (guest | 0xFFFFFFull) + 1;
        continue;
      }
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(guest), &info) || !info.region_size) {
        guest = uint64_t(heap->heap_base()) + heap->heap_size();
        continue;
      }
      const uint64_t region_end = std::min<uint64_t>(guest + info.region_size, kEnd);
      if ((info.state & rex::memory::kMemoryAllocationCommit) &&
          (info.protect & rex::memory::kMemoryProtectRead)) {
        fn(uint32_t(guest), uint32_t(region_end - guest));
      }
      guest = region_end;
    }
  }

  bool ReadValue(ValueType type, uint32_t address, double* out) {
    uint8_t bytes[4];
    const size_t size = SizeOf(type);
    if (SafeRead(memory_->TranslateVirtual(address), bytes, size) != size) {
      return false;
    }
    *out = Decode(type, bytes);
    return true;
  }

  bool Matches(double value, double target) const {
    if (type_ != ValueType::kF32) {
      return value == target;
    }
    return std::abs(value - target) <= tolerance_;
  }

  void Scan(ValueType type, double target, double tolerance) {
    type_ = type;
    tolerance_ = tolerance;
    candidates_.clear();
    values_.clear();
    const size_t size = SizeOf(type);
    std::vector<uint8_t> chunk(kChunkBytes);
    size_t scanned = 0;
    bool truncated = false;
    ForEachCommittedRegion([&](uint32_t base, uint32_t bytes) {
      for (uint64_t offset = 0; offset < bytes && !truncated; offset += kChunkBytes) {
        const size_t want = size_t(std::min<uint64_t>(kChunkBytes, bytes - offset));
        const size_t got =
            SafeRead(memory_->TranslateVirtual(uint32_t(base + offset)), chunk.data(), want);
        scanned += got;
        for (size_t i = 0; i + size <= got; i += size) {
          const double value = Decode(type, chunk.data() + i);
          if (Matches(value, target)) {
            if (candidates_.size() >= kMaxCandidates) {
              truncated = true;
              break;
            }
            candidates_.push_back(uint32_t(base + offset + i));
            values_.push_back(value);
          }
        }
      }
    });
    Print(fmt::format("scan {} {} (tolerance {}): {} match(es) in {} MiB{}", NameOf(type), target,
                      tolerance, candidates_.size(), scanned >> 20,
                      truncated ? " - TRUNCATED, use a rarer value" : ""));
    List(20);
  }

  void Next(const std::string& how) {
    double target = 0;
    const bool by_value = ParseNumber(how, &target);
    if (!by_value && how != "dec" && how != "inc" && how != "same" && how != "changed") {
      Print("next: expected a value or dec|inc|same|changed");
      return;
    }
    std::vector<uint32_t> kept;
    std::vector<double> kept_values;
    for (size_t i = 0; i < candidates_.size(); ++i) {
      double value;
      if (!ReadValue(type_, candidates_[i], &value)) {
        continue;
      }
      const double old_value = values_[i];
      bool keep;
      if (by_value) {
        keep = Matches(value, target);
      } else if (how == "dec") {
        keep = value < old_value;
      } else if (how == "inc") {
        keep = value > old_value;
      } else if (how == "same") {
        keep = value == old_value;
      } else {
        keep = value != old_value;
      }
      if (keep) {
        kept.push_back(candidates_[i]);
        kept_values.push_back(value);
      }
    }
    candidates_ = std::move(kept);
    values_ = std::move(kept_values);
    Print(fmt::format("next {}: {} candidate(s) left", how, candidates_.size()));
    List(20);
  }

  void List(size_t limit) {
    const size_t n = std::min(limit, candidates_.size());
    for (size_t i = 0; i < n; ++i) {
      double now = std::nan("");
      ReadValue(type_, candidates_[i], &now);
      Print(fmt::format("  {:08X}  {} = {}", candidates_[i], NameOf(type_), now));
    }
    if (candidates_.size() > n) {
      Print(fmt::format("  ... {} more", candidates_.size() - n));
    }
  }

  void Peek(uint32_t address, uint32_t words) {
    for (uint32_t row = 0; row < words; row += 4) {
      std::string line = fmt::format("  {:08X}:", address + row * 4);
      for (uint32_t i = row; i < std::min(words, row + 4); ++i) {
        double value;
        if (ReadValue(ValueType::kU32, address + i * 4, &value)) {
          line += fmt::format(" {:08X}", uint32_t(value));
        } else {
          line += " ????????";
        }
      }
      Print(line);
    }
  }

  void PointersTo(uint32_t target, uint32_t range) {
    const uint32_t low = target >= range ? target - range : 0;
    size_t found = 0;
    std::vector<uint8_t> chunk(kChunkBytes);
    ForEachCommittedRegion([&](uint32_t base, uint32_t bytes) {
      for (uint64_t offset = 0; offset < bytes; offset += kChunkBytes) {
        const size_t want = size_t(std::min<uint64_t>(kChunkBytes, bytes - offset));
        const size_t got =
            SafeRead(memory_->TranslateVirtual(uint32_t(base + offset)), chunk.data(), want);
        for (size_t i = 0; i + 4 <= got; i += 4) {
          const uint32_t value = uint32_t(Decode(ValueType::kU32, chunk.data() + i));
          if (value >= low && value <= target && found < 200) {
            ++found;
            Print(fmt::format("  {:08X} -> {:08X} (+0x{:X})", uint32_t(base + offset + i), value,
                              target - value));
          }
        }
      }
    });
    Print(fmt::format("ptrto {:08X} range 0x{:X}: {} hit(s) shown", target, range, found));
  }

  // Rotation matrices in [address, address + bytes): three unit-length,
  // mutually orthogonal float rows, packed (stride 12) or padded to vec4
  // (stride 16). Axis-aligned ones (all entries 0 or +-1) are skipped: they
  // are mostly identity defaults rather than a live orientation.
  void Ortho(uint32_t address, uint32_t bytes) {
    std::vector<uint8_t> data(bytes);
    const size_t got = SafeRead(memory_->TranslateVirtual(address), data.data(), bytes);
    auto f = [&](size_t at) { return float(Decode(ValueType::kF32, data.data() + at)); };
    size_t found = 0;
    for (size_t at = 0; at + 48 <= got && found < 100; at += 4) {
      for (size_t stride : {size_t(12), size_t(16)}) {
        if (at + 2 * stride + 12 > got) continue;
        float m[3][3];
        bool unit = true, trivial = true;
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 3; ++c) {
            m[r][c] = f(at + r * stride + c * 4);
            const float v = std::fabs(m[r][c]);
            if (!(v < 1.001f)) unit = false;
            if (v > 1e-4f && std::fabs(v - 1.0f) > 1e-4f) trivial = false;
          }
          const float len = m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2];
          if (std::fabs(len - 1.0f) > 2e-3f) unit = false;
        }
        if (!unit || trivial) continue;
        auto dot = [&](int i, int j) {
          return std::fabs(m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2]);
        };
        if (dot(0, 1) > 2e-3f || dot(0, 2) > 2e-3f || dot(1, 2) > 2e-3f) continue;
        ++found;
        Print(fmt::format("  {:08X} (+0x{:X}) stride {}: [{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} "
                          "{:.3f}] [{:.3f} {:.3f} {:.3f}]",
                          uint32_t(address + at), at, stride, m[0][0], m[0][1], m[0][2], m[1][0],
                          m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]));
      }
    }
    Print(fmt::format("ortho {:08X} +0x{:X}: {} matrix(es)", address, bytes, found));
  }

  bool Writable(uint32_t address, size_t size) {
    rex::memory::BaseHeap* heap = memory_->LookupHeap(address);
    if (!heap) {
      return false;
    }
    const auto access = heap->QueryRangeAccess(address, uint32_t(address + size - 1));
    return access == rex::memory::PageAccess::kReadWrite ||
           access == rex::memory::PageAccess::kExecuteReadWrite;
  }

  void Write(ValueType type, uint32_t address, double value) {
    uint8_t bytes[4];
    Encode(type, value, bytes);
    // A plain store, like any guest write: on a GPU-watched page it faults
    // into the runtime's handler, which invalidates and retries it.
    std::memcpy(memory_->TranslateVirtual(address), bytes, SizeOf(type));
  }

  void ApplyFreezes() {
    for (const auto& frozen : frozen_) {
      Write(frozen.type, frozen.address, frozen.value);
    }
  }

  void Execute(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> args;
    for (std::string arg; in >> arg;) {
      args.push_back(arg);
    }
    if (args.empty()) {
      return;
    }
    Print("> " + line);
    const std::string& cmd = args[0];
    ValueType type;
    double a = 0, b = 0;
    if (cmd == "scan" && args.size() >= 3 && ParseType(args[1], &type) &&
        ParseNumber(args[2], &a)) {
      if (args.size() < 4 || !ParseNumber(args[3], &b)) {
        b = type == ValueType::kF32 ? 0.01 : 0;
      }
      Scan(type, a, b);
    } else if (cmd == "next" && args.size() >= 2) {
      Next(args[1]);
    } else if (cmd == "list") {
      List(args.size() >= 2 && ParseNumber(args[1], &a) ? size_t(a) : 40);
    } else if (cmd == "peek" && args.size() >= 2 && ParseNumber(args[1], &a)) {
      Peek(uint32_t(a), args.size() >= 3 && ParseNumber(args[2], &b) ? uint32_t(b) : 16);
    } else if (cmd == "ptrto" && args.size() >= 2 && ParseNumber(args[1], &a)) {
      PointersTo(uint32_t(a), args.size() >= 3 && ParseNumber(args[2], &b) ? uint32_t(b) : 0x400);
    } else if (cmd == "ortho" && args.size() >= 2 && ParseNumber(args[1], &a)) {
      Ortho(uint32_t(a), args.size() >= 3 && ParseNumber(args[2], &b) ? uint32_t(b) : 0x4000);
    } else if ((cmd == "poke" || cmd == "freeze") && args.size() >= 4 &&
               ParseType(args[1], &type) && ParseNumber(args[2], &a) && ParseNumber(args[3], &b)) {
      const uint32_t address = uint32_t(a);
      if (!Writable(address, SizeOf(type))) {
        Print(fmt::format("{:08X} is not writable guest memory", address));
        return;
      }
      Write(type, address, b);
      if (cmd == "freeze") {
        frozen_.push_back({type, address, b});
      }
      Print(fmt::format("{} {} {:08X} = {}", cmd, NameOf(type), address, b));
    } else if (cmd == "unfreeze" && args.size() >= 2) {
      if (args[1] == "all") {
        frozen_.clear();
      } else if (ParseNumber(args[1], &a)) {
        std::erase_if(frozen_, [&](const Frozen& f) { return f.address == uint32_t(a); });
      }
      Print(fmt::format("{} value(s) still frozen", frozen_.size()));
    } else {
      Print("unrecognized command");
    }
  }

  void Print(const std::string& text) {
    std::ofstream out(kOutputFile, std::ios::app);
    out << text << '\n';
  }

  rex::memory::Memory* memory_;
  ValueType type_ = ValueType::kU32;
  double tolerance_ = 0;
  std::vector<uint32_t> candidates_;
  std::vector<double> values_;
  std::vector<Frozen> frozen_;
};

}  // namespace

void StartMemScanner(rex::memory::Memory* memory) {
  if (!REXCVAR_GET(ac6_dev_scanner) || !memory) {
    return;
  }
  static std::atomic<bool> started{false};
  if (started.exchange(true)) {
    return;
  }
  std::thread([memory] { Scanner(memory).Run(); }).detach();
}

}  // namespace ac6::devtools
