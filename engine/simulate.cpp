#include "simulate.h"

namespace sr {

Simulator::Simulator(SimScenario scenario, SharedMeters* meters, HANDLE quitEvent)
    : scenario_(scenario), meters_(meters), quitEvent_(quitEvent) {}

Simulator::~Simulator() { Stop(); }

void Simulator::Start() {
    if (!thread_.joinable()) thread_ = std::thread(&Simulator::ThreadMain, this);
}

void Simulator::Stop() {
    if (thread_.joinable()) thread_.join();
}

void Simulator::SetSilent(bool silent) { silent_.store(silent); }

void Simulator::ThreadMain() {
    const DWORD tickMs = 20; // 50 Hz updates, like the analyzer on 10 ms packets
    float level[8] = {};
    uint64_t t = 0;
    while (WaitForSingleObject(quitEvent_, tickMs) == WAIT_TIMEOUT) {
        t += tickMs;
        float target[8] = {};
        if (!silent_.load()) {
            switch (scenario_) {
                case SimSweep:
                    target[(t / 1000) % 8] = 0.85f;
                    break;
                case SimDual:
                    target[0] = 0.8f; // FL
                    target[5] = 0.8f; // BR
                    break;
                case SimPulse:
                    target[6] = (t % 500) < 150 ? 0.9f : 0.0f; // SL, 2 Hz bursts
                    break;
            }
        }
        AnalysisFrame fr;
        fr.active = false;
        for (int c = 0; c < 8; ++c) {
            // fast attack, slower release - mimics the analyzer envelope
            float a = (target[c] > level[c]) ? 0.4f : 0.08f;
            level[c] += a * (target[c] - level[c]);
            fr.level[c] = level[c];
            fr.peak[c] = level[c] >= 0.5f;
            if (level[c] >= 0.05f) fr.active = true;
        }
        {
            std::lock_guard<std::mutex> lk(meters_->mu);
            meters_->frame = fr;
        }
    }
}

} // namespace sr
