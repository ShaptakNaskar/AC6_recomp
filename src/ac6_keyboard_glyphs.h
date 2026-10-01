#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ac6 {

// Replace only fingerprinted, unrelocated NTXR button textures inside a decoded
// PAC/FHM buffer. No filesystem writes, renderer dependency, or original art.
// Returns the number of textures replaced; unknown resources pass through.
size_t PatchKeyboardGlyphs(std::span<uint8_t> decoded);

}  // namespace ac6
