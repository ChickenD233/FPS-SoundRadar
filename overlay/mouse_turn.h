// mouse_turn.h - experimental view compensation.
//
// Idea: the game audio you hear is anchored to the world, but the overlay draws
// it on a screen-fixed ring. When you turn, the view rotates but a delayed
// sound (footstep, gunshot) still points at its world bearing, so the arrow is
// stale. Rotating the drawn bearing by the view angle you turned cancels that:
// the arrow then points where the sound is relative to the view you have now.
//
// The chain is short and exact when the numbers are right:
//   horizontal mouse counts -> view yaw degrees -> subtract from the bearing
// Windows sends one raw count per mouse step, the game scales it by its own
// sensitivity, and cm/360 (the distance for one full turn) ties the two
// together. With CM360 and DPI known, one count is
//     DEG_PER_COUNT = 360 / (CM360 * DPI / 2.54)
// The site the report points to (xidaohai.github.io) uses that same cm/360
// quantity as the common currency between games, so the conversion is the
// standard one: measure cm/360 once, then use it here.
//
// The overlay can also self-calibrate: turn the mouse until the ring matches
// the world, then set the value by hand. The manual value wins when it is set,
// because a measured value beats a computed one when the game does not use raw
// input or has its own acceleration.
#pragma once

#include <cmath>

namespace sr {

// Degrees of view yaw for one mouse count. Zero when the inputs are unusable.
inline double MouseDegPerCount(double cm360, double dpi) {
    if (cm360 <= 1.0 || dpi <= 1.0) return 0.0;
    const double countsPerTurn = (cm360 / 2.54) * dpi; // counts per 360 deg
    if (countsPerTurn < 1.0) return 0.0;
    return 360.0 / countsPerTurn;
}

// Rotate the level array so each source is drawn at bearing - yawDeg.
//
// The overlay estimates direction from an 8-channel wave layout, so a bearing
// can only land on the ring directions: 0, +-30, +-90, +-135 deg. A source is
// therefore moved to the ring direction nearest to (bearing - yaw). That gives
// steps of 30 or 60 deg, which is the resolution the audio layout itself has.
// A finer interpolation was tried and reverted: splitting one source across two
// ring directions makes each of them a weaker local peak, so the peak finder
// reports the wrong angle instead of the average of the two.
//
// yawDeg is the view angle the player turned: a clockwise turn (yaw > 0) moves
// a fixed world bearing counter-clockwise on screen, so the drawn angle drops.
inline void RotateLevels(const float levels[8], float out[8], double yawDeg,
                         const int ringCh[7], const float ringAng[7]) {
    for (int c = 0; c < 8; ++c) out[c] = 0.0f;
    double yaw = std::fmod(yawDeg, 360.0);
    if (yaw < 0.0) yaw += 360.0;
    if (yaw < 1e-9) {
        for (int c = 0; c < 8; ++c) out[c] = levels[c];
        return;
    }

    for (int i = 0; i < 7; ++i) {
        const float lv = levels[ringCh[i]];
        if (lv == 0.0f) continue;
        const double target = ringAng[i] - yaw;
        int best = 0;
        double bestD = 1e9;
        for (int j = 0; j < 7; ++j) {
            const double d = std::fabs(std::fmod(target - ringAng[j] + 540.0, 360.0) - 180.0);
            if (d < bestD) { bestD = d; best = j; }
        }
        out[ringCh[best]] += lv;
    }
}

} // namespace sr
