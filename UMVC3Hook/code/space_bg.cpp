#include "space_bg.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace lobby {

namespace {

constexpr float kScreenW = 1280.0f, kScreenH = 720.0f;
constexpr float kCentreX = kScreenW * 0.5f, kCentreY = kScreenH * 0.5f;
constexpr Uv kFillUv{0.25f, 0.25f, 0.75f, 0.75f}; // the middle of TEX_WHITE

// ---------------------------------------------------------------------------
// The scene, in the units of the Xbox model (a_lic_bg.mod). The viewer stands
// kCameraZ in front of the model's origin looking down -z, so a point
// (x, y, z) lands on the screen at centre + kFocal * (x, -y) / (kCameraZ - z).
// The game's own view for the model was not recovered; this one is a 45
// degree vertical field placed so that the star planes' far edge, 1160 units
// either side at z -700, exactly spans the screen. That is also what makes
// the planes' texture coordinates (u 0.53..2.47) come out edge to edge.
// ---------------------------------------------------------------------------
constexpr float kFocal = 869.1f;   // 360 / tan(22.5 degrees)
constexpr float kCameraZ = 876.0f; // 1160 * kFocal / 640 - 700
constexpr uint32_t kLoopFrames = 6000; // the layout's timeline: 100 s at 60 fps

// Star planes: |x| <= 1160, |y| 0..1160, from z -700 on the centre line to
// z -450 at the outer edge. Texture: u 0.53..2.47 across, v 1.5 (centre) to
// 0.5 (edge).
constexpr float kStarFarZ = 700.0f;
constexpr float kStarSlope = 250.0f / 1160.0f; // |z| lost per unit of |y|
constexpr float kStarHalfWidth = 1160.0f;
constexpr float kStarUHalf = 0.97f;
constexpr int kStarStrips = 18; // per half

// Grid sheets. The mesh is a lattice half a tile apart in both directions,
// 25 points across (u 0..12) by rows v 0, 0.5 .. 3 and then one at v 6, and
// the texture is a cross with a line on each tile edge and through its
// middle: so the grid's lines are exactly the lattice's edges. The rows are
// arcs that widen with distance, which is what bends the grid like a map's
// graticule. Each point: x, y and depth (-z) of the upper sheet; the lower
// one is its mirror image. Vertex alpha is v / 3: nothing on the innermost
// row, solid from the row at v 3.
constexpr int kGridColumns = 25;
constexpr int kGridRows = 7; // v 0 .. 3; the row at v 6 is far off screen
constexpr float kGridSolidV = 3.0f;
constexpr int16_t kGridMesh[kGridRows][kGridColumns][3] = {
    {{-1269, 127, 500}, {-1161, 136, 500}, {-1053, 145, 500}, {-946, 154, 500}, {-840, 162, 500}, {-734, 169, 500}, {-628, 176, 500}, {-523, 182, 500}, {-418, 187, 500}, {-313, 191, 500}, {-209, 194, 500}, {-104, 195, 500}, {0, 196, 500}, {104, 195, 500}, {209, 194, 500}, {313, 191, 500}, {418, 187, 500}, {523, 182, 500}, {628, 176, 500}, {734, 169, 500}, {840, 162, 500}, {946, 154, 500}, {1053, 145, 500}, {1161, 136, 500}, {1269, 127, 500}},
    {{-1362, 191, 492}, {-1246, 203, 492}, {-1131, 215, 492}, {-1017, 227, 492}, {-903, 239, 492}, {-789, 251, 492}, {-676, 262, 492}, {-563, 271, 492}, {-450, 280, 492}, {-337, 286, 492}, {-225, 291, 492}, {-112, 294, 492}, {0, 295, 492}, {112, 294, 492}, {225, 291, 492}, {337, 286, 492}, {450, 280, 492}, {563, 271, 492}, {676, 262, 492}, {789, 251, 492}, {903, 239, 492}, {1017, 227, 492}, {1131, 215, 492}, {1246, 203, 492}, {1362, 191, 492}},
    {{-1455, 255, 485}, {-1332, 269, 485}, {-1209, 285, 485}, {-1087, 301, 485}, {-965, 317, 485}, {-844, 333, 485}, {-723, 347, 485}, {-602, 361, 485}, {-481, 373, 485}, {-361, 382, 485}, {-241, 389, 485}, {-120, 393, 485}, {0, 395, 485}, {120, 393, 485}, {241, 389, 485}, {361, 382, 485}, {481, 373, 485}, {602, 361, 485}, {723, 347, 485}, {844, 333, 485}, {965, 317, 485}, {1087, 301, 485}, {1209, 285, 485}, {1332, 269, 485}, {1455, 255, 485}},
    {{-1547, 318, 477}, {-1417, 336, 477}, {-1287, 355, 477}, {-1158, 375, 477}, {-1028, 395, 477}, {-899, 414, 477}, {-770, 433, 477}, {-642, 450, 477}, {-513, 465, 477}, {-385, 478, 477}, {-256, 486, 477}, {-128, 492, 477}, {0, 494, 477}, {128, 492, 477}, {256, 486, 477}, {385, 478, 477}, {513, 465, 477}, {642, 450, 477}, {770, 433, 477}, {899, 414, 477}, {1028, 395, 477}, {1158, 375, 477}, {1287, 355, 477}, {1417, 336, 477}, {1547, 318, 477}},
    {{-1640, 382, 469}, {-1502, 403, 469}, {-1365, 425, 469}, {-1228, 448, 469}, {-1091, 472, 469}, {-954, 496, 469}, {-818, 519, 469}, {-681, 540, 469}, {-545, 558, 469}, {-409, 573, 469}, {-272, 584, 469}, {-136, 591, 469}, {0, 593, 469}, {136, 591, 469}, {272, 584, 469}, {409, 573, 469}, {545, 558, 469}, {681, 540, 469}, {818, 519, 469}, {954, 496, 469}, {1091, 472, 469}, {1228, 448, 469}, {1365, 425, 469}, {1502, 403, 469}, {1640, 382, 469}},
    {{-1732, 446, 461}, {-1588, 469, 461}, {-1443, 495, 461}, {-1298, 522, 461}, {-1154, 550, 461}, {-1009, 578, 461}, {-865, 605, 461}, {-721, 629, 461}, {-577, 651, 461}, {-432, 669, 461}, {-288, 682, 461}, {-144, 690, 461}, {0, 692, 461}, {144, 690, 461}, {288, 682, 461}, {432, 669, 461}, {577, 651, 461}, {721, 629, 461}, {865, 605, 461}, {1009, 578, 461}, {1154, 550, 461}, {1298, 522, 461}, {1443, 495, 461}, {1588, 469, 461}, {1732, 446, 461}},
    {{-1825, 509, 454}, {-1673, 536, 454}, {-1521, 565, 454}, {-1369, 596, 454}, {-1217, 627, 454}, {-1065, 659, 454}, {-913, 690, 454}, {-760, 719, 454}, {-608, 744, 454}, {-456, 764, 454}, {-304, 779, 454}, {-152, 789, 454}, {0, 792, 454}, {152, 789, 454}, {304, 779, 454}, {456, 764, 454}, {608, 744, 454}, {760, 719, 454}, {913, 690, 454}, {1065, 659, 454}, {1217, 627, 454}, {1369, 596, 454}, {1521, 565, 454}, {1673, 536, 454}, {1825, 509, 454}},
};
constexpr float kGridTilesPerLoop = 25.0f; // UVOffset0.v over the timeline
// The cross texture (chs_bg_grid, 256 texels to a tile, so 128 to a cell):
// a line is a two-texel core of this colour at alpha 230/255, inside a halo
// that starts at 72/255 for three texels either side and thins out over
// about ten more. A cell is some 80 pixels across where the grid is in full
// view, 0.6 pixels to the texel.
constexpr float kGridTexel = 0.6f;
constexpr float kGridCoreWidth = 2.0f * kGridTexel + 0.6f; // never thinner than a line can draw
constexpr float kGridCoreAlpha = 230.0f / 255.0f;
constexpr float kGridHaloWidth = 14.0f * kGridTexel;
constexpr float kGridArcSlack = 0.4f; // pixels a joined run may stray from its arc
constexpr float kGridHaloAlpha = 0.20f; // the halo's mean over that width
constexpr float kGridTexR = 0.46f, kGridTexG = 0.74f, kGridTexB = 0.86f;

// The band over the centre line: black at the horizon, clear at this many
// pixels from it (the model's band reaches |y| 300 at z -600).
constexpr float kBandReach = kFocal * 300.0f / (kCameraZ + 600.0f);
constexpr int kBandSteps = 12; // per half

struct Rgba {
    float r, g, b, a;
};

Rgba Mix(const Rgba& a, const Rgba& b, float t)
{
    return Rgba{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                a.a + (b.a - a.a) * t};
}

// The layout's keys sit at these frames; values between them are linear.
constexpr float kKeyFrames[5] = {0.0f, 1499.0f, 2999.0f, 4499.0f, 5999.0f};

Rgba Track(const Rgba keys[5], float frame)
{
    for (int i = 0; i < 4; ++i) {
        if (frame <= kKeyFrames[i + 1])
            return Mix(keys[i], keys[i + 1],
                       (frame - kKeyFrames[i]) / (kKeyFrames[i + 1] - kKeyFrames[i]));
    }
    return keys[4];
}

// BaseMapFactor of the grid materials (control ids 2 and 3): the tint, and
// in alpha how solid the lines are.
constexpr Rgba kGridTint[5] = {
    {0.4f, 1.0f, 1.0f, 0.5f}, {0.7f, 0.4f, 1.0f, 0.25f}, {1.0f, 0.4f, 0.4f, 0.5f},
    {0.7f, 1.0f, 0.4f, 0.5f}, {0.4f, 1.0f, 1.0f, 0.5f},
};
// BaseMapFactor of the star materials (ids 1 and 4): a brightening late in
// the loop. Colours cannot go past white here, so the resting level is below
// it by the same ratio.
constexpr Rgba kStarLevel[5] = {
    {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
    {1.4f, 1.4f, 1.4f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
};
constexpr float kStarRest = 1.0f / 1.4f;

void Fill(Canvas& canvas, float x, float y, float w, float h, Color color)
{
    canvas.Sprite(TEX_WHITE, x, y, w, h, kFillUv, A_TL, color);
}

// One half of the picture: `sign` +1 is above the centre line, -1 below.
// `near` and `far` are distances from the centre line in pixels.
void Row(Canvas& canvas, Tex tex, int sign, float near, float far, Uv uv, Color color,
         float x = 0.0f, float w = kScreenW)
{
    // Above the line the far edge of a strip is its top; below, its bottom.
    const float top = sign > 0 ? kCentreY - far : kCentreY + near;
    canvas.Sprite(tex, x, top, w, far - near, uv, A_TL, color);
}

// Height on a star plane of the point seen `d` pixels from the centre line.
float StarY(float d) { return d * (kCameraZ + kStarFarZ) / (kFocal + kStarSlope * d); }

void DrawStars(Canvas& canvas, float phase, float level, float alpha)
{
    const Color color{level, level, level, alpha};
    for (int sign : {1, -1}) {
        // The lower plane is the upper one turned half a turn on the model.
        // Flipped coordinates are avoided here; it reads further down the
        // tile instead, which keeps the two halves from mirroring each other.
        const float vShift = sign > 0 ? 0.0f : 0.35f;
        for (int strip = 0; strip < kStarStrips; ++strip) {
            const float d0 = kCentreY * strip / kStarStrips;
            const float d1 = kCentreY * (strip + 1) / kStarStrips;
            const float y0 = StarY(d0), y1 = StarY(d1);
            const float depth = kCameraZ + kStarFarZ - kStarSlope * (y0 + y1) * 0.5f;
            // What the screen's half width covers at that depth.
            const float uHalf = kStarUHalf * (kCentreX * depth / kFocal) / kStarHalfWidth;
            // v runs 1.5 on the centre line to 0.5 at the edge and the layout
            // slides it one tile back per loop. Shifted by whole tiles so the
            // window stays inside the 2 x 2 texture: 0.08 .. 1.95 here.
            const float slide = 1.0f - phase;
            const float vNear = 0.6f - y0 / kStarHalfWidth + slide + vShift;
            const float vFar = 0.6f - y1 / kStarHalfWidth + slide + vShift;
            const float u0 = (1.0f - uHalf) * 0.5f, u1 = (1.0f + uHalf) * 0.5f;
            const Uv uv = sign > 0 ? Uv{u0, vFar * 0.5f, u1, vNear * 0.5f}
                                   : Uv{u0, vNear * 0.5f, u1, vFar * 0.5f};
            Row(canvas, TEX_SPACE_STARS, sign, d0, d1, uv, color);
        }
    }
}

void DrawBand(Canvas& canvas, float alpha)
{
    for (int sign : {1, -1}) {
        for (int step = 0; step < kBandSteps; ++step) {
            const float d0 = kBandReach * step / kBandSteps;
            const float d1 = kBandReach * (step + 1) / kBandSteps;
            // Solid only on the line itself, and thinning quickly from it.
            const float t = 1.0f - (step + 0.5f) / kBandSteps;
            Row(canvas, TEX_WHITE, sign, d0, d1, kFillUv,
                Color{0.0f, 0.0f, 0.0f, t * t * t * alpha});
        }
    }
}

struct Point {
    float x, y;
    bool ahead; // in front of the viewer
};

// A lattice point (column, fractional row) of a grid sheet on the screen.
Point GridPoint(int sign, int column, float row)
{
    int r0 = static_cast<int>(row);
    if (r0 > kGridRows - 2) r0 = kGridRows - 2;
    if (r0 < 0) r0 = 0;
    const float t = row - r0;
    const int16_t* a = kGridMesh[r0][column];
    const int16_t* b = kGridMesh[r0 + 1][column];
    const float x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t;
    const float depth = kCameraZ + a[2] + (b[2] - a[2]) * t;
    return Point{kCentreX + kFocal * x / depth, kCentreY - sign * kFocal * y / depth, depth > 1.0f};
}

bool OnScreen(const Point& a, const Point& b)
{
    const float margin = 8.0f;
    if (!a.ahead || !b.ahead) return false;
    if (a.x < -margin && b.x < -margin) return false;
    if (a.x > kScreenW + margin && b.x > kScreenW + margin) return false;
    if (a.y < -margin && b.y < -margin) return false;
    if (a.y > kScreenH + margin && b.y > kScreenH + margin) return false;
    return true;
}

// Whether the lattice points strictly between two of an arc lie on the
// straight line joining them, to within what the eye can tell.
bool RunIsStraight(const Point* arc, int from, int to)
{
    const Point& a = arc[from];
    const Point& b = arc[to];
    if (!a.ahead || !b.ahead) return false;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.01f) return false;
    for (int i = from + 1; i < to; ++i) {
        if (!arc[i].ahead) return false;
        const float off = std::fabs((arc[i].x - a.x) * dy - (arc[i].y - a.y) * dx) / length;
        if (off > kGridArcSlack) return false;
    }
    return true;
}

void DrawGrid(Canvas& canvas, float phase, const Rgba& tint, float alpha)
{
    // What reaches the screen is texture x BaseMapFactor x vertex colour:
    // the cross's own blue-white and alpha, the layout's tint and overall
    // strength (alpha 0.5 for most of the loop, 0.25 at its faintest), and
    // the mesh's fade in from the horizon.
    const Color ink{kGridTexR * tint.r, kGridTexG * tint.g, kGridTexB * tint.b, 1.0f};
    const float solid = tint.a * alpha;
    const auto line = [&](const Point& a, const Point& b, float v) {
        float fade = v / kGridSolidV;
        if (fade > 1.0f) fade = 1.0f;
        if (fade <= 0.02f || !OnScreen(a, b)) return;
        // One sprite carrying the whole profile where the canvas has it;
        // otherwise the halo and the core as two flat lines.
        if (canvas.GlowLine(a.x, a.y, b.x, b.y, WithAlpha(ink, solid * fade))) return;
        canvas.Line(a.x, a.y, b.x, b.y, kGridHaloWidth,
                    WithAlpha(ink, kGridHaloAlpha * solid * fade));
        canvas.Line(a.x, a.y, b.x, b.y, kGridCoreWidth,
                    WithAlpha(ink, kGridCoreAlpha * solid * fade));
    };
    const int lastRow = kGridRows - 1;
    for (int sign : {1, -1}) {
        // Lines running away from the viewer: the lattice's columns. They do
        // not move; the texture slides along them.
        for (int column = 0; column < kGridColumns; ++column) {
            for (int row = 0; row < lastRow; ++row)
                line(GridPoint(sign, column, static_cast<float>(row)),
                     GridPoint(sign, column, static_cast<float>(row + 1)), (row + 0.5f) * 0.5f);
        }
        // Lines across: half a tile apart, sliding in towards the horizon as
        // the layout pushes v forward by kGridTilesPerLoop tiles per loop.
        // In rows (half tiles) the pattern repeats every row.
        const float shift = std::fmod(kGridTilesPerLoop * phase * 2.0f, 1.0f);
        for (int index = 1; index <= lastRow; ++index) {
            const float row = index - shift;
            // An arc is one strength from end to end, so its lattice steps
            // are joined wherever a single straight run stays on the curve.
            // Near the horizon the arcs are almost flat and a few runs do.
            Point arc[kGridColumns];
            for (int column = 0; column < kGridColumns; ++column)
                arc[column] = GridPoint(sign, column, row);
            for (int from = 0; from + 1 < kGridColumns;) {
                int to = from + 1;
                while (to + 1 < kGridColumns && RunIsStraight(arc, from, to + 1)) ++to;
                line(arc[from], arc[to], row * 0.5f);
                from = to;
            }
        }
    }
}

} // namespace

bool BuildSpaceStarsRgba(const uint8_t* tile, size_t size, uint8_t* rgba)
{
    if (!tile || !rgba || size != kSpaceStarsTileBytes) return false;
    const int blocks = kSpaceStarsTile / 4;
    for (int by = 0; by < blocks; ++by) {
        for (int bx = 0; bx < blocks; ++bx) {
            const uint8_t* block = tile + (static_cast<size_t>(by) * blocks + bx) * 8;
            const uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
            const uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
            int palette[4][3];
            const uint16_t ends[2] = {c0, c1};
            for (int e = 0; e < 2; ++e) {
                const int r = (ends[e] >> 11) & 31, g = (ends[e] >> 5) & 63, b = ends[e] & 31;
                palette[e][0] = (r << 3) | (r >> 2);
                palette[e][1] = (g << 2) | (g >> 4);
                palette[e][2] = (b << 3) | (b >> 2);
            }
            for (int c = 0; c < 3; ++c) {
                // The asset is written with c0 >= c1 throughout: four colours.
                palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
                palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
            }
            const uint32_t bits = static_cast<uint32_t>(block[4]) | (block[5] << 8) |
                                  (block[6] << 16) | (static_cast<uint32_t>(block[7]) << 24);
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int* colour = palette[(bits >> (2 * (py * 4 + px))) & 3];
                    const int x = bx * 4 + px, y = by * 4 + py;
                    // The same pixel in each of the four copies of the tile.
                    for (int copy = 0; copy < 4; ++copy) {
                        const size_t tx = x + (copy & 1) * kSpaceStarsTile;
                        const size_t ty = y + (copy >> 1) * kSpaceStarsTile;
                        uint8_t* out = rgba + (ty * kSpaceStarsSize + tx) * 4;
                        out[0] = static_cast<uint8_t>(colour[0]);
                        out[1] = static_cast<uint8_t>(colour[1]);
                        out[2] = static_cast<uint8_t>(colour[2]);
                        out[3] = 255;
                    }
                }
            }
        }
    }
    // The grid line's profile, in the rows above anything the stars read
    // (their window starts 42 rows down): white, with the alpha of a bright
    // core two texels across inside a soft halo. Flat along u.
    static const uint8_t kProfile[kSpaceLineRows] = {0,  0,  0,  0,  14,  36, 60, 230,
                                                     230, 60, 36, 14, 0,  0,  0,  0};
    for (int y = 0; y < kSpaceLineRow0 + kSpaceLineRows + 4; ++y) {
        const int row = y - kSpaceLineRow0;
        const uint8_t a = row >= 0 && row < kSpaceLineRows ? kProfile[row] : 0;
        for (int x = 0; x < kSpaceStarsSize; ++x) {
            uint8_t* out = rgba + (static_cast<size_t>(y) * kSpaceStarsSize + x) * 4;
            out[0] = out[1] = out[2] = 255;
            out[3] = a;
        }
    }
    return true;
}

void DrawSpaceBackground(Canvas& canvas, uint32_t frame, float alpha)
{
    if (alpha <= 0.004f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    const float time = static_cast<float>(frame % kLoopFrames);
    const float phase = time / kLoopFrames;

    // Black first: the star texture may not have arrived yet, and what is
    // behind (the title's key art) must not show through the picture.
    Fill(canvas, 0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.02f, alpha});
    DrawStars(canvas, phase, kStarRest * Track(kStarLevel, time).r, alpha);
    DrawBand(canvas, alpha);
    DrawGrid(canvas, phase, Track(kGridTint, time), alpha);
}

} // namespace lobby
