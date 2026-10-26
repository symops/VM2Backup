// VM2Backup — a small UI toolkit on plain Win32 (no frameworks), in the look of VM Explorer: a blue title band, white pages, blue
// tabs, grouped boxes. Everything is laid out in "design units" (96 dpi pixels) and scaled to the monitor, also when the window
// moves to a monitor with another scale.
#pragma once
#include "base.h"
#include "lang.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

namespace ui {

using vm2b::Str; using vm2b::Json; using vm2b::Vec; using vm2b::i64;

// colors (VM Explorer 7.1)
extern const COLORREF kBlue, kBlueDark, kBlueLight, kRed, kGreen, kAmber, kGrey, kText, kBorder;

UINT  DpiOf(HWND h);
int   Sc(int v, UINT dpi);
HFONT MakeFont(UINT dpi, bool bold = false, int sizeDelta = 0);
Str   U8(const wchar_t* w);                       // UTF-16 -> UTF-8
Vec<wchar_t> W(const Str& s);                     // UTF-8 -> UTF-16 (NUL-terminated)
Str   GetText(HWND h);
void  SetText(HWND h, const Str& s);
bool  InitUi();                                    // common controls, DPI
const wchar_t* TrW(const char* en);                // = Tr

// One window or dialog. Controls are added in design units; a control belongs to a "page" (-1: always shown) so that a tab control
// can switch between groups of controls.
class Form {
public:
    Form();
    virtual ~Form();
    HWND hwnd() const { return m_wnd; }
    UINT dpi() const { return m_dpi; }
    int  sc(int v) const { return Sc(v, m_dpi); }

    // modal use: returns the id of the button that closed the form (IDOK / IDCANCEL / ...)
    int runModal(HWND owner, const Str& title, int w, int h, bool header = true);
    // a normal top-level window (the main window); the caller runs the message loop
    bool createTop(const Str& title, int w, int h, int showCmd);
    void setFixedSize(bool on) { m_fixed = on; }                           // before createTop(): a window that cannot be resized (the control panel); w x h is its client area
    // use as a child panel of another window
    bool createChild(HWND parent, int x, int y, int w, int h);
    void close(int result);
    void move(int x, int y, int w, int h);          // design units (a child panel)

    // controls
    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0, int page = -2);
    HWND label(int x, int y, int w, int h, const wchar_t* text, int id = -1, DWORD style = 0);
    HWND edit(int id, int x, int y, int w, int h = 24, const Str& text = Str(), DWORD style = 0);
    HWND password(int id, int x, int y, int w, int h = 24);
    HWND checkbox(int id, int x, int y, int w, const wchar_t* text, bool on = false, int h = 20);
    HWND radio(int id, int x, int y, int w, const wchar_t* text, bool first = false, int h = 20);
    HWND combo(int id, int x, int y, int w, int dropH = 200);
    HWND button(int id, int x, int y, int w, int h, const wchar_t* text, bool def = false);
    HWND group(int x, int y, int w, int h, const wchar_t* text);
    HWND listview(int id, int x, int y, int w, int h, bool checkboxes = false);
    HWND tree(int id, int x, int y, int w, int h);
    HWND tabs(int id, int x, int y, int w, const Vec<Str>& names);          // the tab strip; pages are switched with showPage()
    void setPage(int page) { m_curPage = page; }                            // the page the next added controls belong to
    void showPage(int page);
    int  page() const { return m_shownPage; }
    void place(int id, int x, int y, int w, int h);                       // move / resize a control (design units)

    HWND ctl(int id) const { return GetDlgItem(m_wnd, id); }
    Str  text(int id) const { return GetText(ctl(id)); }
    void setText(int id, const Str& s);                                   // also re-fits the text to the control (smaller font when it does not fit)
    bool checked(int id) const { return IsDlgButtonChecked(m_wnd, id) == BST_CHECKED; }
    void setChecked(int id, bool on) { CheckDlgButton(m_wnd, id, on ? BST_CHECKED : BST_UNCHECKED); }
    void enable(int id, bool on) { EnableWindow(ctl(id), on); }
    void showCtl(int id, bool on) { ShowWindow(ctl(id), on ? SW_SHOW : SW_HIDE); }
    int  comboAdd(int id, const Str& text, i64 data = 0);
    int  comboSel(int id) const { return (int)SendMessageW(ctl(id), CB_GETCURSEL, 0, 0); }
    void comboSet(int id, int i) { SendMessageW(ctl(id), CB_SETCURSEL, i, 0); }
    i64  comboData(int id, int i = -1) const;
    void comboSetData(int id, i64 data);                                  // selects the item with this data

    void message(const Str& text, bool error = false);                    // a message box on this form
    bool confirm(const Str& text);                                        // Yes / No

    // overridables
    virtual void onInit() {}                                              // build the controls here
    virtual bool onCommand(int id, int code) { return false; }            // true = handled
    virtual bool onNotify(NMHDR* h) { return false; }
    virtual bool onOk() { return true; }                                  // IDOK pressed: false keeps the form open
    virtual void onTimer(UINT_PTR id) {}
    virtual LRESULT onMessage(UINT m, WPARAM wp, LPARAM lp, bool& handled) { handled = false; return 0; }
    virtual void onPaintExtra(HDC dc, const RECT& rc) {}
    virtual void onDpiChanged() {}
    virtual void onPageShown(int page) {}                                  // after showPage(): hide what does not apply (tab switch included)
    virtual void onSize(int cx, int cy) {}                                 // client size in pixels (top-level and child forms)

    void destroyControls();                                               // removes every control that was added (the language changed: build again)
    // Layout check (self test): lists the controls whose text does not fit (cut off, wrapped past the control, outside the form). Hidden pages are checked too.
    int  checkLayout(Vec<Str>& issues);
    static Vec<Str>* s_layoutLog;                                         // when set, every modal form is checked after it is built and just before it closes itself
    static void (*s_onShown)(Form*);                                      // self test: called (from a timer, inside the modal loop) once a modal form is on screen
    static int s_autoCloseMs;                                             // self test: modal forms close themselves after this many ms (0 = off)
    static int s_shown;                                                   // how many modal forms were shown (self test)
    static const int kHeaderH = 44;                                       // the blue band (design units)
protected:
    friend LRESULT CALLBACK FormProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT m, WPARAM wp, LPARAM lp);
    void relayout();
    void applyFonts();
    struct Item { HWND h; RECT du; int page; bool bold; int lvl; };
    void fitItem(size_t i); void fitAll();
    HWND m_wnd, m_owner; UINT m_dpi; HFONT m_font, m_fontBold, m_fontTitle, m_fontS1, m_fontS2; Vec<Item> m_items;
    int m_curPage, m_shownPage, m_result; bool m_modal, m_header, m_done, m_child; Str m_title; HBRUSH m_white; int m_tabsId;
    int m_w, m_h; bool m_top = false, m_fixed = false; int m_defId = 0;                  // m_defId: the default push button (answers DM_GETDEFID, so Enter presses it instead of IDOK)
};

// ---- list view helpers
int  LvAddColumn(HWND lv, const wchar_t* title, int widthDu, UINT dpi, int fmt = LVCFMT_LEFT);
int  LvAdd(HWND lv, const Str& first, i64 param = 0);
void LvSet(HWND lv, int row, int col, const Str& text);
int  LvSel(HWND lv);                                                // the selected row or -1
i64  LvParam(HWND lv, int row);

// ---- common dialogs
bool PickFolder(HWND owner, const Str& title, Str& path);
bool PickNetworkFolder(HWND owner, const Str& title, Str& path);   // starts at "Network"; a share or a folder in it (\\server\share\...)
bool PickOpenFile(HWND owner, const Str& title, const wchar_t* filter, Str& path);
bool PickSaveFile(HWND owner, const Str& title, const wchar_t* filter, const wchar_t* defExt, Str& path);
Str  FormatBytes(double b);
Str  FormatRate(double bytesPerSec);
Str  FormatTime(double unix);                                       // local "2026-10-24 08:00"

} // namespace ui
