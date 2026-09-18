// devicedefault.h - set the Windows default render endpoint via IPolicyConfig
// (undocumented but stable since Vista; this is what the Sound panel does).
#pragma once

#include <string>

namespace sr {

// Sets the default render endpoint (console + multimedia roles) to the first
// render endpoint whose friendly name contains nameSubstring (ci).
// Logs the outcome to log.txt. Returns true on success.
bool SetDefaultRenderDevice(const std::wstring& nameSubstring);

// Maps a capture endpoint name to its render-side counterpart needle:
//   "CABLE Output" -> "CABLE In", "Voicemeeter Out ..." -> "Voicemeeter Input",
//   "SoundRadar ... Loopback" -> SoundRadar speaker (non-loopback).
// Returns empty if no mapping is known.
std::wstring RenderCounterpartNeedle(const std::wstring& captureName);

} // namespace sr
