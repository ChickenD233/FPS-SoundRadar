// measure.h - latency measurement modes (--measure, --measure-loopback).
#pragma once

#include <string>

namespace sr {

// Full pipeline latency report: device periods, buffer depths, ring depth,
// render-side DAC estimation via IAudioClock. Runs ~3 seconds.
// Returns 0 ok, 1 generic failure, 2 SoundRadar endpoints absent.
int RunMeasure(const std::wstring& outputName, const std::wstring& captureDevice);

// Click-train round-trip: plays clicks on the SoundRadar render endpoint and
// detects their arrival in the SoundRadar loopback capture stream.
// Returns 0 ok, 1 generic failure, 2 SoundRadar endpoints absent.
int RunMeasureLoopback();

} // namespace sr
