// log.h - append-only diagnostic log at %APPDATA%\SoundRadar\log.txt.
// Always active; this is how "won't open" reports get diagnosed.
#pragma once

#include <string>

namespace sr {

// Creates the directory if needed. Safe to call once at startup.
void LogInit();

// Appends one timestamped line. printf-style, narrow (UTF-8) and wide forms.
void Log(const char* fmt, ...);
void LogW(const wchar_t* fmt, ...);

} // namespace sr
