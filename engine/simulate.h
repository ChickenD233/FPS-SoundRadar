// simulate.h - feeds synthetic meter data into SharedMeters without audio
// devices, for headless overlay verification (--simulate, --overlaytest).
#pragma once

#include <windows.h>

#include <atomic>
#include <thread>

#include "meters.h"

namespace sr {

enum SimScenario {
    SimSweep, // one channel at a time, 1 s each, FL->FR->C->LFE->BL->BR->SL->SR
    SimDual,  // FL + BR simultaneously (proves independent sectors)
    SimPulse, // 2 Hz bursts on SL (footstep-like)
};

class Simulator {
public:
    Simulator(SimScenario scenario, SharedMeters* meters, HANDLE quitEvent);
    ~Simulator(); // Stop()

    void Start();
    void Stop();
    void SetSilent(bool silent); // used to measure idle overlay CPU

private:
    void ThreadMain();

    SimScenario scenario_;
    SharedMeters* meters_;
    HANDLE quitEvent_; // not owned (global shutdown)
    HANDLE stopEvent_; // owned: Stop() works without a global quit
    std::thread thread_;
    std::atomic<bool> silent_{false};
};

} // namespace sr
