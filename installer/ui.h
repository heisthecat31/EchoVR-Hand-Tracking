#pragma once
// EchoXR's hand-drawn GDI+ UI kit, shared by EchoXRSetup.exe (setup.cpp) and
// EchoXRSettings.exe (settings.cpp): palette, fonts, shapes, text, buttons,
// toggles, the logo and the page header. Everything is drawn in logical 96-DPI
// units (the caller scales the Graphics); clickable controls register their
// rectangles in g_hots, and the window hit-tests the mouse against those.
//
// Include after <windows.h>, <gdiplus.h> and `using namespace Gdiplus;`.
#include <string>
#include <vector>

static float g_uiW = 720;                // logical page width (Header spans it)
static int   g_hot = 0, g_press = 0;     // hovered / pressed control id (0 = none)
struct HotRect { RectF r; int id; };
static std::vector<HotRect> g_hots;

// palette
static const DWORD kBg = 0x0E1015, kCard = 0x171A21, kCardHi = 0x1D212A, kBorder = 0x262B36,
                   kText = 0xEEF0F4, kMuted = 0x8E95A3, kFaint = 0x5A6170,
                   kAccent = 0x5B8CFF, kAccentHi = 0x7AA2FF, kAccent2 = 0x9A6BFF,
                   kGood = 0x3DDC97, kWarn = 0xFFB547, kBad = 0xFF5D6C;

static Color C(DWORD rgb, BYTE a = 255) { return Color(a, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
static Color Mix(DWORD a, DWORD b, float t) {
    auto ch = [&](int s) { return (BYTE)(((a >> s) & 255) + (((int)((b >> s) & 255) - (int)((a >> s) & 255)) * t)); };
    return Color(255, ch(16), ch(8), ch(0));
}

static Font *g_fTitle, *g_fH2, *g_fBody, *g_fBodyB, *g_fSmall, *g_fLabel, *g_fIcon, *g_fIconSm, *g_fIconBig, *g_fBtn;

static void MakeFonts() {
    g_fTitle  = new Font(L"Segoe UI Semibold", 25, FontStyleRegular, UnitPixel);
    g_fH2     = new Font(L"Segoe UI Semibold", 20, FontStyleRegular, UnitPixel);
    g_fBody   = new Font(L"Segoe UI", 13.5f, FontStyleRegular, UnitPixel);
    g_fBodyB  = new Font(L"Segoe UI Semibold", 14.5f, FontStyleRegular, UnitPixel);
    g_fSmall  = new Font(L"Segoe UI", 12.5f, FontStyleRegular, UnitPixel);
    g_fLabel  = new Font(L"Segoe UI Semibold", 11, FontStyleRegular, UnitPixel);
    g_fBtn    = new Font(L"Segoe UI Semibold", 14, FontStyleRegular, UnitPixel);
    g_fIcon   = new Font(L"Segoe MDL2 Assets", 17, FontStyleRegular, UnitPixel);
    g_fIconSm = new Font(L"Segoe MDL2 Assets", 12, FontStyleRegular, UnitPixel);
    g_fIconBig = new Font(L"Segoe MDL2 Assets", 34, FontStyleRegular, UnitPixel);
}

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
static void RoundPath(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRound(Graphics& g, RectF r, float rad, const Brush& b) { GraphicsPath p; RoundPath(p, r, rad); g.FillPath(&b, &p); }
static void FillRound(Graphics& g, RectF r, float rad, Color c) { SolidBrush b(c); FillRound(g, r, rad, b); }
static void StrokeRound(Graphics& g, RectF r, float rad, Color c, float w = 1) {
    GraphicsPath p; RoundPath(p, r, rad); Pen pen(c, w); g.DrawPath(&pen, &p);
}

enum { A_LEFT = 0, A_CENTER = 1, A_RIGHT = 2 };
static void Text(Graphics& g, const std::wstring& s, Font* f, Color c, RectF r, int align = A_LEFT, bool wrap = false) {
    StringFormat sf;
    sf.SetAlignment(align == A_CENTER ? StringAlignmentCenter : align == A_RIGHT ? StringAlignmentFar : StringAlignmentNear);
    sf.SetLineAlignment(wrap ? StringAlignmentNear : StringAlignmentCenter);
    if (!wrap) { sf.SetFormatFlags(StringFormatFlagsNoWrap); sf.SetTrimming(StringTrimmingEllipsisPath); }
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &sf, &b);
}
static float TextHeight(Graphics& g, const std::wstring& s, Font* f, float width) {
    RectF out;
    g.MeasureString(s.c_str(), -1, f, RectF(0, 0, width, 1000), &out);
    return out.Height;
}
static void Glyph(Graphics& g, wchar_t ch, Font* f, Color c, RectF r) { Text(g, std::wstring(1, ch), f, c, r, A_CENTER); }

static void AddHot(RectF r, int id) { g_hots.push_back({ r, id }); }

enum BtnStyle { B_PRIMARY, B_GHOST, B_DANGER };
static void Button(Graphics& g, RectF r, int id, const std::wstring& label, BtnStyle st, bool enabled = true, wchar_t icon = 0) {
    bool hot = enabled && g_hot == id, down = hot && g_press == id;
    float rad = r.Height / 2;
    if (st == B_PRIMARY || st == B_DANGER) {
        if (!enabled) FillRound(g, r, rad, C(0x2A2F3A));
        else {
            DWORD a = st == B_DANGER ? kBad : kAccent, b = st == B_DANGER ? 0xFF7A5C : kAccent2;
            LinearGradientBrush lg(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y), hot ? Mix(a, 0xFFFFFF, 0.14f) : C(a),
                                   hot ? Mix(b, 0xFFFFFF, 0.14f) : C(b));
            if (down) { r.Y += 1; }
            FillRound(g, r, rad, lg);
        }
    } else {
        if (hot) FillRound(g, r, rad, C(0xFFFFFF, down ? 22 : 14));
        StrokeRound(g, r, rad, C(hot ? 0x3A4150 : kBorder));
    }
    Color tc = !enabled ? C(kFaint) : (st == B_GHOST ? C(kText) : C(0xFFFFFF));
    if (icon) {
        RectF m;
        g.MeasureString(label.c_str(), -1, g_fBtn, PointF(0, 0), &m);
        float total = 18 + 8 + m.Width, x = r.X + (r.Width - total) / 2;
        Glyph(g, icon, g_fIcon, tc, RectF(x, r.Y, 18, r.Height));
        Text(g, label, g_fBtn, tc, RectF(x + 26, r.Y, m.Width + 4, r.Height));
    } else {
        Text(g, label, g_fBtn, tc, r, A_CENTER);
    }
    if (enabled) AddHot(r, id);
}

static void Toggle(Graphics& g, float x, float y, float t) {
    RectF tr(x, y, 44, 24);
    FillRound(g, tr, 12, Mix(0x303644, kAccent, t));
    SolidBrush knob(C(0xFFFFFF));
    g.FillEllipse(&knob, x + 4 + t * 20, y + 4, 16.f, 16.f);
}

static void IconBubble(Graphics& g, RectF r, wchar_t ch, DWORD tint, bool active) {
    SolidBrush b(C(tint, active ? 38 : 18));
    g.FillEllipse(&b, r);
    Glyph(g, ch, g_fIcon, active ? C(tint) : C(kFaint), r);
}

static void Pill(Graphics& g, float x, float cy, const std::wstring& s, DWORD tint) {
    RectF m;
    g.MeasureString(s.c_str(), -1, g_fSmall, PointF(0, 0), &m);
    RectF r(x, cy - 11, m.Width + 26, 22);
    FillRound(g, r, 11, C(tint, 30));
    SolidBrush dot(C(tint));
    g.FillEllipse(&dot, r.X + 9, cy - 3, 6.f, 6.f);
    Text(g, s, g_fSmall, C(tint), RectF(r.X + 19, r.Y, m.Width + 4, r.Height));
}

// The EchoXR logo; same geometry as tools/gen_logo.py (a 256-unit canvas).
static void DrawLogo(Graphics& g, RectF r) {
    float k = r.Width / 256.f;
    LinearGradientBrush bg(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y + r.Height), C(0x4F7BFF), C(0x9A5CFF));
    FillRound(g, r, 60 * k, bg);
    const float bars[4][4] = { { 44, 62, 76, 194 }, { 44, 62, 136, 94 }, { 44, 112, 118, 144 }, { 44, 162, 136, 194 } };
    for (auto& b : bars) {
        RectF br(r.X + b[0] * k, r.Y + b[1] * k, (b[2] - b[0]) * k, (b[3] - b[1]) * k);
        FillRound(g, br, min(br.Width, br.Height) / 2 - 0.01f, C(0xFFFFFF));
    }
    const float arcs[2][2] = { { 52, 255 }, { 86, 140 } };   // radius, alpha
    for (auto& a : arcs) {
        Pen pen(C(0xFFFFFF, (BYTE)a[1]), 20 * k);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        float cx = r.X + 124 * k, cy = r.Y + 128 * k, rad = a[0] * k;
        g.DrawArc(&pen, cx - rad, cy - rad, rad * 2, rad * 2, -48.f, 96.f);
    }
}

static void Header(Graphics& g, const wchar_t* title, const wchar_t* sub) {
    const float kW = g_uiW;
    LinearGradientBrush lg(PointF(0, 0), PointF(0, 150), C(0x18214A), C(kBg));
    g.FillRectangle(&lg, 0.f, 0.f, kW, 150.f);
    SolidBrush glow1(C(kAccent2, 14)), glow2(C(kAccent, 10));
    g.FillEllipse(&glow1, kW - 190, -120.f, 300.f, 240.f);
    g.FillEllipse(&glow2, kW - 330, -150.f, 260.f, 220.f);
    DrawLogo(g, RectF(32, 36, 56, 56));
    Text(g, title, g_fTitle, C(kText), RectF(104, 36, kW - 140, 32));
    Text(g, sub, g_fBody, C(kMuted), RectF(104, 68, kW - 140, 24));
}

static void SectionLabel(Graphics& g, float y, const wchar_t* s) {
    Text(g, s, g_fLabel, C(kFaint), RectF(34, y, 400, 16));
}
