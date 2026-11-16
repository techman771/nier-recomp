// On-screen keyboard, standing in for the console's own (XamShowKeyboardUI: the game asks for the
// hero's name with it). Drawn over the game's picture and worked with whatever is at hand:
//  - controller: d-pad or left stick moves, A types, X deletes, Y is space, LB shifts, Start accepts,
//    B cancels;
//  - keyboard: typing goes straight into the text, Enter accepts, Esc cancels, arrows move;
//  - mouse: click a key.
// While it is open the game itself gets no input (input.cpp), as on the console.
// The picture is drawn with GDI+ into a 1920x1080 image whenever something changes and handed to the
// renderer as an overlay (RendererSetOverlay). Windows-only for now, like the rest of the window code.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "guest.h"
#include "input.h"
#include "renderer.h"

using namespace Gdiplus;

namespace
{
    constexpr int kCanvasWidth = 1920, kCanvasHeight = 1080;
    constexpr int kColumns = 10, kLetterRows = 4;
    const wchar_t* const kRows[kLetterRows] = { L"1234567890", L"qwertyuiop", L"asdfghjkl-", L"zxcvbnm'.!" };
    // The bottom row: wide keys spanning these columns.
    enum Special { Shift, Space, Delete, Done, SpecialCount };
    struct Wide { int firstColumn, lastColumn; const wchar_t* label; };
    const Wide kWide[SpecialCount] = { { 0, 1, L"Shift" }, { 2, 5, L"Space" }, { 6, 7, L"Delete" }, { 8, 9, L"Done" } };

    // Layout, in canvas pixels.
    constexpr int kKeyWidth = 96, kKeyHeight = 78, kGap = 10;
    constexpr int kGridWidth = kColumns * kKeyWidth + (kColumns - 1) * kGap;
    constexpr int kGridLeft = (kCanvasWidth - kGridWidth) / 2, kGridTop = 450;
    constexpr int kPanelLeft = kGridLeft - 60, kPanelTop = 170, kPanelWidth = kGridWidth + 120, kPanelHeight = 810;

    struct Event { enum Kind { Char, Key, Click } kind; int a = 0, b = 0, c = 0, d = 0; };
    std::mutex gEventMutex;
    std::deque<Event> gEvents;
    std::atomic<bool> gActive{ false };

    struct State
    {
        std::wstring text, title, description;
        size_t maxChars = 0;
        int row = 1, column = 0;   // row kLetterRows is the wide keys
        bool shift = true;         // the next letter is a capital (on for the first letter of an empty text)
        InputStyle style = InputStyle::Xbox;
    };

    bool StartGdiplus()
    {
        static std::once_flag once;
        static ULONG_PTR token = 0;
        std::call_once(once, [] { GdiplusStartupInput input; GdiplusStartup(&token, &input, nullptr); });
        return token != 0;
    }

    int WideAt(int column)
    {
        for (int i = 0; i < SpecialCount; i++) if (column >= kWide[i].firstColumn && column <= kWide[i].lastColumn) return i;
        return 0;
    }

    RectF KeyRect(int row, int column)
    {
        const float y = float(kGridTop + row * (kKeyHeight + kGap));
        if (row < kLetterRows) return RectF(float(kGridLeft + column * (kKeyWidth + kGap)), y, float(kKeyWidth), float(kKeyHeight));
        const Wide& wide = kWide[WideAt(column)];
        const float x = float(kGridLeft + wide.firstColumn * (kKeyWidth + kGap));
        return RectF(x, y, float((wide.lastColumn - wide.firstColumn + 1) * (kKeyWidth + kGap) - kGap), float(kKeyHeight));
    }

    wchar_t KeyChar(const State& s, int row, int column)
    {
        const wchar_t c = kRows[row][column];
        return s.shift && c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c;
    }

    void Draw(const State& s, std::vector<uint8_t>& rgba)
    {
        Bitmap bitmap(kCanvasWidth, kCanvasHeight, PixelFormat32bppARGB);
        {
            Graphics g(&bitmap);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            g.Clear(Color(150, 0, 0, 0));   // the game dims behind it
            SolidBrush panel(Color(238, 18, 18, 20)), light(Color(255, 232, 228, 214)), dim(Color(255, 150, 148, 140)), dark(Color(255, 20, 20, 22)),
                key(Color(255, 46, 46, 50)), field(Color(255, 8, 8, 10));
            Pen border(Color(255, 150, 148, 140), 2.0f);
            g.FillRectangle(&panel, kPanelLeft, kPanelTop, kPanelWidth, kPanelHeight);
            g.DrawRectangle(&border, kPanelLeft, kPanelTop, kPanelWidth, kPanelHeight);

            FontFamily serif(L"Times New Roman");
            Font titleFont(&serif, 46, FontStyleBold, UnitPixel), textFont(&serif, 52, FontStyleBold, UnitPixel), keyFont(&serif, 40, FontStyleBold, UnitPixel),
                smallFont(&serif, 28, FontStyleRegular, UnitPixel), wideFont(&serif, 32, FontStyleBold, UnitPixel);
            StringFormat centred;
            centred.SetAlignment(StringAlignmentCenter);
            centred.SetLineAlignment(StringAlignmentCenter);
            StringFormat left;
            left.SetLineAlignment(StringAlignmentCenter);

            const std::wstring title = s.title.empty() ? L"Enter a name" : s.title;
            g.DrawString(title.c_str(), -1, &titleFont, RectF(float(kPanelLeft), float(kPanelTop + 24), float(kPanelWidth), 60.0f), &centred, &light);
            if (!s.description.empty())
                g.DrawString(s.description.c_str(), -1, &smallFont, RectF(float(kPanelLeft + 40), float(kPanelTop + 88), float(kPanelWidth - 80), 44.0f), &centred, &dim);

            // The text, with a bar where the next character goes and how many are left.
            const RectF box(float(kGridLeft), float(kPanelTop + 150), float(kGridWidth), 92.0f);
            g.FillRectangle(&field, box);
            g.DrawRectangle(&border, box);
            RectF measured;
            const std::wstring shown = s.text.empty() ? L"" : s.text;
            StringFormat typographic(StringFormat::GenericTypographic());
            typographic.SetFormatFlags(typographic.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
            g.MeasureString(shown.c_str(), -1, &textFont, PointF(0, 0), &typographic, &measured);
            const float textLeft = box.X + 24.0f, textTop = box.Y + (box.Height - 60.0f) / 2;
            g.DrawString(shown.c_str(), -1, &textFont, PointF(textLeft, textTop), &typographic, &light);
            g.FillRectangle(&light, textLeft + measured.Width + 4.0f, box.Y + 18.0f, 4.0f, box.Height - 36.0f);
            if (s.maxChars)
            {
                const std::wstring count = std::to_wstring(s.text.size()) + L" / " + std::to_wstring(s.maxChars);
                StringFormat right;
                right.SetAlignment(StringAlignmentFar);
                right.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(count.c_str(), -1, &smallFont, RectF(box.X, box.Y, box.Width - 18.0f, box.Height), &right, &dim);
            }

            for (int row = 0; row <= kLetterRows; row++)
                for (int column = 0; column < kColumns; column++)
                {
                    if (row == kLetterRows && column != kWide[WideAt(column)].firstColumn) continue;   // one box per wide key
                    const bool selected = row == s.row && (row < kLetterRows ? column == s.column : WideAt(column) == WideAt(s.column));
                    const bool lit = row == kLetterRows && WideAt(column) == Shift && s.shift;
                    const RectF rect = KeyRect(row, column);
                    g.FillRectangle(selected ? &light : &key, rect);
                    if (lit && !selected) g.DrawRectangle(&border, rect);
                    if (row < kLetterRows)
                    {
                        const wchar_t label[2] = { KeyChar(s, row, column), 0 };
                        g.DrawString(label, -1, &keyFont, rect, &centred, selected ? &dark : &light);
                    }
                    else g.DrawString(kWide[WideAt(column)].label, -1, &wideFont, rect, &centred, selected ? &dark : (lit ? &light : &dim));
                }

            // What does what, in the words of the device in use.
            const wchar_t* hint = L"A  Select      X  Delete      Y  Space      LB  Shift      Start  Done      B  Cancel";
            if (s.style == InputStyle::PS4 || s.style == InputStyle::PS5) hint = L"Cross  Select      Square  Delete      Triangle  Space      L1  Shift      Options  Done      Circle  Cancel";
            else if (s.style == InputStyle::Keyboard) hint = L"Type on the keyboard, or click a key      Enter  Done      Esc  Cancel";
            g.DrawString(hint, -1, &smallFont, RectF(float(kPanelLeft), float(kPanelTop + kPanelHeight - 62), float(kPanelWidth), 44.0f), &centred, &dim);
        }
        BitmapData data;
        Rect all(0, 0, kCanvasWidth, kCanvasHeight);
        rgba.resize(size_t(kCanvasWidth) * kCanvasHeight * 4);
        if (bitmap.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return;
        for (int y = 0; y < kCanvasHeight; y++)
        {
            const uint8_t* source = static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
            uint8_t* out = rgba.data() + size_t(y) * kCanvasWidth * 4;
            for (int x = 0; x < kCanvasWidth; x++) { out[x * 4] = source[x * 4 + 2]; out[x * 4 + 1] = source[x * 4 + 1]; out[x * 4 + 2] = source[x * 4]; out[x * 4 + 3] = source[x * 4 + 3]; }   // BGRA -> RGBA
        }
        bitmap.UnlockBits(&data);
    }

    void Type(State& s, wchar_t c)
    {
        if (c < 32 || c > 126) return;   // what the game's font is sure to have
        if (s.maxChars && s.text.size() >= s.maxChars) return;
        s.text += c;
        s.shift = false;
    }

    void Erase(State& s)
    {
        if (!s.text.empty()) s.text.pop_back();
        if (s.text.empty()) s.shift = true;
    }

    // Presses the key under the cursor; returns 1 when the text is accepted.
    int Press(State& s)
    {
        if (s.row < kLetterRows) { Type(s, KeyChar(s, s.row, s.column)); return 0; }
        switch (WideAt(s.column))
        {
        case Shift: s.shift = !s.shift; break;
        case Space: Type(s, L' '); break;
        case Delete: Erase(s); break;
        case Done: if (!s.text.empty()) return 1; break;
        }
        return 0;
    }

    void Move(State& s, int dx, int dy)
    {
        if (dy)
        {
            const int rows = kLetterRows + 1;
            s.row = (s.row + dy + rows) % rows;
        }
        if (dx)
        {
            if (s.row < kLetterRows) s.column = (s.column + dx + kColumns) % kColumns;
            else
            {
                const int next = (WideAt(s.column) + dx + SpecialCount) % SpecialCount;
                s.column = kWide[next].firstColumn;
            }
        }
    }
}

bool DrawSmallLabel(const std::string& text, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
{
    if (!StartGdiplus()) return false;
    width = std::max<uint32_t>(240, (uint32_t(text.size()) * 25 + 40 + 3) & ~3u);   // the frame rate alone, or the longer line with the system's load
    height = 64;
    Bitmap bitmap(INT(width), INT(height), PixelFormat32bppARGB);
    {
        Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        SolidBrush plate(Color(150, 0, 0, 0)), ink(Color(255, 240, 240, 235));
        g.FillRectangle(&plate, 0, 0, INT(width), INT(height));
        FontFamily family(L"Consolas");
        Font font(&family, 44, FontStyleBold, UnitPixel);
        StringFormat centred;
        centred.SetAlignment(StringAlignmentCenter);
        centred.SetLineAlignment(StringAlignmentCenter);
        const std::wstring wide(text.begin(), text.end());
        g.DrawString(wide.c_str(), -1, &font, RectF(0, 2, float(width), float(height)), &centred, &ink);
    }
    BitmapData data;
    Rect all(0, 0, INT(width), INT(height));
    rgba.resize(size_t(width) * height * 4);
    if (bitmap.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return false;
    for (uint32_t y = 0; y < height; y++)
    {
        const uint8_t* source = static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
        uint8_t* out = rgba.data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; x++) { out[x * 4] = source[x * 4 + 2]; out[x * 4 + 1] = source[x * 4 + 1]; out[x * 4 + 2] = source[x * 4]; out[x * 4 + 3] = source[x * 4 + 3]; }
    }
    bitmap.UnlockBits(&data);
    return true;
}

bool OskActive() { return gActive.load(); }

void OskChar(wchar_t c)
{
    if (!gActive.load()) return;
    std::lock_guard lock(gEventMutex);
    gEvents.push_back({ Event::Char, int(c) });
}

void OskKey(int virtualKey)
{
    if (!gActive.load()) return;
    std::lock_guard lock(gEventMutex);
    gEvents.push_back({ Event::Key, virtualKey });
}

void OskClick(int x, int y, int clientWidth, int clientHeight)
{
    if (!gActive.load()) return;
    std::lock_guard lock(gEventMutex);
    gEvents.push_back({ Event::Click, x, y, clientWidth, clientHeight });
}

// Shows the keyboard until the text is accepted (true) or cancelled. `text` is the starting text and
// receives the result; `maxChars` 0 means no limit.
bool OskRun(std::wstring& text, const std::wstring& title, const std::wstring& description, size_t maxChars)
{
    if (!StartGdiplus() || !RendererReady()) return false;
    State s;
    s.text = text;
    if (maxChars && s.text.size() > maxChars) s.text.resize(maxChars);
    s.title = title;
    s.description = description;
    s.maxChars = maxChars;
    s.shift = s.text.empty();
    s.style = InputLastStyle();
    { std::lock_guard lock(gEventMutex); gEvents.clear(); }
    InputSetOverlay(true);
    gActive = true;

    std::vector<uint8_t> rgba;
    bool dirty = true;
    int result = -1;   // 1 accepted, 0 cancelled
    uint16_t lastButtons = 0xFFFF;   // whatever is held as it opens doesn't count
    int heldX = 0, heldY = 0;
    ULONGLONG repeatAt = 0;
    while (result < 0)
    {
        uint16_t buttons = 0;
        int16_t lx = 0, ly = 0;
        InputOverlayRead(buttons, lx, ly);
        const uint16_t pressed = buttons & ~lastButtons;
        lastButtons = buttons;
        constexpr int kStick = 16000;
        const int dx = ((buttons & 0x0008) || lx > kStick) - ((buttons & 0x0004) || lx < -kStick);
        const int dy = ((buttons & 0x0002) || ly < -kStick) - ((buttons & 0x0001) || ly > kStick);
        const ULONGLONG now = GetTickCount64();
        if (dx != heldX || dy != heldY)
        {
            heldX = dx; heldY = dy;
            if (dx || dy) { Move(s, dx, dy); dirty = true; repeatAt = now + 380; }
        }
        else if ((dx || dy) && now >= repeatAt) { Move(s, dx, dy); dirty = true; repeatAt = now + 110; }
        if (pressed) { s.style = InputLastStyle() == InputStyle::Keyboard ? InputStyle::Xbox : InputLastStyle(); dirty = true; }
        if (pressed & 0x1000) { if (Press(s) == 1) result = 1; }            // A
        if (pressed & 0x4000) Erase(s);                                     // X
        if (pressed & 0x8000) Type(s, L' ');                                // Y
        if (pressed & 0x0100) s.shift = !s.shift;                           // LB
        if ((pressed & 0x0010) && !s.text.empty()) result = 1;              // Start
        if (pressed & 0x2000) result = 0;                                   // B

        std::deque<Event> events;
        { std::lock_guard lock(gEventMutex); events.swap(gEvents); }
        for (const Event& e : events)
        {
            dirty = true;
            if (e.kind == Event::Char)
            {
                s.style = InputStyle::Keyboard;
                if (e.a == 8) Erase(s);
                else if (e.a == 13) { if (!s.text.empty()) result = 1; }
                else if (e.a == 27) result = 0;
                else Type(s, wchar_t(e.a));
            }
            else if (e.kind == Event::Key)
            {
                if (e.a == VK_LEFT) Move(s, -1, 0);
                else if (e.a == VK_RIGHT) Move(s, 1, 0);
                else if (e.a == VK_UP) Move(s, 0, -1);
                else if (e.a == VK_DOWN) Move(s, 0, 1);
            }
            else if (e.c > 0 && e.d > 0)
            {
                // The overlay fills the largest 16:9 area of the window, centred.
                float w = float(e.c), h = float(e.d);
                if (w / h > 16.0f / 9.0f) w = h * 16.0f / 9.0f; else h = w * 9.0f / 16.0f;
                const float x = (float(e.a) - (float(e.c) - w) / 2) * kCanvasWidth / w, y = (float(e.b) - (float(e.d) - h) / 2) * kCanvasHeight / h;
                for (int row = 0; row <= kLetterRows; row++)
                    for (int column = 0; column < kColumns; column++)
                        if (KeyRect(row, column).Contains(x, y))
                        {
                            s.row = row;
                            s.column = row < kLetterRows ? column : kWide[WideAt(column)].firstColumn;
                            s.style = InputStyle::Keyboard;
                            if (Press(s) == 1) result = 1;
                        }
            }
        }
        if (dirty || pressed)
        {
            Draw(s, rgba);
            RendererSetOverlay(rgba.data(), kCanvasWidth, kCanvasHeight);
            dirty = false;
        }
        InputKeepPointerMode();   // the mouse pointer shows, to click keys
        Sleep(8);
    }
    RendererSetOverlay(nullptr, 0, 0);
    gActive = false;
    // The button that closed it is still down: the game sees nothing until it is let go (it would
    // answer the game's next question, "Set your name as ...?", whose cursor starts on No).
    for (int i = 0; i < 300; i++)
    {
        uint16_t buttons = 0;
        int16_t lx = 0, ly = 0;
        InputOverlayRead(buttons, lx, ly);
        if (!buttons) break;
        Sleep(8);
    }
    InputSetOverlay(false);
    if (result == 1) text = s.text;
    return result == 1;
}
