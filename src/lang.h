// VM2Backup — interface strings in 17 languages. In the sources a string is written TR with the English text in quotes; the translations come
// from src/lang/<code>.txt through tools/gen_lang.py (lang_table.inc). The language is the Windows display language unless the
// user picks another one (Settings > Language); a text without a translation is shown in English.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

const wchar_t* Tr(const char* english);          // the text in the current language (the pointer stays valid)
#define TR(s) Tr(s)
#define TRK(s) s            // marks an English text kept in a table (translated later with Tr(); tools/gen_lang.py collects it too)

void LangSelect(int setting);                    // 0 = the Windows display language, 1..LangCount() = a language of the table
int  LangCount();
int  LangAutoIndex();                            // table index chosen for "system language"
int  LangCurrent();                              // table index in use (0-based)
const wchar_t* LangName(int index);              // native name of table language `index` (0-based)
