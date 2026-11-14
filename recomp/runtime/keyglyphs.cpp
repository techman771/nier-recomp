// Keyboard glyphs: the keyboard icon set is drawn at run time from the current key bindings, so any
// key the player binds gets its own keycap (GDI+; Windows only, like the rest of the windowing code).
// Same grid as the other sets (glyphs.cpp): 4 columns x 6 rows of 64-pixel cells, 256x384 RGBA.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "guest.h"
#include "input.h"
#include "glyphs.h"

namespace
{
    using namespace Gdiplus;
    constexpr int S = 64;
    const Color kInk(255, 26, 26, 30), kPaper(255, 245, 245, 240), kShade(255, 170, 170, 165), kLit(255, 255, 208, 80);

    bool StartGdiplus()
    {
        static std::once_flag once;
        static ULONG_PTR token = 0;
        std::call_once(once, [] { GdiplusStartupInput input; GdiplusStartup(&token, &input, nullptr); });
        return token != 0;
    }

    // What goes on a keycap: short enough to fit, clear enough to find on a keyboard.
    std::wstring KeyLabel(int key)
    {
        if (key >= 'A' && key <= 'Z' || key >= '0' && key <= '9') return std::wstring(1, wchar_t(key));
        if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return L"Num" + std::to_wstring(key - VK_NUMPAD0);
        switch (key)
        {
        case 0: return L"-";
        case VK_LBUTTON: return L"LMB"; case VK_RBUTTON: return L"RMB"; case VK_MBUTTON: return L"MMB";
        case VK_XBUTTON1: return L"M4"; case VK_XBUTTON2: return L"M5";
        case VK_UP: return L"↑"; case VK_DOWN: return L"↓"; case VK_LEFT: return L"←"; case VK_RIGHT: return L"→";
        case VK_SPACE: return L"Space"; case VK_RETURN: return L"Enter"; case VK_TAB: return L"Tab"; case VK_ESCAPE: return L"Esc";
        case VK_BACK: return L"Bksp"; case VK_LSHIFT: return L"Shift"; case VK_RSHIFT: return L"RShift";
        case VK_LCONTROL: return L"Ctrl"; case VK_RCONTROL: return L"RCtrl"; case VK_LMENU: return L"Alt"; case VK_RMENU: return L"RAlt";
        case VK_CAPITAL: return L"Caps"; case VK_INSERT: return L"Ins"; case VK_DELETE: return L"Del"; case VK_HOME: return L"Home";
        case VK_END: return L"End"; case VK_PRIOR: return L"PgUp"; case VK_NEXT: return L"PgDn";
        case VK_MULTIPLY: return L"Num*"; case VK_ADD: return L"Num+"; case VK_SUBTRACT: return L"Num-"; case VK_DIVIDE: return L"Num/"; case VK_DECIMAL: return L"Num.";
        }
        // Punctuation and anything else: the character the key types, or its system name.
        const UINT character = MapVirtualKeyW(UINT(key), MAPVK_VK_TO_CHAR) & 0x7FFF;
        if (character > 32) return std::wstring(1, wchar_t(character));
        const std::string name = InputKeyName(key);
        return std::wstring(name.begin(), name.end()).substr(0, 6);
    }

    void RoundedRect(GraphicsPath& path, float x, float y, float w, float h, float r)
    {
        const float d = 2 * r;
        path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90);
        path.CloseFigure();
    }

    void Label(Graphics& g, const std::wstring& text, float cx, float cy, float size)
    {
        FontFamily family(L"Segoe UI");
        Font font(&family, size, FontStyleBold, UnitPixel);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);
        SolidBrush ink(kInk);
        g.DrawString(text.c_str(), int(text.size()), &font, PointF(cx, cy + size * 0.04f), &format, &ink);
    }

    // A mouse with one button lit, centred in cell (column, row): left, right, middle (the wheel),
    // or a side button (a tab on the left edge, numbered).
    void MouseIcon(Graphics& g, int column, int row, int key)
    {
        const float cx = column * S + S / 2.0f, cy = row * S + S / 2.0f, w = 36, h = 52, x = cx - w / 2, y = cy - h / 2;
        GraphicsPath body;
        RoundedRect(body, x, y, w, h, 16);
        SolidBrush paper(kPaper), lit(kLit);
        Pen ink(kInk, 3), thin(kInk, 2);
        g.FillPath(&paper, &body);
        if (key == VK_LBUTTON || key == VK_RBUTTON)
        {
            Gdiplus::Region half(RectF(key == VK_LBUTTON ? x : cx, y, w / 2, 22));
            Gdiplus::Region button(&body);
            button.Intersect(&half);
            g.FillRegion(&lit, &button);
        }
        g.DrawPath(&ink, &body);
        g.DrawLine(&thin, x, y + 22, x + w, y + 22);
        g.DrawLine(&thin, cx, y, cx, y + 22);
        GraphicsPath wheel;
        RoundedRect(wheel, cx - 4, y + 5, 8, 13, 4);
        SolidBrush wheelFill(key == VK_MBUTTON ? kLit : kPaper);
        g.FillPath(&wheelFill, &wheel);
        g.DrawPath(&thin, &wheel);
        if (key == VK_XBUTTON1 || key == VK_XBUTTON2)
        {
            GraphicsPath side;
            RoundedRect(side, x - 7, cy - 2, 10, 16, 4);
            g.FillPath(&lit, &side);
            g.DrawPath(&thin, &side);
            Label(g, key == VK_XBUTTON1 ? L"4" : L"5", cx, cy + 12, 18);
        }
    }

    // A full-size keycap centred in cell (column, row).
    void Keycap(Graphics& g, int column, int row, int key)
    {
        if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2) { MouseIcon(g, column, row, key); return; }
        const std::wstring text = KeyLabel(key);
        const float cx = column * S + S / 2.0f, cy = row * S + S / 2.0f;
        const float width = text.size() <= 2 ? 50.0f : 60.0f;
        const float size = text.size() == 1 ? 30.0f : text.size() == 2 ? 24.0f : text.size() == 3 ? 20.0f : text.size() == 4 ? 17.0f : text.size() == 5 ? 15.0f : 12.0f;
        GraphicsPath path;
        RoundedRect(path, cx - width / 2, cy - 25, width, 50, 9);
        SolidBrush paper(kPaper);
        Pen ink(kInk, 3), shade(kShade, 2);
        g.FillPath(&paper, &path);
        g.DrawPath(&ink, &path);
        g.DrawLine(&shade, cx - width / 2 + 8, cy + 19, cx + width / 2 - 8, cy + 19);
        Label(g, text, cx, cy - 3, size);
    }

    // Four small keys laid out like a d-pad (up, left, down, right) in cell (column, row).
    void Cluster(Graphics& g, int column, int row, const int keys[4], bool litUp, bool litLeft, bool litDown, bool litRight)
    {
        const float x = column * S + 2.0f, y = row * S + 12.0f;
        const float px[4] = { x + 20.5f, x, x + 20.5f, x + 41 }, py[4] = { y, y + 21, y + 21, y + 21 };
        const bool lit[4] = { litUp, litLeft, litDown, litRight };
        for (int i = 0; i < 4; i++)
        {
            GraphicsPath path;
            RoundedRect(path, px[i], py[i], 19, 19, 4);
            SolidBrush fill(lit[i] ? kLit : kPaper);
            Pen ink(kInk, 2);
            g.FillPath(&fill, &path);
            g.DrawPath(&ink, &path);
            std::wstring text = KeyLabel(keys[i]);
            if (text.rfind(L"Num", 0) == 0) text = L"N" + text.substr(3);   // numpad keys: N8, N+
            text = text.substr(0, 3);
            Label(g, text, px[i] + 9.5f, py[i] + 9.0f, text.size() == 1 ? 13.0f : text.size() == 2 ? 10.0f : 8.0f);
        }
    }
}

uint64_t KeyBindingsStamp()
{
    uint64_t stamp = 0xCBF29CE484222325ull;
    for (int i = 0; i < kKeyActions; i++) stamp = (stamp ^ uint64_t(InputGetKey(i))) * 0x100000001B3ull;
    return stamp;
}

bool DrawKeyboardAtlas(uint8_t* rgba, uint32_t width, uint32_t height)
{
    if (!StartGdiplus()) return false;

    Bitmap bitmap(INT(width), INT(height), PixelFormat32bppARGB);
    {
        Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.Clear(Color(0, 26, 26, 30));
        auto key = [](int action) { return InputGetKey(action); };
        // Action indices (input.cpp): 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 Start, 9 Back, 10 L3, 11 R3,
        // 12-15 move up/down/left/right, 16-19 camera up/down/left/right, 20-23 d-pad up/down/left/right.
        Keycap(g, 0, 0, key(0)); Keycap(g, 1, 0, key(1)); Keycap(g, 2, 0, key(2)); Keycap(g, 3, 0, key(3));
        Keycap(g, 0, 1, key(6)); Keycap(g, 1, 1, key(7)); Keycap(g, 2, 1, key(4)); Keycap(g, 3, 1, key(5));
        const int move[4] = { key(12), key(14), key(13), key(15) }, camera[4] = { key(16), key(18), key(17), key(19) };
        const int dpad[4] = { key(20), key(22), key(21), key(23) };
        Cluster(g, 0, 2, move, false, false, false, false);
        Cluster(g, 1, 2, camera, false, false, false, false);
        Keycap(g, 2, 2, key(10)); Keycap(g, 3, 2, key(11));
        Keycap(g, 0, 3, key(9)); Keycap(g, 1, 3, key(8));
        Cluster(g, 2, 3, dpad, false, false, false, false);
        Keycap(g, 3, 3, 0);                                           // the Guide button has no key
        Keycap(g, 0, 4, key(23)); Keycap(g, 1, 4, key(22)); Keycap(g, 2, 4, key(20)); Keycap(g, 3, 4, key(21));   // d-pad right, left, up, down
        Cluster(g, 0, 5, move, false, true, false, true);             // L left/right
        Cluster(g, 1, 5, move, true, false, true, false);             // L up/down
    }
    BitmapData data;
    Rect all(0, 0, INT(width), INT(height));
    if (bitmap.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return false;
    for (uint32_t y = 0; y < height; y++)
    {
        const uint8_t* source = static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
        uint8_t* out = rgba + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; x++) { out[x * 4] = source[x * 4 + 2]; out[x * 4 + 1] = source[x * 4 + 1]; out[x * 4 + 2] = source[x * 4]; out[x * 4 + 3] = source[x * 4 + 3]; }   // BGRA -> RGBA
    }
    bitmap.UnlockBits(&data);
    return true;
}

// The title prompt in the style of the game's own: a bold serif, widely spaced, with a soft glow.
// Centred in the box and shrunk to fit it; white on black, one byte per pixel.
bool DrawPromptMask(const std::string& text, uint8_t* mask, int width, int height, float scale, bool glow, float iconWidth, float* iconLeft)
{
    if (!StartGdiplus()) return false;
    const std::wstring wide(text.begin(), text.end());
    Bitmap bitmap(width, height, PixelFormat32bppARGB);
    {
        Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.Clear(Color(255, 0, 0, 0));
        FontFamily family(L"Times New Roman");
        StringFormat format(StringFormat::GenericTypographic());
        float size = 19.0f * scale, total = 0;
        std::vector<float> advances;
        for (int pass = 0; pass < 2; pass++)   // the second pass at a smaller size if the text is too wide
        {
            const float tracking = size * 0.115f, space = size * 0.62f;
            Font font(&family, size, FontStyleBold, UnitPixel);
            advances.clear();
            total = 0;
            for (wchar_t c : wide)
            {
                float advance = space;
                if (c == L'*') advance = iconWidth;
                else if (c != L' ') { RectF box; g.MeasureString(&c, 1, &font, PointF(0, 0), &format, &box); advance = box.Width + tracking; }
                advances.push_back(advance);
                total += advance;
            }
            total -= tracking;
            const float room = float(width) - 10.0f * scale;   // leaves the glow inside the box
            if (total <= room) break;
            size *= room / total;
        }
        GraphicsPath path;
        float x = (float(width) - total) / 2;
        for (size_t i = 0; i < wide.size(); i++)
        {
            if (wide[i] == L'*') { if (iconLeft) *iconLeft = x; }
            else if (wide[i] != L' ') path.AddString(&wide[i], 1, &family, FontStyleBold, size, PointF(x, (float(height) - size * 1.15f) / 2), &format);
            x += advances[i];
        }
        if (glow) for (float spread : { 5.0f, 3.5f, 2.0f })
        {
            Pen pen(Color(26, 255, 255, 255), spread * scale);
            pen.SetLineJoin(LineJoinRound);
            g.DrawPath(&pen, &path);
        }
        SolidBrush fill(Color(255, 255, 255, 255));
        g.FillPath(&fill, &path);
    }
    BitmapData data;
    Rect all(0, 0, width, height);
    if (bitmap.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return false;
    for (int y = 0; y < height; y++)
    {
        const uint8_t* source = static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
        for (int x = 0; x < width; x++) mask[size_t(y) * width + x] = source[x * 4 + 1];
    }
    bitmap.UnlockBits(&data);
    return true;
}
