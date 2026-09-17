// selftest.h - DSP self-tests, no audio devices required.
#pragma once

namespace sr {

// Runs all tests, prints PASS/FAIL per test. Returns 0 if all pass, 1 otherwise.
int RunSelfTest();

} // namespace sr
