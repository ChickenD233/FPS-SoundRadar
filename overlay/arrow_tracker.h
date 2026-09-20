// arrow_tracker.h - direction estimation for the overlay: peak clustering,
// frontal merge, and cross-frame arrow tracking. Pure C++, no Windows APIs, so
// the host test harness can exercise it (see engine/tests/sr_probe.cpp).
#pragma once

#include <cmath>
#include <vector>

namespace sr {

// Angle of each wave channel: FL FR C LFE BL BR SL SR. LFE has no angle.
inline constexpr float kArrowAngleDeg[8] = { -30.f, 30.f, 0.f, -999.f, -135.f, 135.f, -90.f, 90.f };

// Ring order sorted by angle (circular adjacency): BL SL FL C FR SR BR
inline constexpr int kArrowRingCh[7] = { 4, 6, 0, 2, 1, 7, 5 };
inline constexpr float kArrowRingAng[7] = { -135.f, -90.f, -30.f, 0.f, 30.f, 90.f, 135.f };

constexpr float kPiF = 3.14159265358979323846f;

// shortest-path angular difference a-b, in -180..180
inline float ArrowAngDist(float a, float b) {
    float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
    return d;
}

inline float ArrowNormAngle(float a) {
    a = std::fmod(a + 540.0f, 360.0f);
    return a - 180.0f;
}

// directional channel whose angle is closest to `angleDeg` (LFE excluded)
inline int ArrowNearestChannel(float angleDeg) {
    int best = -1;
    float bestD = 1e9f;
    for (int c = 0; c < 8; ++c) {
        if (c == 3) continue; // LFE has no angle
        float d = std::fabs(ArrowAngDist(angleDeg, kArrowAngleDeg[c]));
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

class ArrowTracker {
public:
    struct Arrow {
        float angle = 0;    // smoothed display angle, degrees
        float strength = 0; // smoothed energy
        float pulse = 0;    // onset ripple 0..1 (0 = none), ~150 ms
        float age = 0;      // seconds since spawn (scale-in pop)
        float trail0 = 0, trail1 = 0; // recent older angles (comet trail)
        float missMs = 0;   // ms since last matched (fade hold + decay)
        bool matched = false;
    };

    // minLevel: display threshold; dt: seconds since last call.
    // frontMerge: fuse the FL/FR pair into one dead-ahead arrow.
    // arrowFadeMs: unmatched arrows hold 90 ms, then fade out over ~arrowFadeMs.
    void Update(const float levels[8], float minLevel, float dt, bool frontMerge,
                int arrowFadeMs) {
        // 1) candidate peaks: level ~>= both ring neighbors (15% tolerance so a
        //    diffuse quiet step spread over adjacent channels still spawns)
        bool cand[7];
        for (int i = 0; i < 7; ++i) {
            float l = levels[kArrowRingCh[i]];
            float lp = levels[kArrowRingCh[(i + 6) % 7]];
            float ln = levels[kArrowRingCh[(i + 1) % 7]];
            cand[i] = l > minLevel && l >= lp * 0.85f && l >= ln * 0.85f;
        }
        // 2) maximal circular runs of adjacent candidates -> one peak each,
        //    centroid = energy-weighted circular mean over run +/- 1 neighbor
        struct Peak { float angle, energy; };
        Peak peaks[4];
        int nPeaks = 0;
        int start = -1;
        for (int i = 0; i < 7; ++i)
            if (!cand[i] && cand[(i + 1) % 7]) { start = (i + 1) % 7; break; }
        if (start >= 0) {
            int i = start;
            do {
                if (!cand[i]) { i = (i + 1) % 7; continue; }
                int a = i, b = i;
                while (cand[(b + 1) % 7] && (b + 1) % 7 != start) b = (b + 1) % 7;
                if (a == b && cand[(b + 1) % 7]) break; // all 7: no direction
                double sx = 0, sy = 0;
                float emax = 0;
                for (int j = (a + 6) % 7;; j = (j + 1) % 7) {
                    float lv = levels[kArrowRingCh[j]];
                    // run +/- 1 neighbor: noise below minLevel gets no vote
                    bool edge = (j == (a + 6) % 7 || j == (b + 1) % 7);
                    float w = (edge && lv < minLevel) ? 0.0f : std::sqrt(lv);
                    float rad = kArrowRingAng[j] * kPiF / 180.0f;
                    sx += w * std::sin(rad);
                    sy += w * std::cos(rad);
                    if (lv > emax) emax = lv;
                    if (j == (b + 1) % 7) break;
                }
                if (nPeaks < 4 && (sx * sx + sy * sy) > 1e-6) {
                    peaks[nPeaks].angle = ArrowNormAngle(
                        static_cast<float>(std::atan2(sx, sy)) * 180.0f / kPiF);
                    peaks[nPeaks].energy = emax;
                    ++nPeaks;
                }
                i = (b + 1) % 7;
            } while (i != start && nPeaks < 4);
        }

        // frontal merge: fuse the FL/FR pair (and any peaks closer than 20 deg)
        // with an energy-weighted circular mean. Two genuinely separate sources
        // sit at least ±30 deg apart, so they survive.
        if (frontMerge && nPeaks > 1) {
            bool merged = true;
            while (merged && nPeaks > 1) {
                merged = false;
                for (int a = 0; a < nPeaks && !merged; ++a) {
                    for (int b = a + 1; b < nPeaks; ++b) {
                        float d = std::fabs(ArrowAngDist(peaks[a].angle, peaks[b].angle));
                        bool flfr = (std::fabs(peaks[a].angle + 30.0f) < 1.0f &&
                                     std::fabs(peaks[b].angle - 30.0f) < 1.0f) ||
                                    (std::fabs(peaks[a].angle - 30.0f) < 1.0f &&
                                     std::fabs(peaks[b].angle + 30.0f) < 1.0f);
                        if (!flfr && d >= 20.0f) continue;
                        double ra = peaks[a].angle * kPiF / 180.0;
                        double rb = peaks[b].angle * kPiF / 180.0;
                        double sx = peaks[a].energy * std::sin(ra) +
                                    peaks[b].energy * std::sin(rb);
                        double sy = peaks[a].energy * std::cos(ra) +
                                    peaks[b].energy * std::cos(rb);
                        peaks[a].angle = ArrowNormAngle(
                            static_cast<float>(std::atan2(sx, sy)) * 180.0f / kPiF);
                        if (peaks[b].energy > peaks[a].energy)
                            peaks[a].energy = peaks[b].energy;
                        peaks[b] = peaks[--nPeaks];
                        merged = true;
                        break;
                    }
                }
            }
            // The merged front pair must sit dead ahead, not at a small offset:
            // FL louder than FR would otherwise leave the arrow visibly left.
            if (levels[0] > minLevel && levels[1] > minLevel) {
                for (int p = 0; p < nPeaks; ++p) {
                    if (std::fabs(peaks[p].angle) < 45.0f) peaks[p].angle = 0.0f;
                }
            }
        }

        // 3) match peaks to existing arrows (< 60 deg), else spawn
        float smoothA = 1.0f - std::exp(-dt / 0.05f); // 50 ms glide
        for (auto& ar : arrows_) ar.matched = false;
        for (int p = 0; p < nPeaks; ++p) {
            int best = -1;
            float bestD = 60.0f;
            for (size_t k = 0; k < arrows_.size(); ++k) {
                if (arrows_[k].matched) continue;
                float d = std::fabs(ArrowAngDist(peaks[p].angle, arrows_[k].angle));
                if (d < bestD) { bestD = d; best = static_cast<int>(k); }
            }
            if (best >= 0) {
                Arrow& ar = arrows_[best];
                ar.matched = true;
                ar.trail1 = ar.trail0;
                ar.trail0 = ar.angle;
                ar.angle = ArrowNormAngle(ar.angle + ArrowAngDist(peaks[p].angle, ar.angle) * smoothA);
                ar.strength += (peaks[p].energy - ar.strength) * smoothA;
                if (ar.pulse > 0.0f) {
                    ar.pulse += dt / 0.15f; // ~150 ms onset ripple
                    if (ar.pulse >= 1.0f) ar.pulse = 0.0f;
                }
            } else {
                Arrow ar;
                ar.angle = ar.trail0 = ar.trail1 = peaks[p].angle;
                ar.strength = peaks[p].energy;
                ar.pulse = 0.001f; // onset ripple
                ar.matched = true;
                arrows_.push_back(ar);
            }
        }
        // unmatched arrows hold then fade out; all arrows age (scale-in pop)
        FadeArrows(dt, arrowFadeMs, true);
    }

    const std::vector<Arrow>& Arrows() const { return arrows_; }

    // Stereo input (srcChannels == 2): one indicator sweeping the front.
    // pan = (R-L)/(L+R) -> angle = pan * 90 deg, smoothed like the clusters.
    void UpdateStereo(float lvlL, float lvlR, float dt, int arrowFadeMs) {
        float smoothA = 1.0f - std::exp(-dt / 0.05f); // 50 ms glide
        float sum = lvlL + lvlR;
        if (sum < 0.05f) { // fade out
            FadeArrows(dt, arrowFadeMs, false);
            return;
        }
        float pan = (lvlR - lvlL) / sum; // -1..+1
        float target = pan * 90.0f;
        float energy = sum * 0.5f;
        if (arrows_.empty()) {
            Arrow a;
            a.angle = a.trail0 = a.trail1 = target;
            a.strength = energy;
            a.pulse = 0.001f;
            a.matched = true;
            arrows_.push_back(a);
            return;
        }
        if (arrows_.size() > 1) arrows_.resize(1); // stereo = one indicator
        Arrow& ar = arrows_[0];
        ar.matched = true;
        ar.missMs = 0.0f;
        ar.trail1 = ar.trail0;
        ar.trail0 = ar.angle;
        ar.age += dt;
        ar.angle += ShortestDelta(target, ar.angle) * smoothA;
        ar.strength += (energy - ar.strength) * smoothA;
        if (ar.pulse > 0.0f) {
            ar.pulse += dt / 0.15f;
            if (ar.pulse >= 1.0f) ar.pulse = 0.0f;
        }
    }

    static float ShortestDelta(float a, float b) { // a-b in -180..180
        return std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
    }

private:
    // fading arrows hold full strength for 90 ms, then decay exponentially
    // with tau = arrowFadeMs/3000 seconds (gone ~arrowFadeMs after the hold)
    void FadeArrows(float dt, int arrowFadeMs, bool unmatchedOnly) {
        float tau = (arrowFadeMs > 0 ? arrowFadeMs : 500) / 3000.0f;
        for (size_t k = 0; k < arrows_.size();) {
            Arrow& ar = arrows_[k];
            ar.age += dt;
            if (unmatchedOnly && ar.matched) { ar.missMs = 0.0f; ++k; continue; }
            ar.missMs += dt * 1000.0f;
            if (ar.missMs > 90.0f)
                ar.strength *= std::exp(-dt / tau);
            if (ar.strength < 0.02f) {
                arrows_.erase(arrows_.begin() + k);
                continue;
            }
            ++k;
        }
    }

    std::vector<Arrow> arrows_;
};

} // namespace sr
