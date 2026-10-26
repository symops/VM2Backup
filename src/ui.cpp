#include "ui.h"
#include "winutil.h"
#include <shlobj.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <wchar.h>

namespace ui {

const COLORREF kBlue = RGB(0, 120, 232), kBlueDark = RGB(0, 90, 180), kBlueLight = RGB(214, 232, 250), kRed = RGB(214, 40, 40),
               kGreen = RGB(30, 150, 70), kAmber = RGB(210, 130, 0), kGrey = RGB(120, 120, 120), kText = RGB(40, 40, 40), kBorder = RGB(190, 190, 190);

UINT DpiOf(HWND h)
{
    typedef UINT(WINAPI* Fn)(HWND);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && h) { UINT d = fn(h); if (d) return d; }
    HDC dc = GetDC(nullptr); UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

int Sc(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

HFONT MakeFont(UINT dpi, bool bold, int sizeDelta)
{
    LOGFONTW lf; memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(9 + sizeDelta, (int)dpi, 72); lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL; lf.lfCharSet = DEFAULT_CHARSET; lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

Str U8(const wchar_t* w) { return vm2b::FromWide(w); }
Vec<wchar_t> W(const Str& s) { vm2b::Wide w(s); size_t n = wcslen(w.c_str()) + 1; Vec<wchar_t> v; v.resize(n); memcpy(v.data(), w.c_str(), n * sizeof(wchar_t)); return v; }
const wchar_t* TrW(const char* en) { return Tr(en); }

Str GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return Str();
    Vec<wchar_t> b; b.resize((size_t)n + 1);
    GetWindowTextW(h, b.data(), n + 1);
    return U8(b.data());
}

void SetText(HWND h, const Str& s) { Vec<wchar_t> w = W(s); SetWindowTextW(h, w.data()); }

bool InitUi()
{
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_DATE_CLASSES | ICC_UPDOWN_CLASS };
    return InitCommonControlsEx(&ic) != 0;
}

// ---- Form ------------------------------------------------------------------------------------------------------
static const wchar_t* kFormClass = L"VM2BackupForm";
int Form::s_autoCloseMs = 0;
Vec<Str>* Form::s_layoutLog = nullptr;
void (*Form::s_onShown)(Form*) = nullptr;
int Form::s_shown = 0;
static bool g_classRegistered;

LRESULT CALLBACK FormProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    Form* f = (Form*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (m == WM_NCCREATE) { CREATESTRUCTW* cs = (CREATESTRUCTW*)lp; SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams); f = (Form*)cs->lpCreateParams; if (f) f->m_wnd = hw; }
    if (!f) return DefWindowProcW(hw, m, wp, lp);
    return f->handle(m, wp, lp);
}

Form::Form() : m_wnd(nullptr), m_owner(nullptr), m_dpi(96), m_font(nullptr), m_fontBold(nullptr), m_fontTitle(nullptr), m_fontS1(nullptr), m_fontS2(nullptr), m_curPage(-1), m_shownPage(0), m_result(IDCANCEL),
               m_modal(false), m_header(true), m_done(false), m_child(false), m_white(nullptr), m_tabsId(0), m_w(0), m_h(0) {}

Form::~Form()
{
    if (m_font) DeleteObject(m_font); if (m_fontBold) DeleteObject(m_fontBold); if (m_fontTitle) DeleteObject(m_fontTitle); if (m_fontS1) DeleteObject(m_fontS1); if (m_fontS2) DeleteObject(m_fontS2); if (m_white) DeleteObject(m_white);
}

static void RegisterFormClass()
{
    if (g_classRegistered) return;
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = FormProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kFormClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    RegisterClassW(&wc);
    g_classRegistered = true;
}

void Form::applyFonts()
{
    if (m_font) DeleteObject(m_font); if (m_fontBold) DeleteObject(m_fontBold); if (m_fontTitle) DeleteObject(m_fontTitle); if (m_fontS1) DeleteObject(m_fontS1); if (m_fontS2) DeleteObject(m_fontS2);
    m_font = MakeFont(m_dpi); m_fontBold = MakeFont(m_dpi, true); m_fontTitle = MakeFont(m_dpi, true, 1); m_fontS1 = MakeFont(m_dpi, false, -1); m_fontS2 = MakeFont(m_dpi, false, -2);
    for (size_t i = 0; i < m_items.size(); ++i) SendMessageW(m_items[i].h, WM_SETFONT, (WPARAM)(m_items[i].bold ? m_fontBold : m_font), TRUE);
}

void Form::destroyControls()
{
    for (size_t i = 0; i < m_items.size(); ++i) DestroyWindow(m_items[i].h);
    m_items.clear(); m_tabsId = 0; m_curPage = -1; m_shownPage = 0;
}

void Form::relayout()
{
    for (size_t i = 0; i < m_items.size(); ++i) {
        const Item& it = m_items[i]; RECT r = it.du;
        SetWindowPos(it.h, nullptr, sc(r.left), sc(r.top), sc(r.right - r.left), sc(r.bottom - r.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

HWND Form::add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex, int page)
{
    if (page == -2) page = m_curPage;
    int yy = y + ((m_header && !m_child) ? kHeaderH : 0);
    HWND c = CreateWindowExW(ex, cls, text ? text : L"", WS_CHILD | WS_VISIBLE | style, sc(x), sc(yy), sc(w), sc(h), m_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)m_font, TRUE);
    Item it; it.h = c; it.du.left = x; it.du.top = yy; it.du.right = x + w; it.du.bottom = yy + h; it.page = page; it.bold = false; it.lvl = 0;
    m_items.push_back(it);
    if (page >= 0 && page != m_shownPage) ShowWindow(c, SW_HIDE);
    return c;
}

HWND Form::label(int x, int y, int w, int h, const wchar_t* text, int id, DWORD style) { return add(L"STATIC", text, SS_LEFT | style, x, y, w, h, id); }
HWND Form::edit(int id, int x, int y, int w, int h, const Str& text, DWORD style)
{
    Vec<wchar_t> t = W(text);
    return add(L"EDIT", t.data(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | style, x, y, w, h, id, WS_EX_CLIENTEDGE * 0);
}
HWND Form::password(int id, int x, int y, int w, int h) { return add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, x, y, w, h, id); }
HWND Form::checkbox(int id, int x, int y, int w, const wchar_t* text, bool on, int h)
{
    HWND c = add(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, h, id);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}
HWND Form::radio(int id, int x, int y, int w, const wchar_t* text, bool first, int h) { return add(L"BUTTON", text, BS_AUTORADIOBUTTON | (first ? WS_GROUP | WS_TABSTOP : 0), x, y, w, h, id); }
HWND Form::combo(int id, int x, int y, int w, int dropH) { return add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, dropH, id); }
HWND Form::button(int id, int x, int y, int w, int h, const wchar_t* text, bool def) { if (def) m_defId = id; return add(L"BUTTON", text, WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y, w, h, id); }
HWND Form::group(int x, int y, int w, int h, const wchar_t* text) { return add(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, -1); }
HWND Form::listview(int id, int x, int y, int w, int h, bool checks)
{
    HWND c = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, x, y, w, h, id);
    ListView_SetExtendedListViewStyle(c, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | (checks ? LVS_EX_CHECKBOXES : 0));
    SetWindowTheme(c, L"Explorer", nullptr);
    return c;
}
HWND Form::tree(int id, int x, int y, int w, int h)
{
    HWND c = add(WC_TREEVIEWW, L"", WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS, x, y, w, h, id);
    SetWindowTheme(c, L"Explorer", nullptr);
    return c;
}

HWND Form::tabs(int id, int x, int y, int w, const Vec<Str>& names)
{
    HWND c = add(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, x, y, w, 28, id, 0, -1);
    for (size_t i = 0; i < names.size(); ++i) { Vec<wchar_t> t = W(names[i]); TCITEMW ti; memset(&ti, 0, sizeof(ti)); ti.mask = TCIF_TEXT; ti.pszText = t.data(); TabCtrl_InsertItem(c, (int)i, &ti); }
    m_tabsId = id;
    return c;
}

void Form::place(int id, int x, int y, int w, int h)
{
    HWND c = ctl(id);
    for (size_t i = 0; i < m_items.size(); ++i) if (m_items[i].h == c) {
        int yy = y + ((m_header && !m_child) ? kHeaderH : 0);
        m_items[i].du.left = x; m_items[i].du.top = yy; m_items[i].du.right = x + w; m_items[i].du.bottom = yy + h;
        SetWindowPos(c, nullptr, sc(x), sc(yy), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE); fitItem(i); return;
    }
}

void Form::showPage(int page)
{
    m_shownPage = page;
    for (size_t i = 0; i < m_items.size(); ++i) if (m_items[i].page >= 0) ShowWindow(m_items[i].h, m_items[i].page == page ? SW_SHOW : SW_HIDE);
    if (m_tabsId) TabCtrl_SetCurSel(ctl(m_tabsId), page);
    onPageShown(page);
}

int Form::comboAdd(int id, const Str& text, i64 data)
{
    Vec<wchar_t> t = W(text);
    int i = (int)SendMessageW(ctl(id), CB_ADDSTRING, 0, (LPARAM)t.data());
    SendMessageW(ctl(id), CB_SETITEMDATA, i, (LPARAM)data);
    return i;
}
i64 Form::comboData(int id, int i) const { if (i < 0) i = comboSel(id); return i < 0 ? 0 : (i64)SendMessageW(ctl(id), CB_GETITEMDATA, i, 0); }
void Form::comboSetData(int id, i64 data)
{
    int n = (int)SendMessageW(ctl(id), CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) if ((i64)SendMessageW(ctl(id), CB_GETITEMDATA, i, 0) == data) { SendMessageW(ctl(id), CB_SETCURSEL, i, 0); return; }
}

void Form::message(const Str& text, bool error)
{
    Vec<wchar_t> t = W(text);
    MessageBoxW(m_wnd, t.data(), TrW("VM2Backup"), MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}
bool Form::confirm(const Str& text) { Vec<wchar_t> t = W(text); return MessageBoxW(m_wnd, t.data(), TrW("VM2Backup"), MB_YESNO | MB_ICONQUESTION) == IDYES; }

void Form::close(int result) { m_result = result; m_done = true; if (m_wnd) PostMessageW(m_wnd, WM_NULL, 0, 0); }

bool Form::createChild(HWND parent, int x, int y, int w, int h)
{
    RegisterFormClass();
    m_child = true; m_header = false; m_owner = parent; m_dpi = DpiOf(parent);
    m_white = CreateSolidBrush(RGB(255, 255, 255));
    CreateWindowExW(0, kFormClass, L"", WS_CHILD | WS_VISIBLE, sc(x), sc(y), sc(w), sc(h), parent, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_wnd) return false;
    m_w = w; m_h = h;
    applyFonts();
    onInit();
    fitAll();
    return true;
}

static BOOL AdjustWindowRectExForDpi_Compat(LPRECT r, DWORD style, BOOL menu, UINT dpi);

bool Form::createTop(const Str& title, int w, int h, int showCmd)
{
    RegisterFormClass();
    m_top = true; m_header = true; m_title = title; m_dpi = DpiOf(nullptr); m_w = w; m_h = h;
    m_white = CreateSolidBrush(RGB(255, 255, 255));
    Vec<wchar_t> t = W(title);
    // a fixed-size panel: caption + minimize + close, no sizing border; w x h is then the client area; no WS_CLIPCHILDREN (the group box needs the white behind it)
    DWORD style = m_fixed ? (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) : (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
    DWORD exStyle = m_fixed ? WS_EX_DLGMODALFRAME : 0;
    auto outer = [&](int& ow, int& oh) { RECT r = { 0, 0, sc(w), sc(h + kHeaderH) }; if (m_fixed) AdjustWindowRectExForDpi_Compat(&r, style, 0, m_dpi); ow = m_fixed ? r.right - r.left : sc(w); oh = m_fixed ? r.bottom - r.top : sc(h + kHeaderH); };
    int ow, oh; outer(ow, oh);
    CreateWindowExW(exStyle, kFormClass, t.data(), style, CW_USEDEFAULT, CW_USEDEFAULT, ow, oh, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_wnd) return false;
    { UINT d = DpiOf(m_wnd); if (d != m_dpi) { m_dpi = d; outer(ow, oh); SetWindowPos(m_wnd, nullptr, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); } }      // the window was sized for the system dpi, it opened on a monitor with another one
    applyFonts();
    onInit();
    relayout();
    fitAll();
    ShowWindow(m_wnd, showCmd);
    UpdateWindow(m_wnd);
    return true;
}

void Form::move(int x, int y, int w, int h) { m_w = w; m_h = h; SetWindowPos(m_wnd, nullptr, sc(x), sc(y), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE); }

static BOOL AdjustWindowRectExForDpi_Compat(LPRECT r, DWORD style, BOOL menu, UINT dpi)
{
    typedef BOOL(WINAPI* Fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    return fn ? fn(r, style, menu, WS_EX_DLGMODALFRAME, dpi) : AdjustWindowRectEx(r, style, menu, WS_EX_DLGMODALFRAME);
}

int Form::runModal(HWND owner, const Str& title, int w, int h, bool header)
{
    RegisterFormClass();
    m_owner = owner; m_modal = true; m_header = header; m_title = title; m_done = false; m_result = IDCANCEL; m_w = w; m_h = h;
    m_dpi = owner ? DpiOf(owner) : DpiOf(nullptr);
    m_white = CreateSolidBrush(RGB(255, 255, 255));
    RECT r = { 0, 0, sc(w), sc(h + (header ? kHeaderH : 0)) };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;                       // no WS_CLIPCHILDREN: the group boxes need the parent's white behind them
    AdjustWindowRectExForDpi_Compat(&r, style, 0, m_dpi);
    Vec<wchar_t> t = W(title);
    CreateWindowExW(WS_EX_DLGMODALFRAME, kFormClass, t.data(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_wnd) return IDCANCEL;
    { UINT d = DpiOf(m_wnd); if (d != m_dpi) { m_dpi = d; RECT r2 = { 0, 0, sc(w), sc(h + (header ? kHeaderH : 0)) }; AdjustWindowRectExForDpi_Compat(&r2, style, 0, m_dpi); SetWindowPos(m_wnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); } }
    applyFonts();
    onInit();
    relayout();
    fitAll();
    // centre over the owner
    RECT wr, orc; GetWindowRect(m_wnd, &wr);
    if (owner && GetWindowRect(owner, &orc)) SetWindowPos(m_wnd, nullptr, orc.left + ((orc.right - orc.left) - (wr.right - wr.left)) / 2, orc.top + ((orc.bottom - orc.top) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    if (owner) EnableWindow(owner, FALSE);
    ++s_shown;
    if (s_autoCloseMs > 0) SetTimer(m_wnd, 0xFFF0, (UINT)s_autoCloseMs, nullptr);
    if (s_layoutLog) checkLayout(*s_layoutLog);
    if (s_onShown) SetTimer(m_wnd, 0xFFF1, 30, nullptr);
    ShowWindow(m_wnd, SW_SHOW);
    UpdateWindow(m_wnd);
    MSG msg;
    while (!m_done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(m_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DestroyWindow(m_wnd); m_wnd = nullptr;
    return m_result;
}

LRESULT Form::handle(UINT m, WPARAM wp, LPARAM lp)
{
    bool handled = false;
    LRESULT r = onMessage(m, wp, lp, handled);
    if (handled) return r;
    switch (m) {
    case DM_GETDEFID: if ((m_modal || m_top) && m_defId && ctl(m_defId)) return MAKELONG(m_defId, DC_HASDEFID); break;   // IsDialogMessage: Enter presses this button (without it, Enter sends IDOK and closes a wizard half way)
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (m_tabsId == 0 || id != m_tabsId) {
            if (onCommand(id, code)) return 0;
            if (m_modal && code == BN_CLICKED) {
                if (id == IDOK) { if (onOk()) close(IDOK); return 0; }
                if (id == IDCANCEL) { close(IDCANCEL); return 0; }
            }
        }
        break; }
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        if (m_tabsId && h->idFrom == (UINT_PTR)m_tabsId && h->code == TCN_SELCHANGE) { showPage(TabCtrl_GetCurSel(h->hwndFrom)); return 0; }
        if (onNotify(h)) return 0;
        break; }
    case WM_TIMER:
        if (wp == 0xFFF1) { KillTimer(m_wnd, 0xFFF1); if (s_onShown) s_onShown(this); return 0; }
        if (wp == 0xFFF0) { KillTimer(m_wnd, 0xFFF0); if (s_layoutLog) checkLayout(*s_layoutLog); close(IDCANCEL); return 0; }
        onTimer(wp); return 0;
    case WM_SIZE: onSize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_DESTROY: if (m_top) PostQuitMessage(0); return 0;
    case WM_CLOSE: if (m_modal) { close(IDCANCEL); return 0; } break;
    case WM_ERASEBKGND: { RECT rc; GetClientRect(m_wnd, &rc); FillRect((HDC)wp, &rc, m_white); return 1; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: { HDC dc = (HDC)wp; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, kText); return (LRESULT)m_white; }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: { HDC dc = (HDC)wp; SetTextColor(dc, kText); SetBkColor(dc, RGB(255, 255, 255)); return (LRESULT)m_white; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(m_wnd, &ps);
        RECT rc; GetClientRect(m_wnd, &rc);
        if (m_header && !m_child) {
            RECT band = { 0, 0, rc.right, sc(kHeaderH) };
            HBRUSH b = CreateSolidBrush(kBlue); FillRect(dc, &band, b); DeleteObject(b);
            HFONT old = (HFONT)SelectObject(dc, m_fontTitle); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            RECT tr = { sc(16), 0, rc.right - sc(40), sc(kHeaderH) };
            Vec<wchar_t> t = W(m_title); DrawTextW(dc, t.data(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(dc, old);
        }
        onPaintExtra(dc, rc);
        EndPaint(m_wnd, &ps); return 0; }
    case WM_DPICHANGED: {
        m_dpi = HIWORD(wp); const RECT* sg = (const RECT*)lp;
        applyFonts(); relayout(); fitAll();
        if (sg && !m_child) SetWindowPos(m_wnd, nullptr, sg->left, sg->top, sg->right - sg->left, sg->bottom - sg->top, SWP_NOZORDER | SWP_NOACTIVATE);
        onDpiChanged();
        InvalidateRect(m_wnd, nullptr, TRUE);
        return 0; }
    }
    return DefWindowProcW(m_wnd, m, wp, lp);
}


// ---- layout check / fit -----------------------------------------------------------------------------------------
static Str Clip(const Str& s, size_t n) { return s.size() > n ? s.substr(0, n) + "..." : s; }

// Does the text of control `h` overflow it when drawn in font `f`?  need / have are in pixels (width, or height for wrapping labels).
static bool ItemOverflow(HDC dc, HWND h, HFONT f, UINT dpi, int& need, int& have, const char*& kind, Str* what)
{
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    RECT wr; GetWindowRect(h, &wr); int w = wr.right - wr.left, hh = wr.bottom - wr.top;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    HFONT old = f ? (HFONT)SelectObject(dc, f) : nullptr; bool over = false;
    int sc10 = Sc(10, dpi), sc16 = Sc(16, dpi), sc20 = Sc(20, dpi);
    if (!_wcsicmp(cls, L"ComboBox")) {
        int n = (int)SendMessageW(h, CB_GETCOUNT, 0, 0), avail = w - GetSystemMetrics(SM_CXVSCROLL) - Sc(8, dpi);
        for (int k = 0; k < n; ++k) {
            int l = (int)SendMessageW(h, CB_GETLBTEXTLEN, k, 0); if (l <= 0 || l > 1000) continue;
            Vec<wchar_t> b; b.resize((size_t)l + 1); SendMessageW(h, CB_GETLBTEXT, k, (LPARAM)b.data());
            SIZE sz; GetTextExtentPoint32W(dc, b.data(), l, &sz);
            if (sz.cx > avail + Sc(4, dpi) && sz.cx - avail > need - have) { over = true; need = sz.cx; have = avail; kind = "combo box item"; if (what) *what = U8(b.data()); }
        }
    } else {
        Str text = GetText(h); Vec<wchar_t> wt = W(text); int len = (int)wcslen(wt.data());
        if (len > 0) {
            if (!_wcsicmp(cls, L"Static") && !(st & (SS_ICON | SS_BITMAP | SS_OWNERDRAW | SS_BLACKRECT | SS_GRAYRECT | SS_WHITERECT))) {
                bool one = (st & SS_TYPEMASK) == SS_LEFTNOWORDWRAP || (st & SS_TYPEMASK) == SS_SIMPLE;
                RECT r = { 0, 0, w, 0 }; DrawTextW(dc, wt.data(), len, &r, DT_CALCRECT | DT_EXPANDTABS | DT_NOPREFIX | (one ? DT_SINGLELINE : DT_WORDBREAK));
                int n2 = one ? r.right : r.bottom, h2 = one ? w : hh + Sc(3, dpi);
                if (n2 > h2) { over = true; need = n2; have = one ? w : hh; kind = "label"; }
            } else if (!_wcsicmp(cls, L"Button")) {
                UINT ty = (UINT)(st & BS_TYPEMASK); bool push = ty == BS_PUSHBUTTON || ty == BS_DEFPUSHBUTTON;
                int pad = ty == BS_GROUPBOX ? sc16 : push ? sc10 : sc20;
                if (st & BS_MULTILINE) { RECT r = { 0, 0, w - pad, 0 }; DrawTextW(dc, wt.data(), len, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX); if (r.bottom > hh - Sc(2, dpi)) { over = true; need = r.bottom; have = hh; kind = "multi-line button"; } }
                else { SIZE sz; GetTextExtentPoint32W(dc, wt.data(), len, &sz); if (sz.cx + pad > w) { over = true; need = sz.cx + pad; have = w; kind = ty == BS_GROUPBOX ? "group box title" : push ? "button" : "check box / radio"; } }
            }
        }
        if (over && what) *what = text;
    }
    if (old) SelectObject(dc, old);
    return over;
}

// A translation that does not fit its control is drawn one or two sizes smaller (9 -> 8 -> 7 pt) instead of being cut off.
void Form::fitItem(size_t i)
{
    if (i >= m_items.size() || !m_wnd || !m_font) return;
    HWND h = m_items[i].h; if (!IsWindow(h)) return;
    HFONT fonts[3] = { m_font, m_fontS1, m_fontS2 };
    HFONT cur = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0); bool known = false; for (HFONT f : fonts) if (f == cur) known = true;
    if (!known) return;                                                    // a control with a font of its own (a big heading)
    HDC dc = GetDC(m_wnd); int lvl = 0;
    for (; lvl < 3; ++lvl) { int need = 0, have = 0; const char* kind = ""; if (!ItemOverflow(dc, h, fonts[lvl], m_dpi, need, have, kind, nullptr)) break; }
    if (lvl == 3) lvl = 2;
    ReleaseDC(m_wnd, dc);
    if (fonts[lvl] != cur) { SendMessageW(h, WM_SETFONT, (WPARAM)fonts[lvl], TRUE); InvalidateRect(h, nullptr, TRUE); }
    m_items[i].lvl = lvl;
}
void Form::fitAll() { for (size_t i = 0; i < m_items.size(); ++i) fitItem(i); }
void Form::setText(int id, const Str& s)
{
    HWND c = ctl(id); SetText(c, s);
    for (size_t i = 0; i < m_items.size(); ++i) if (m_items[i].h == c) { fitItem(i); break; }
}

int Form::checkLayout(Vec<Str>& out)
{
    int found = 0; RECT cr; GetClientRect(m_wnd, &cr);
    HDC dc = GetDC(m_wnd);
    for (size_t i = 0; i < m_items.size(); ++i) {
        HWND h = m_items[i].h; if (!IsWindow(h)) continue;
        wchar_t cls[64]; GetClassNameW(h, cls, 64);
        RECT wr; GetWindowRect(h, &wr); int w = wr.right - wr.left, hh = wr.bottom - wr.top;
        POINT tl = { wr.left, wr.top }; ScreenToClient(m_wnd, &tl);
        Str text = GetText(h);
        auto report = [&](const Str& kind, int need, int have) {
            ++found; out.push_back(Str::fmt("%s | %s \"%s\" needs %d px, has %d px (x%.2f) [control %dx%d px, %u dpi]", m_title.c_str(), kind.c_str(), Clip(text, 70).c_str(), need, have, have > 0 ? (double)need / have : 9.9, w, hh, m_dpi));
        };
        if ((m_modal || m_top) && !m_child && tl.x + w > cr.right + 1) report("outside of the form (right): " + U8(cls), tl.x + w, cr.right);
        if ((m_modal || m_top) && !m_child && tl.y + hh > cr.bottom + 1) report("outside of the form (bottom): " + U8(cls), tl.y + hh, cr.bottom);
        int need = 0, have = 0; const char* kind = ""; Str what = text;
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        if (ItemOverflow(dc, h, f, m_dpi, need, have, kind, &what)) { text = what; report(kind, need, have); }
        if (!_wcsicmp(cls, L"SysTabControl32")) {
            int n = TabCtrl_GetItemCount(h);
            if (n > 0) { RECT last; TabCtrl_GetItemRect(h, n - 1, &last); if (TabCtrl_GetRowCount(h) > 1 || last.right > w) report("tab strip (tabs do not fit in one row)", last.right, w); }
        } else if (!_wcsicmp(cls, L"SysListView32") && f) {
            HWND hd = ListView_GetHeader(h); int nc = hd ? Header_GetItemCount(hd) : 0; HFONT o2 = (HFONT)SelectObject(dc, f);
            for (int k = 0; k < nc; ++k) {
                wchar_t b[256]; LVCOLUMNW c; memset(&c, 0, sizeof(c)); c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = b; c.cchTextMax = 256;
                if (!ListView_GetColumn(h, k, &c)) continue;
                SIZE sz; GetTextExtentPoint32W(dc, b, (int)wcslen(b), &sz);
                if (sz.cx + sc(8) > c.cx) { text = U8(b); report("list column header", sz.cx + sc(12), c.cx); }
            }
            SelectObject(dc, o2);
        }
    }
    ReleaseDC(m_wnd, dc);
    return found;
}

// ---- list views ------------------------------------------------------------------------------------------------
int LvAddColumn(HWND lv, const wchar_t* title, int widthDu, UINT dpi, int fmt)
{
    LVCOLUMNW c; memset(&c, 0, sizeof(c)); c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.pszText = (LPWSTR)title; c.cx = Sc(widthDu, dpi); c.fmt = fmt;
    { HDC dc = GetDC(lv); HFONT f = (HFONT)SendMessageW(lv, WM_GETFONT, 0, 0), o = f ? (HFONT)SelectObject(dc, f) : nullptr; SIZE sz = { 0, 0 };      // a long translated title widens the column
      GetTextExtentPoint32W(dc, title, (int)wcslen(title), &sz); if (o) SelectObject(dc, o); ReleaseDC(lv, dc); if (sz.cx + Sc(14, dpi) > c.cx) c.cx = sz.cx + Sc(14, dpi); }
    return ListView_InsertColumn(lv, Header_GetItemCount(ListView_GetHeader(lv)), &c);
}
int LvAdd(HWND lv, const Str& first, i64 param)
{
    Vec<wchar_t> t = W(first);
    LVITEMW it; memset(&it, 0, sizeof(it)); it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = ListView_GetItemCount(lv); it.pszText = t.data(); it.lParam = (LPARAM)param;
    return ListView_InsertItem(lv, &it);
}
void LvSet(HWND lv, int row, int col, const Str& text) { Vec<wchar_t> t = W(text); ListView_SetItemText(lv, row, col, t.data()); }
int LvSel(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }
i64 LvParam(HWND lv, int row) { LVITEMW it; memset(&it, 0, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = row; return ListView_GetItem(lv, &it) ? (i64)it.lParam : 0; }

// ---- common dialogs --------------------------------------------------------------------------------------------
static int CALLBACK FolderInit(HWND h, UINT m, LPARAM, LPARAM data) { if (m == BFFM_INITIALIZED && data) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data); return 0; }

bool PickFolder(HWND owner, const Str& title, Str& path)
{
    wchar_t pick[MAX_PATH]; Vec<wchar_t> t = W(title), cur = W(path);
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi)); bi.hwndOwner = owner; bi.pszDisplayName = pick; bi.lpszTitle = t.data();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE; bi.lpfn = FolderInit; bi.lParam = (LPARAM)cur.data();
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    bool ok = SHGetPathFromIDListW(pidl, pick) != 0;
    CoTaskMemFree(pidl);
    if (ok) path = U8(pick);
    return ok;
}

bool PickNetworkFolder(HWND owner, const Str& title, Str& path)
{
    wchar_t pick[MAX_PATH]; Vec<wchar_t> t = W(title);
    PIDLIST_ABSOLUTE root = nullptr; SHGetSpecialFolderLocation(owner, CSIDL_NETWORK, &root);
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi)); bi.hwndOwner = owner; bi.pszDisplayName = pick; bi.lpszTitle = t.data(); bi.pidlRoot = root;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (root) CoTaskMemFree(root);
    if (!pidl) return false;
    bool ok = SHGetPathFromIDListW(pidl, pick) != 0;
    CoTaskMemFree(pidl);
    if (ok) path = U8(pick);
    return ok;
}

static bool PickFile(HWND owner, const Str& title, const wchar_t* filter, const wchar_t* defExt, Str& path, bool save)
{
    wchar_t buf[MAX_PATH * 2]; Vec<wchar_t> cur = W(path); wcsncpy(buf, cur.data(), MAX_PATH * 2 - 1); buf[MAX_PATH * 2 - 1] = 0;
    Vec<wchar_t> t = W(title);
    OPENFILENAMEW o; memset(&o, 0, sizeof(o)); o.lStructSize = sizeof(o); o.hwndOwner = owner; o.lpstrFilter = filter; o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2; o.lpstrTitle = t.data();
    o.lpstrDefExt = defExt; o.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o))) return false;
    path = U8(buf); return true;
}
bool PickOpenFile(HWND owner, const Str& title, const wchar_t* filter, Str& path) { return PickFile(owner, title, filter, nullptr, path, false); }
bool PickSaveFile(HWND owner, const Str& title, const wchar_t* filter, const wchar_t* defExt, Str& path) { return PickFile(owner, title, filter, defExt, path, true); }

Str FormatBytes(double b) { return vm2b::FormatSize((vm2b::u64)(b < 0 ? 0 : b)); }
Str FormatRate(double r) { return FormatBytes(r) + "/s"; }
Str FormatTime(double t)
{
    if (t <= 0) return Str();
    Str iso = vm2b::LocalIso(t);
    return iso.substr(0, 16).replace_all("T", " ");
}

} // namespace ui
