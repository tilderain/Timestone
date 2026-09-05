#pragma once

#include <Windows.h>
#include <commctrl.h>

// Keep styles explicit: visibility, tab order and owner drawing vary by control.
namespace LaunchMenuControls {

inline HWND Create(HWND parent, const char* className, const char* text,
                   DWORD style, int x, int y, int width, int height, int id)
{
    return CreateWindowA(className, text, style, x, y, width, height, parent,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                        nullptr, nullptr);
}

inline HWND Button(HWND parent, const char* text, DWORD style,
                   int x, int y, int width, int height, int id = 0)
{
    return Create(parent, "BUTTON", text, style, x, y, width, height, id);
}

inline HWND Label(HWND parent, const char* text, DWORD style,
                  int x, int y, int width, int height, int id = 0)
{
    return Create(parent, "STATIC", text, style, x, y, width, height, id);
}

inline HWND Combo(HWND parent, DWORD style,
                  int x, int y, int width, int height, int id)
{
    return Create(parent, "COMBOBOX", "", style, x, y, width, height, id);
}

inline HWND Edit(HWND parent, const char* text, DWORD style,
                 int x, int y, int width, int height, int id)
{
    return Create(parent, "EDIT", text, style, x, y, width, height, id);
}

inline HWND LobbyType(HWND parent, int id, int selection, int x, int y, int width)
{
    HWND combo = Combo(parent,
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        x, y, width, 100, id);
    const char* labels[] = {"Public", "Friends Only", "Private"};
    for (const char* label : labels)
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageA(combo, CB_SETCURSEL, selection, 0);
    return combo;
}

inline HWND LobbyName(HWND parent, int id, const char* name, size_t maxLength,
                      int x, int y, int width)
{
    HWND edit = Edit(parent, name,
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        x, y, width, 22, id);
    SendMessageA(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(maxLength), 0);
    return edit;
}

struct LobbyColumn {
    const char* heading;
    int width;
};

inline HWND LobbyList(HWND parent, int id, int x, int y, int width, int height,
                      const LobbyColumn (&columns)[3])
{
    HWND list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNA column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<char*>(columns[i].heading);
        column.cx = columns[i].width;
        column.iSubItem = i;
        ListView_InsertColumn(list, i, &column);
    }
    return list;
}

} // namespace LaunchMenuControls
