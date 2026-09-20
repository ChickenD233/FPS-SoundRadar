// selftest_detect.h - adaptive detection self-test (--sselftest).
#pragma once

namespace sr {

// Returns 0 when every check passes, 1 otherwise. No audio devices required.
int RunDetectSelfTest();

} // namespace sr
