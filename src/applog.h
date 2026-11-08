// The program log (applog.cpp): %ProgramData%\VM2Backup\logs\<name>.log, UTF-8 with a BOM, rotated at 1 MB.
#pragma once
void AppLogOpen(const wchar_t* name);
void Log(const wchar_t* fmt, ...);
const wchar_t* AppLogDir();                        // the folder of the logs (after AppLogOpen)
extern const wchar_t* const kInstallDirSddl;         // protected DACL of the installation folder (SYSTEM, Administrators: full; Users: read and execute)
extern const wchar_t* const kInstallFileSddl;        // the same for the files in it
bool SecureInstallDir(const wchar_t* dir, const wchar_t* const* files, int nfiles);
