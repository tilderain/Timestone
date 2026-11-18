#include "lobby_avatar.h"

#include <cstring>

namespace lobby {

namespace {

uint8_t ClampByte(float value)
{
    return value <= 0.0f ? 0 : value >= 255.0f ? 255 : static_cast<uint8_t>(value + 0.5f);
}

// DXT5 alpha block: two endpoints, six interpolated values, 3-bit indices.
void EncodeAlphaBlock(const uint8_t values[16], uint8_t out[8])
{
    uint8_t low = 255, high = 0;
    for (int i = 0; i < 16; ++i) {
        if (values[i] < low) low = values[i];
        if (values[i] > high) high = values[i];
    }
    out[0] = high; // alpha0 > alpha1 selects the eight-value mode
    out[1] = low;
    uint64_t bits = 0;
    if (high != low) {
        // Palette order: a0, a1, then six steps from a0 down to a1.
        const int range = high - low;
        for (int i = 0; i < 16; ++i) {
            // Position 0..7 from high (0) to low (7), rounded.
            const int step = ((high - values[i]) * 7 + range / 2) / range;
            const int index = step == 0 ? 0 : step == 7 ? 1 : step + 1;
            bits |= static_cast<uint64_t>(index) << (3 * i);
        }
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

uint16_t Pack565(int r, int g, int b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Unpack565(uint16_t c, int rgb[3])
{
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// DXT color block: endpoints from the block's bounding box, 2-bit indices.
void EncodeColorBlock(const uint8_t pixels[16][3], uint8_t out[8])
{
    int low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            if (pixels[i][c] < low[c]) low[c] = pixels[i][c];
            if (pixels[i][c] > high[c]) high[c] = pixels[i][c];
        }
    }
    uint16_t c0 = Pack565(high[0], high[1], high[2]);
    uint16_t c1 = Pack565(low[0], low[1], low[2]);
    if (c0 < c1) {
        const uint16_t swap = c0;
        c0 = c1;
        c1 = swap;
    }
    out[0] = static_cast<uint8_t>(c0);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    uint32_t bits = 0;
    if (c0 != c1) {
        // In DXT5 the color block always has four colors: c0, c1 and the two
        // thirds between them.
        int palette[4][3];
        Unpack565(c0, palette[0]);
        Unpack565(c1, palette[1]);
        for (int c = 0; c < 3; ++c) {
            palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
            palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 0, bestError = 0x7FFFFFFF;
            for (int k = 0; k < 4; ++k) {
                int error = 0;
                for (int c = 0; c < 3; ++c) {
                    const int d = pixels[i][c] - palette[k][c];
                    error += d * d;
                }
                if (error < bestError) {
                    bestError = error;
                    best = k;
                }
            }
            bits |= static_cast<uint32_t>(best) << (2 * i);
        }
    }
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

} // namespace

void EncodeAvatarTexture(const uint8_t rgba[kAvatarRgbaBytes],
                         uint8_t out[kAvatarTextureBytes])
{
    for (int by = 0; by < kAvatarSize / 4; ++by) {
        for (int bx = 0; bx < kAvatarSize / 4; ++bx) {
            uint8_t luma[16];
            uint8_t chroma[16][3];
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const uint8_t* pixel =
                        rgba + ((by * 4 + py) * kAvatarSize + bx * 4 + px) * 4;
                    const float r = pixel[0], g = pixel[1], b = pixel[2];
                    // The inverse of the shader's decode:
                    //   R = Y + 1.402 Cr, G = Y - 0.344 Cb - 0.714 Cr, B = Y + 1.772 Cb
                    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
                    const int i = py * 4 + px;
                    luma[i] = ClampByte(y);
                    chroma[i][0] = ClampByte((r - y) / 1.402f + 128.0f); // Cr
                    chroma[i][1] = 255;                                  // opaque
                    chroma[i][2] = ClampByte((b - y) / 1.772f + 128.0f); // Cb
                }
            }
            uint8_t* block = out + (by * (kAvatarSize / 4) + bx) * 16;
            EncodeAlphaBlock(luma, block);
            EncodeColorBlock(chroma, block + 8);
        }
    }
}

} // namespace lobby
