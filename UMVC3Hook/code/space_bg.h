#pragma once

// The lobby's space backdrop: a starfield floor and ceiling receding to a
// horizon across the middle of the screen, under a slowly scrolling grid
// whose colour cycles. It is drawn from the numbers of the layout the Xbox
// 360 game used for its lobby (ui\lic\lic_bg\a_lic_bg, see
// doc/native_lobby_ui.md), with art of the plugin's own.

#include "lobby_canvas.h"

#include <cstddef>
#include <cstdint>

namespace lobby {

// The embedded star tile (UMVC3Hook/assets/space_stars_512.dxt1): raw DXT1
// blocks of one 512x512 image that tiles in both directions.
constexpr int kSpaceStarsTile = 512;
constexpr size_t kSpaceStarsTileBytes = (kSpaceStarsTile / 4) * (kSpaceStarsTile / 4) * 8;
// The texture drawn from: the tile twice across and twice down, so that every
// window the backdrop samples lies inside it and nothing depends on how the
// sprite sampler treats coordinates past the edge.
constexpr int kSpaceStarsSize = kSpaceStarsTile * 2;
constexpr size_t kSpaceStarsRgbaBytes =
    static_cast<size_t>(kSpaceStarsSize) * kSpaceStarsSize * 4;

// Decodes the tile into the star texture's pixels (kSpaceStarsRgbaBytes of
// RGBA). False if `size` is not exactly one tile.
bool BuildSpaceStarsRgba(const uint8_t* tile, size_t size, uint8_t* rgba);

// Draws the whole backdrop over the screen. `frame` advances once per
// displayed frame; `alpha` fades it in over what is behind (0..1).
void DrawSpaceBackground(Canvas& canvas, uint32_t frame, float alpha);

} // namespace lobby
