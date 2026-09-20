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
        int ownerCh = -1;   // wave channel this arrow follows (-1 = unknown)
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
        struct Peak { float angle, energy; int ch; bool snap; };
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

                // The run itself always votes, and the centre of the run is
                // exact: a lone FL is -30, a lone SL is -90.
                double sx = 0, sy = 0;
                float emax = 0;
                int ech = -1;
                for (int j = a;; j = (j + 1) % 7) {
                    const float lv = levels[kArrowRingCh[j]];
                    const float w = std::sqrt(lv);
                    const float rad = kArrowRingAng[j] * kPiF / 180.0f;
                    sx += w * std::sin(rad);
                    sy += w * std::cos(rad);
                    if (lv > emax) { emax = lv; ech = kArrowRingCh[j]; }
                    if (j == b) break;
                }
                const float runAngle =
                    ArrowNormAngle(static_cast<float>(std::atan2(sx, sy)) * 180.0f / kPiF);
                // One ring neighbour may vote, but only when it is clearly part
                // of the same source (60% of the run peak) and the pair stays
                // within 90 deg. Without this limit the ring wraps a lone FL
                // towards FR and drags the arrow about 20 deg off its channel.
                const int cands[2] = { (a + 6) % 7, (b + 1) % 7 };
                for (int n = 0; n < 2; ++n) {
                    const int j = cands[n];
                    if (j == a || j == b) continue; // run of 6 or 7
                    const float lv = levels[kArrowRingCh[j]];
                    if (lv < minLevel || lv < 0.6f * emax) continue;
                    const float na = kArrowRingAng[j];
                    if (std::fabs(ArrowAngDist(na, runAngle)) > 90.0f) continue;
                    const float w = std::sqrt(lv);
                    const float rad = na * kPiF / 180.0f;
                    sx += w * std::sin(rad);
                    sy += w * std::cos(rad);
                    if (lv > emax) { emax = lv; ech = kArrowRingCh[j]; }
                }
                if (nPeaks < 4 && (sx * sx + sy * sy) > 1e-6) {
                    peaks[nPeaks].angle = ArrowNormAngle(
                        static_cast<float>(std::atan2(sx, sy)) * 180.0f / kPiF);
                    peaks[nPeaks].energy = emax;
                    peaks[nPeaks].ch = ech;
                    peaks[nPeaks].snap = false;
                    ++nPeaks;
                }
                i = (b + 1) % 7;
            } while (i != start && nPeaks < 4);
        }

        // Frontal merge. Only the FL/FR pair is fused, and the fused pair is
        // pushed dead ahead at once instead of gliding there: a 50 ms glide
        // leaves the arrow visibly left or right for about 150 ms, which reads
        // as a wrong direction. Two genuinely separate sources are at least
        // 60 deg apart (FL and SL), so they survive.
        if (frontMerge && nPeaks > 1) {
            int fi = -1, fj = -1;
            for (int a = 0; a < nPeaks && fi < 0; ++a) {
                for (int b = a + 1; b < nPeaks; ++b) {
                    const bool fl = (peaks[a].ch == 0 && peaks[b].ch == 1) ||
                                    (peaks[a].ch == 1 && peaks[b].ch == 0);
                    if (fl) { fi = a; fj = b; break; }
                }
            }
            if (fi >= 0) {
                // Both front channels carry energy. When they are close in
                // level (within about 5 dB) the pair reads as one source dead
                // ahead, and the merged arrow snaps there: a 50 ms glide would
                // leave it visibly left or right, which reads as a wrong
                // direction. When one channel dominates, the pair is treated as
                // one source on its way across the front, and the arrow follows
                // the energy-weighted angle instead of sitting at dead ahead
                // while the sound is already to the side. The angle it reaches
                // is the angle the cluster tracker would draw with the merge
                // off, so the takeover rule below can still snap it across.
                const float ea = peaks[fi].energy;
                const float eb = peaks[fj].energy;
                const float lo = ea < eb ? ea : eb;
                const float hi = ea < eb ? eb : ea;
                if (hi > 0.0f && lo > 0.5f * hi) {
                    peaks[fi].angle = 0.0f; // one source dead ahead
                } else {
                    const double ra = peaks[fi].angle * kPiF / 180.0;
                    const double rb = peaks[fj].angle * kPiF / 180.0;
                    const double sx = ea * std::sin(ra) + eb * std::sin(rb);
                    const double sy = ea * std::cos(ra) + eb * std::cos(rb);
                    peaks[fi].angle = ArrowNormAngle(
                        static_cast<float>(std::atan2(sx, sy)) * 180.0f / kPiF);
                }
                peaks[fi].snap = true; // reaches it in one frame, no glide
                if (peaks[fj].energy > peaks[fi].energy) peaks[fi].energy = peaks[fj].energy;
                // The merged arrow carries the louder channel, so the takeover
                // test above compares against the right owner.
                peaks[fi].ch = (eb > ea) ? peaks[fj].ch : peaks[fi].ch;
                peaks[fj] = peaks[--nPeaks];
            }
        }

        // 3) match peaks to existing arrows (< 60 deg), else spawn
        float smoothA = 1.0f - std::exp(-dt / 0.05f); // 50 ms glide
        for (auto& ar : arrows_) ar.matched = false;
        for (int p = 0; p < nPeaks; ++p) {
            int best = -1;
            int strongArrow = -1; // arrow this peak may take over by strength
            float bestD = 60.0f;
            for (size_t k = 0; k < arrows_.size(); ++k) {
                if (arrows_[k].matched) continue;
                // A different channel only matches within 40 deg, so the FL
                // arrow never becomes the FR arrow (they are 60 deg apart).
                // Exception: when the old owner channel is quiet, the arrow is
                // free to be taken over by any channel. Without this, rotating
                // the view leaves the old arrow behind next to the new one.
                const int oc = arrows_[k].ownerCh;
                const bool ownerQuiet = (oc < 0) || (oc < 8 && levels[oc] < minLevel);
                // Second exception: the new channel is clearly stronger than the
                // old owner. A source that steps from one wave channel to the
                // next (FL to FR, FL to SL) leaves the old level decaying on its
                // envelope release; without this rule the arrow sat on the old
                // bearing for about 300 ms, which reads as "the pointer does not
                // follow the sound". The 1.5x test keeps a genuine second source
                // (a quieter one, 30 dB down) from stealing the arrow.
                const bool stronger = oc >= 0 && oc < 8 && peaks[p].ch >= 0 &&
                                      oc != peaks[p].ch &&
                                      peaks[p].energy > 1.5f * levels[oc];
                if (stronger) strongArrow = static_cast<int>(k);
                if (!ownerQuiet && !stronger && oc >= 0 && peaks[p].ch >= 0 &&
                    oc != peaks[p].ch && bestD > 40.0f)
                    bestD = 40.0f;
                float d = std::fabs(ArrowAngDist(peaks[p].angle, arrows_[k].angle));
                if (d < bestD) { bestD = d; best = static_cast<int>(k); }
            }
            if (best >= 0) {
                Arrow& ar = arrows_[best];
                ar.matched = true;
                ar.ownerCh = peaks[p].ch;
                ar.trail1 = ar.trail0;
                ar.trail0 = ar.angle;
                if (peaks[p].snap || best == strongArrow) {
                    // A fused front pair, or a takeover by a clearly stronger
                    // channel, jumps to the new bearing at once. A glide here
                    // would leave the arrow visibly on the old direction.
                    ar.angle = peaks[p].angle;
                } else {
                    ar.angle = ArrowNormAngle(ar.angle +
                                              ArrowAngDist(peaks[p].angle, ar.angle) * smoothA);
                }
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
                ar.ownerCh = peaks[p].ch;
                arrows_.push_back(ar);
            }
        }
        // unmatched arrows hold then fade out; all arrows age (scale-in pop)
        FadeArrows(dt, arrowFadeMs, true);
    }

    const std::vector<Arrow>& Arrows() const { return arrows_; }

    // True silence: drop every arrow now, without the fade tail. The analyzer
    // reports silence when its gate is shut, so this is a definite "no sound",
    // not a quiet passage.
    void Clear() { arrows_.clear(); }

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
    // Arrow lifetime after a match stops. A linear ramp, not an exponential:
    // an exponential tail needs about 4 tau to fall under the cut-off, so the
    // old code kept a faint arrow for roughly 600 ms even at the shortest fade
    // setting, and the field report was "the mark stays after the sound stops".
    // Total lifetime = hold + fade, and fade is the slider value.
    void FadeArrows(float dt, int arrowFadeMs, bool unmatchedOnly) {
        const float holdMs = 60.0f;
        float fadeMs = static_cast<float>(arrowFadeMs > 0 ? arrowFadeMs : 500);
        if (fadeMs < 100.0f) fadeMs = 100.0f;
        for (size_t k = 0; k < arrows_.size();) {
            Arrow& ar = arrows_[k];
            ar.age += dt;
            if (unmatchedOnly && ar.matched) { ar.missMs = 0.0f; ++k; continue; }
            ar.missMs += dt * 1000.0f;
            if (ar.missMs > holdMs) {
                // Linear ramp to zero over fadeMs.
                ar.strength -= dt * 1000.0f / fadeMs;
                if (ar.strength <= 0.0f) {
                    arrows_.erase(arrows_.begin() + k);
                    continue;
                }
            }
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
