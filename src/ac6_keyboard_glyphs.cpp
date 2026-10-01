#include "ac6_keyboard_glyphs.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace ac6 {
namespace {

constexpr size_t kTextureSize = 0x5000;
constexpr size_t kPayloadOffset = 0x1000;

uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Fingerprint(std::span<const uint8_t> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t b : bytes) hash = (hash ^ b) * 1099511628211ull;
  return hash;
}

enum class Symbol { MouseLeft, MouseRight, R, T, Q, E };
struct Glyph {
  uint64_t fingerprint;
  uint16_t width, height;
  Symbol symbol;
};

// Exact whole-resource fingerprints verified against the US DATA.TBL corpus.
// The six textures recur in save/load, menus, hangar and tutorial resources.
// Width/format alone would also match flags, weapon art and other UI sprites.
constexpr Glyph kGlyphs[] = {
    {0x24838721e175ef3cull, 40, 40, Symbol::MouseLeft},
    {0x9b5901a94e162709ull, 40, 40, Symbol::MouseRight},
    {0xe0c9ff3f53daa179ull, 40, 40, Symbol::R},
    {0x383498ee912c7a9cull, 40, 40, Symbol::T},
    {0xb52fdf9287ed2671ull, 64, 32, Symbol::Q},
    {0x506279cc469d6b74ull, 64, 32, Symbol::E},
};

struct Pixel { uint8_t gray = 0, alpha = 0; };

Pixel Sample(Symbol symbol, int x, int y, int width) {
  const bool mouse = symbol == Symbol::MouseLeft || symbol == Symbol::MouseRight;
  if (mouse) {
    // Keep the artwork inside the original icon's 32x32 sampling rectangle.
    const int left = 10, right = 29, top = 5, bottom = 34;
    if (x < left || x > right || y < top || y > bottom) return {};
    if ((y <= top + 2 || y >= bottom - 2) &&
        (x <= left + 2 || x >= right - 2)) return {};
    if (x <= left + 1 || x >= right - 1 || y <= top + 1 ||
        y >= bottom - 1 || y == 19 || (x == 20 && y <= 19))
      return {235, 255};
    const bool selected = symbol == Symbol::MouseLeft ? x < 20 : x > 20;
    if (selected && y < 19) return {210, 255};
    return {32, 255};
  }

  const int left = width == 64 ? 9 : 5;
  const int right = width == 64 ? 54 : 34;
  const int top = width == 64 ? 4 : 5;
  const int bottom = width == 64 ? 27 : 34;
  if (x < left || x > right || y < top || y > bottom) return {};
  if ((x == left || x == right) && (y == top || y == bottom)) return {};
  if (x <= left + 1 || x >= right - 1 || y <= top + 1 || y >= bottom - 1)
    return {235, 255};

  // A tiny code-native 5x7 font. Integer scaling keeps labels sharp at 720p.
  std::array<uint8_t, 7> rows{};
  switch (symbol) {
    case Symbol::R: rows = {30, 17, 17, 30, 20, 18, 17}; break;
    case Symbol::T: rows = {31, 4, 4, 4, 4, 4, 4}; break;
    case Symbol::Q: rows = {14, 17, 17, 17, 21, 18, 13}; break;
    case Symbol::E: rows = {31, 16, 16, 30, 16, 16, 31}; break;
    default: break;
  }
  const int gx = x - (width - 10) / 2;
  const int gy = y - (top + bottom + 1 - 14) / 2;
  if (gx >= 0 && gx < 10 && gy >= 0 && gy < 14 &&
      (rows[gy / 2] & (1u << (4 - gx / 2)))) return {235, 255};
  return {32, 255};
}

uint16_t Gray565(uint8_t gray) {
  return uint16_t(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
}

uint8_t DecodeGray565(uint16_t value) {
  const unsigned red = (value >> 11) & 31;
  return uint8_t((red << 3) | (red >> 2));
}

// BC3/DXT5 with grayscale RGB endpoints and independently compressed alpha.
// Emit host little-endian blocks; Xbox assets swap each 16-bit word below.
std::array<uint8_t, 16> EncodeBlock(const std::array<Pixel, 16>& pixels) {
  std::array<uint8_t, 16> block{};
  block[0] = 255;
  block[1] = 0;
  uint64_t alpha_bits = 0;
  uint8_t low = 255, high = 0;
  for (size_t i = 0; i < pixels.size(); ++i) {
    alpha_bits |= uint64_t(pixels[i].alpha ? 0 : 1) << (3 * i);
    if (pixels[i].alpha) {
      low = std::min(low, pixels[i].gray);
      high = std::max(high, pixels[i].gray);
    }
  }
  for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t(alpha_bits >> (8 * i));
  if (low > high) low = high = 0;  // fully transparent block
  const uint16_t c0 = Gray565(high), c1 = Gray565(low);
  block[8] = uint8_t(c0);
  block[9] = uint8_t(c0 >> 8);
  block[10] = uint8_t(c1);
  block[11] = uint8_t(c1 >> 8);
  const int hi = DecodeGray565(c0), lo = DecodeGray565(c1);
  const int palette[] = {hi, lo, (2 * hi + lo) / 3, (hi + 2 * lo) / 3};
  uint32_t color_bits = 0;
  for (size_t i = 0; i < pixels.size(); ++i) {
    unsigned best = 0;
    int distance = 256;
    for (unsigned j = 0; j < 4; ++j) {
      const int delta = int(pixels[i].gray) - palette[j];
      const int candidate = delta < 0 ? -delta : delta;
      if (candidate < distance) { distance = candidate; best = j; }
    }
    color_bits |= best << (2 * i);
  }
  for (int i = 0; i < 4; ++i) block[12 + i] = uint8_t(color_bits >> (8 * i));
  return block;
}

// Xbox 360 2D tiled address, pitch=32 BC3 blocks, log2(bytes/block)=4.
// Equivalent to texture_util::GetTiledOffset2D; no graphics dependency here.
size_t BlockOffset(int x, int y) {
  const int macro = ((x >> 5) + (y >> 5)) << 11;
  const int micro = ((x & 7) + ((y & 14) << 2)) << 4;
  const int offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 1) << 4);
  return size_t(((offset & ~511) << 3) + ((y & 16) << 7) +
                ((offset & 448) << 2) +
                (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63));
}

void WriteGlyph(std::span<uint8_t> texture, const Glyph& glyph) {
  for (int by = 0; by < (glyph.height + 3) / 4; ++by) {
    for (int bx = 0; bx < (glyph.width + 3) / 4; ++bx) {
      std::array<Pixel, 16> pixels{};
      for (int py = 0; py < 4; ++py)
        for (int px = 0; px < 4; ++px)
          pixels[py * 4 + px] = Sample(glyph.symbol, bx * 4 + px, by * 4 + py,
                                      glyph.width);
      auto block = EncodeBlock(pixels);
      for (size_t i = 0; i < block.size(); i += 2) std::swap(block[i], block[i + 1]);
      const size_t offset = kPayloadOffset + BlockOffset(bx, by);
      // All six fingerprinted resources have the same 16 KiB tiled payload.
      std::memcpy(texture.data() + offset, block.data(), block.size());
    }
  }
}

size_t Patch(std::span<uint8_t> bytes, unsigned depth) {
  if (bytes.size() < 4) return 0;
  if (std::memcmp(bytes.data(), "NTXR", 4) == 0) {
    if (bytes.size() != kTextureSize) return 0;
    const uint64_t hash = Fingerprint(bytes);
    for (const Glyph& glyph : kGlyphs) {
      if (hash == glyph.fingerprint) { WriteGlyph(bytes, glyph); return 1; }
    }
    return 0;
  }
  if (depth >= 8 || bytes.size() < 20 || std::memcmp(bytes.data(), "FHM ", 4) != 0)
    return 0;
  const uint32_t count = ReadBE32(bytes.data() + 16);
  if (count > (bytes.size() - 20) / 8) return 0;
  const size_t table_end = 20 + size_t(count) * 8;
  size_t patched = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t offset = ReadBE32(bytes.data() + 20 + size_t(i) * 4);
    const size_t size = ReadBE32(bytes.data() + 20 + size_t(count + i) * 4);
    if (offset < table_end || offset > bytes.size() || size > bytes.size() - offset)
      continue;
    patched += Patch(bytes.subspan(offset, size), depth + 1);
  }
  return patched;
}

}  // namespace

size_t PatchKeyboardGlyphs(std::span<uint8_t> decoded) { return Patch(decoded, 0); }

}  // namespace ac6
