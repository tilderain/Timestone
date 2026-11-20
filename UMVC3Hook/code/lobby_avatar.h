#pragma once

// Converts a Steam profile picture into the pixel format of the game's own
// 64x64 icon textures (ui\lic\lic_user_i, TEX format 0x2A), so the native
// lobby can draw it through a sprite like any other icon.
//
// Nothing here parses an image file. The input is the fixed-size block of
// decoded pixels Steam returns for a 64x64 avatar, and the output is a
// fixed-size block; no length or offset is ever taken from the data.

#include <cstddef>
#include <cstdint>

namespace lobby {

constexpr int kAvatarSize = 64;
constexpr size_t kAvatarRgbaBytes = kAvatarSize * kAvatarSize * 4;
// 16 x 16 DXT5 blocks of 16 bytes: one mip of a 64x64 texture.
constexpr size_t kAvatarTextureBytes = (kAvatarSize / 4) * (kAvatarSize / 4) * 16;

// Format 0x2A is DXT5 holding luma in the alpha block and, in the color
// block, Cr in red, the real alpha in green and Cb in blue.
void EncodeAvatarTexture(const uint8_t rgba[kAvatarRgbaBytes],
                         uint8_t out[kAvatarTextureBytes]);

// The same conversion for an image of any size whose sides are multiples of
// four: `rgba` holds width * height * 4 bytes and `out` receives
// width * height bytes (16 per 4x4 block). The alpha channel is kept.
void EncodeIconFormat(const uint8_t* rgba, int width, int height, uint8_t* out);

} // namespace lobby
