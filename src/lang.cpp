#include "lang.h"
#include "lang_table.inc"
#include <string.h>
#include <wchar.h>

// Primary Windows language id of each table (same order as kLangNames).
static const WORD kPrimary[kLangCount] = {
    LANG_ENGLISH, LANG_RUSSIAN, LANG_UKRAINIAN, LANG_BELARUSIAN, LANG_GERMAN, LANG_FRENCH, LANG_SPANISH, LANG_ITALIAN,
    LANG_PORTUGUESE, LANG_POLISH, LANG_DUTCH, LANG_TURKISH, LANG_INDONESIAN, LANG_VIETNAMESE, LANG_CHINESE,
    LANG_JAPANESE, LANG_KOREAN
};

static int g_lang = 0;                           // index in the tables

int LangCount() { return kLangCount; }
int LangCurrent() { return g_lang; }
const wchar_t* LangName(int index) { return (index >= 0 && index < kLangCount) ? kLangNames[index] : L""; }

static int IndexForId(LANGID id)
{
    WORD primary = PRIMARYLANGID(id);
    for (int i = 0; i < kLangCount; ++i) if (kPrimary[i] == primary) return i;
    return -1;
}

int LangAutoIndex()
{
    int i = IndexForId(GetUserDefaultUILanguage());       // the display language, then the regional settings
    if (i < 0) i = IndexForId(GetUserDefaultLangID());
    return i < 0 ? 0 : i;
}

void LangSelect(int setting)
{
    if (setting >= 1 && setting <= kLangCount) g_lang = setting - 1;
    else g_lang = LangAutoIndex();
}

// English texts of the table are sorted like strcmp sorts them (gen_lang.py), so a binary search finds one.
static const LangEntry* Find(const char* en)
{
    int lo = 0, hi = kStringCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(en, kStrings[mid].en);
        if (c == 0) return &kStrings[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return nullptr;
}

const wchar_t* Tr(const char* english)
{
    const LangEntry* e = Find(english);
    if (e && e->tr[g_lang]) return e->tr[g_lang];
    // not translated: the English text, converted once and kept (the same pointer every time)
    static struct Cache { const char* key; wchar_t* val; } cache[512];
    static int used;
    for (int i = 0; i < used; ++i) if (cache[i].key == english) return cache[i].val;
    int n = MultiByteToWideChar(CP_UTF8, 0, english, -1, nullptr, 0);
    wchar_t* w = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (n > 0 ? n : 1) * sizeof(wchar_t));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, english, -1, w, n); else w[0] = 0;
    if (used < 512) { cache[used].key = english; cache[used].val = w; ++used; }
    return w;
}
