#pragma once

// Hooks for the layout editor (tools/lobby_editor). They exist only in a
// build with LOBBY_LAYOUT_EDIT defined, which the plugin's is not: there
// Site is empty and the Edit functions do nothing but say "draw", so the
// screens compile to what they were.
//
// In an editing build every Painter call carries the source line that made
// it, and the editor's host program is asked what was changed at that line.
// One line draws all of what it draws: moving a list's row moves every row.

#include "lobby_canvas.h"

namespace lobby {

#ifdef LOBBY_LAYOUT_EDIT

// Where in the screen code a Painter call was written.
struct Site {
    const char* file;
    int line;
    Site(const char* f = __builtin_FILE(), int l = __builtin_LINE()) : file(f), line(l) {}
};

// What the editor changed at one site. Sizes and positions are offsets from
// what the code asks for, in the 1280x720 UI space.
struct LayoutEdit {
    float dx = 0.0f, dy = 0.0f, dw = 0.0f, dh = 0.0f;
    float scale = 1.0f; // text only
    int tex = -1;       // a Tex, or TEX_COUNT + n for the editor's own nth texture
    bool hasUv = false;
    Uv uv{};
    bool hidden = false; // deleted in the editor: the call draws nothing
    bool swap = false;   // a picture drawn as a stretched frame, or a frame as a plain picture
};

// All three are the host program's. LayoutSite notes that `site` is drawing next
// and returns its edit (one with nothing changed if there is none);
// LayoutCustomTex describes the editor's own nth texture.
const LayoutEdit& LayoutSite(Site site);
const TexInfo& LayoutCustomTex(int index);
// Draws the elements made in the editor for one of LobbyUi's screens (the
// number is its Screen): over the screen's own, under modals and the footer.
void LayoutDrawAdded(Canvas& canvas, int screen);

// Each returns false for a call the editor deleted: the Painter then draws nothing.
// `swap` is set where the editor asked for the other of picture and frame.
inline bool EditSprite(Site site, Tex& tex, float& x, float& y, float& w, float& h, Uv* uv,
                       bool* swap = nullptr)
{
    const LayoutEdit& edit = LayoutSite(site);
    if (edit.hidden) return false;
    if (swap) *swap = edit.swap;
    x += edit.dx;
    y += edit.dy;
    w += edit.dw;
    h += edit.dh;
    if (edit.tex >= 0) tex = static_cast<Tex>(edit.tex);
    if (edit.hasUv && uv) *uv = edit.uv;
    return true;
}

inline bool EditText(Site site, float& x, float& y, float& scale)
{
    const LayoutEdit& edit = LayoutSite(site);
    if (edit.hidden) return false;
    x += edit.dx;
    y += edit.dy;
    scale *= edit.scale;
    return true;
}

inline bool EditLine(Site site, float& x0, float& y0, float& x1, float& y1)
{
    const LayoutEdit& edit = LayoutSite(site);
    if (edit.hidden) return false;
    x0 += edit.dx;
    y0 += edit.dy;
    x1 += edit.dx;
    y1 += edit.dy;
    return true;
}

#else

struct Site {};
inline bool EditSprite(Site, Tex&, float&, float&, float&, float&, Uv*, bool* = nullptr) { return true; }
inline bool EditText(Site, float&, float&, float&) { return true; }
inline bool EditLine(Site, float&, float&, float&, float&) { return true; }

#endif

} // namespace lobby
