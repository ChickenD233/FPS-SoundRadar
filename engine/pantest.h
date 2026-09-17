// pantest.h - acceptance-test tone player for the SoundRadar speaker endpoint.
#pragma once

namespace sr {

// Plays the pan test pattern on the "SoundRadar"+"Speaker" render endpoint.
// seconds <= 0: play the pattern once; otherwise loop until the budget is used.
// Returns 0 ok, 1 generic failure, 2 SoundRadar speaker endpoint absent.
int RunPanTest(int seconds);

} // namespace sr
