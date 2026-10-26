// The program log (applog.cpp): %ProgramData%\VM2Backup\logs\<name>.log, UTF-8 with a BOM, rotated at 1 MB.
#pragma once
void AppLogOpen(const wchar_t* name);
void Log(const wchar_t* fmt, ...);
const wchar_t* AppLogDir();                        // the folder of the logs (after AppLogOpen)
