// Standalone parser/sanitizer test, with optional user-supplied decoded PAC or
// NTXR inputs. See docs/KEYBOARD_GLYPHS.txt for build/run commands.
#include "ac6_keyboard_glyphs.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <optional>
#include <string_view>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

void StoreBE32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (24 - i * 8));
}

void CheckUnchanged(std::vector<uint8_t> bytes) {
  const auto original = bytes;
  Require(ac6::PatchKeyboardGlyphs(bytes) == 0, "unknown data was matched");
  Require(bytes == original, "unknown data was modified");
}

void SelfTest() {
  for (size_t size = 0; size < 28; ++size) {
    std::vector<uint8_t> bytes(size, 0);
    if (size >= 4) std::copy_n("FHM ", 4, bytes.begin());
    CheckUnchanged(bytes);  // includes empty and truncated headers/tables
  }
  std::vector<uint8_t> bytes(64, 0);
  std::copy_n("FHM ", 4, bytes.begin());
  StoreBE32(bytes, 16, 0xffffffffu);
  CheckUnchanged(bytes);  // malicious count
  StoreBE32(bytes, 16, 1);
  StoreBE32(bytes, 20, 0xfffffff0u);
  StoreBE32(bytes, 24, 32);
  CheckUnchanged(bytes);  // offset overflow/out of range
  StoreBE32(bytes, 20, 28);
  StoreBE32(bytes, 24, 0xffffffffu);
  CheckUnchanged(bytes);  // child size overflow/out of range
  StoreBE32(bytes, 20, 0);
  StoreBE32(bytes, 24, 64);
  CheckUnchanged(bytes);  // child overlapping the container table

  // Deep valid nesting must stop at the recursion limit without a stack fault.
  bytes.assign(4, 0);
  for (int i = 0; i < 100; ++i) {
    std::vector<uint8_t> parent(28 + bytes.size(), 0);
    std::copy_n("FHM ", 4, parent.begin());
    StoreBE32(parent, 16, 1);
    StoreBE32(parent, 20, 28);
    StoreBE32(parent, 24, uint32_t(bytes.size()));
    std::copy(bytes.begin(), bytes.end(), parent.begin() + 28);
    bytes = std::move(parent);
  }
  CheckUnchanged(bytes);
  CheckUnchanged(std::vector<uint8_t>(0x5000, 0));
  bytes.assign(0x5000, 0);
  std::copy_n("NTXR", 4, bytes.begin());
  CheckUnchanged(bytes);  // dimensions alone never authorize a replacement

  std::mt19937 random(0xAC6);
  for (int i = 0; i < 2000; ++i) {
    bytes.resize(random() % 1024);
    for (uint8_t& b : bytes) b = uint8_t(random());
    if (bytes.size() >= 4) std::copy_n(i % 2 ? "FHM " : "NTXR", 4, bytes.begin());
    CheckUnchanged(bytes);
  }
}
}  // namespace

int main(int argc, char** argv) {
  SelfTest();
  size_t total = 0;
  size_t inputs = 0;
  std::optional<size_t> expected;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument.starts_with("--expect=")) {
      size_t count = 0;
      const auto value = argument.substr(9);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
      Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
              "--expect requires an unsigned replacement count");
      expected = count;
      continue;
    }
    std::ifstream input(argv[i], std::ios::binary);
    Require(bool(input), "cannot read supplied texture/PAC");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const auto original = bytes;
    const size_t count = ac6::PatchKeyboardGlyphs(bytes);
    total += count;
    ++inputs;
    Require(ac6::PatchKeyboardGlyphs(bytes) == 0, "patch is not idempotent");
    if (count) {
      Require(bytes.size() == original.size(), "patch resized a resource");
      Require(bytes != original, "matched resource did not change");
      // For standalone NTXR inputs, relocation/fetch metadata must survive.
      if (original.size() >= 0x1000 && std::equal(original.begin(), original.begin() + 4, "NTXR"))
        Require(std::equal(original.begin(), original.begin() + 0x1000, bytes.begin()),
                "texture metadata changed");
    } else {
      Require(bytes == original, "unmatched resource changed");
    }
  }
  Require(!expected || total == *expected, "unexpected number of known texture replacements");
  std::cout << "Parser tests passed; " << total << " known textures replaced in "
            << inputs << " supplied inputs (original files untouched).\n";
}
