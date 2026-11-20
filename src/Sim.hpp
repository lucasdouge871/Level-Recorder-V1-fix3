#pragma once

// Hitbox-based simulator for click-to-jump gameplay (cube and robot).
//
// The Auto Path bot never guesses from fixed distances. It takes the real hitbox
// shapes of the level objects (boxes, slope triangles, pads, orbs) and simulates the
// player's hitbox:
//   roll(): what happens if the bot does NOTHING (this is how it knows a jump is needed,
//           and how it knows a jump pad or a slope will already handle an obstacle)
//   jump(): what happens if it jumps from a given spot (optionally using orbs on the way)
// then picks the middle of the widest safe stretch of start positions.
//
// This file has no Geode dependency so it can be tested on its own.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace acm {

    struct Rect {
        float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;

        bool overlaps(Rect const& o) const {
            return minX < o.maxX && maxX > o.minX && minY < o.maxY && maxY > o.minY;
        }
    };

    constexpr uint32_t kNoId = 0xFFFFFFFFu;

    // A hitbox in the level the bot has to deal with.
    //   hazard : spike, saw, animated hazard...   (kills on touch of the outer hitbox)
    //   solid  : block                            (kills only on the small inner hitbox)
    //   slope  : a block cut diagonally           (a triangle inside the rectangle)
    //   pad    : jump pad                         (launches the player; never kills)
    //   orb    : jump orb / ring                  (launches the player when you click on it)
    struct Obstacle {
        float minX = 0.f, maxX = 0.f, minY = 0.f, maxY = 0.f;
        bool hazard = false;

        bool pad = false;
        float padVel = 0.f;      // launch velocity (units per 60 Hz frame, "up" = positive)
        bool padStop = false;    // gravity / spider pad: the sim stops here, the pad takes over

        bool orb = false;
        float orbVel = 0.f;      // launch velocity when clicked; 0 = a kind of orb the bot cannot plan with
        int orbKind = 0;         // for drawing only

        // Slopes: the diagonal runs from (minX, yL) to (maxX, yR).
        // A floor slope is solid below that line, a ceiling slope is solid above it.
        bool slope = false;
        bool slopeCeil = false;
        float yL = 0.f, yR = 0.f;

        uint32_t id = kNoId;     // index in the bot's obstacle list

        float refX() const { return hazard ? (minX + maxX) * 0.5f : minX; }
        Rect rect() const { return Rect{minX, minY, maxX, maxY}; }
    };

    // ---- slope geometry --------------------------------------------------------------

    inline float slopeLineY(Obstacle const& o, float x) {
        float w = o.maxX - o.minX;
        float t = w > 0.001f ? std::clamp((x - o.minX) / w, 0.f, 1.f) : 0.f;
        return o.yL + (o.yR - o.yL) * t;
    }

    // Does the rectangle `r` overlap the solid triangle of slope `o`?
    inline bool slopeOverlaps(Obstacle const& o, Rect const& r) {
        float a = std::max(r.minX, o.minX);
        float b = std::min(r.maxX, o.maxX);
        if (a >= b) return false;
        if (!o.slopeCeil) {
            if (!(r.maxY > o.minY)) return false;
            return r.minY < std::max(slopeLineY(o, a), slopeLineY(o, b));
        }
        if (!(r.minY < o.maxY)) return false;
        return r.maxY > std::min(slopeLineY(o, a), slopeLineY(o, b));
    }

    // Highest point of a floor slope's surface within [x0, x1], or a huge negative number.
    inline float slopeTopOver(Obstacle const& o, float x0, float x1) {
        float a = std::max(x0, o.minX);
        float b = std::min(x1, o.maxX);
        if (a >= b) return -1e9f;
        return std::max(slopeLineY(o, a), slopeLineY(o, b));
    }

    // Lowest point of a ceiling slope's surface within [x0, x1], or a huge positive number.
    inline float slopeBottomOver(Obstacle const& o, float x0, float x1) {
        float a = std::max(x0, o.minX);
        float b = std::min(x1, o.maxX);
        if (a >= b) return 1e9f;
        return std::min(slopeLineY(o, a), slopeLineY(o, b));
    }

    // Height of the surface that carries a box of half-width `hw` centred at `cx`.
    // Going up a slope the front corner carries the box; once that corner is past the top
    // the box is launched. Going down (or on a flat top) the highest point under the box counts.
    inline float slopeSupport(Obstacle const& o, float cx, float hw) {
        float f = cx + hw;
        float r = cx - hw;
        if (f <= o.minX || r >= o.maxX) return -1e9f;           // not over the slope
        if (o.yR > o.yL + 0.01f) {                              // rising to the right
            if (f > o.maxX) return -1e9f;                       // front corner is past the top
            return slopeLineY(o, f);
        }
        return slopeTopOver(o, r, f);
    }

    // Overlap test that respects the real shape.
    inline bool touches(Obstacle const& o, Rect const& r) {
        return o.slope ? slopeOverlaps(o, r) : r.overlaps(o.rect());
    }

    // Mirror an obstacle top-to-bottom (used to treat an upside-down player as an upright one).
    inline void flipVertically(Obstacle& o) {
        float lo = -o.maxY;
        float hi = -o.minY;
        o.minY = lo;
        o.maxY = hi;
        if (o.slope) {
            o.yL = -o.yL;
            o.yR = -o.yR;
            o.slopeCeil = !o.slopeCeil;
        }
    }

    namespace sim {

        // Half sizes of the player's hitboxes. The outer box kills on hazards and lands on
        // blocks. Only the small inner box kills on a block, which is why you can graze
        // the corner of a block and live.
        struct Box {
            float hw = 15.f, hh = 15.f;
            float ihw = 4.5f, ihh = 4.5f;
        };

        struct Params {
            float speed = 5.1949f;        // level units per 60 Hz frame (1x speed)
            float jumpVel = 11.180032f;   // cube jump velocity, units per frame
            float gravity = 0.958199f;    // units per frame squared
            float floorY = 0.f;           // the level's ground: where you land when nothing else is below
            float startFoot = -1e9f;      // where the player's feet are at the start (default: on floorY)
        };

        struct Result {
            bool safe = false;
            bool padHit = false;          // a jump pad was touched along the way
            bool orbHit = false;          // an orb was used along the way
            float failX = 0.f;
            uint32_t hitId = kNoId;       // which hitbox ended the run
            std::vector<Rect> trail;      // hitbox positions along the run (for drawing)
        };

        struct Window {
            bool found = false;
            float lo = 0.f, hi = 0.f, center = 0.f;
        };

        // Runs the player forward from x = cx. With `startWithJump` it presses jump at the
        // start, otherwise it just rolls along. With `useOrbs` it also clicks every orb it
        // touches. The run ends safely once the player has landed and is past `clearX`,
        // or unsafely on the first deadly hitbox.
        inline Result run(float cx, Params const& pr, Box const& b, std::vector<Obstacle> const& obs,
                          float clearX, bool startWithJump, bool wantTrail, bool useOrbs = false) {
            Result res;
            constexpr float dt = 0.5f;            // half a 60 Hz frame per sub-step
            float const kSlop = b.hh - b.ihh;     // feet this far under a block top still land on it

            float fy = pr.startFoot > -1e8f ? pr.startFoot : pr.floorY;
            float vy = startWithJump ? pr.jumpVel : 0.f;
            bool airborne = startWithJump;
            float lastFeetVel = 0.f;
            std::vector<char> used(obs.size(), 0);

            for (int step = 0; step < 480; ++step) {
                float prevFoot = fy;
                if (airborne) {
                    fy += vy * dt - 0.5f * pr.gravity * dt * dt;
                    vy -= pr.gravity * dt;
                }
                cx += pr.speed * dt;

                // What is under the player: the ground, the top of a block, or a slope surface.
                float floorY = pr.floorY;
                bool slopeFloor = false;
                for (auto const& o : obs) {
                    if (o.pad || o.orb) continue;
                    if (o.slope) {
                        if (o.hazard || o.slopeCeil) continue;
                        float top = slopeSupport(o, cx, b.hw);
                        if (top > -1e8f && prevFoot >= top - kSlop && top > floorY) {
                            floorY = top;
                            slopeFloor = true;
                        }
                        continue;
                    }
                    if (o.hazard) continue;
                    if (cx + b.hw > o.minX && cx - b.hw < o.maxX && prevFoot >= o.maxY - kSlop && o.maxY > floorY) {
                        floorY = o.maxY;
                        slopeFloor = false;
                    }
                }

                if (!airborne) {
                    if (fy > floorY + 0.5f) {
                        if (slopeFloor && fy - floorY <= 6.5f) {
                            fy = floorY;                      // sticks to a downhill slope
                        } else {
                            airborne = true;                  // left the edge: keeps the speed it had
                            vy = std::clamp(lastFeetVel, -15.f, 15.f);
                        }
                    } else if (fy < floorY) {
                        fy = floorY;                          // rides up a slope / steps onto a low block
                    }
                } else if (fy <= floorY && vy <= 0.f) {
                    fy = floorY;                              // landed
                    vy = 0.f;
                    airborne = false;
                }
                lastFeetVel = (fy - prevFoot) / dt;

                Rect outer{cx - b.hw, fy, cx + b.hw, fy + 2.f * b.hh};
                Rect inner{cx - b.ihw, fy + b.hh - b.ihh, cx + b.ihw, fy + b.hh + b.ihh};

                if (wantTrail && step % 6 == 0) res.trail.push_back(outer);

                // Jump pads launch the player when touched. Orbs launch it when clicked on.
                for (size_t i = 0; i < obs.size(); ++i) {
                    auto const& o = obs[i];
                    if (used[i]) continue;
                    if (o.pad) {
                        if (!outer.overlaps(o.rect())) continue;
                        used[i] = 1;
                        res.padHit = true;
                        if (o.padStop) {                      // gravity / spider pads: not modelled, stop here
                            res.safe = true;
                            return res;
                        }
                        vy = o.padVel;
                        airborne = true;
                    } else if (o.orb && useOrbs && o.orbVel > 0.f) {
                        if (!outer.overlaps(o.rect())) continue;
                        used[i] = 1;
                        res.orbHit = true;
                        vy = o.orbVel;
                        airborne = true;
                    }
                }

                for (auto const& o : obs) {
                    if (o.pad || o.orb) continue;
                    bool dead;
                    if (o.slope) dead = o.hazard ? slopeOverlaps(o, outer) : slopeOverlaps(o, inner);
                    else dead = o.hazard ? outer.overlaps(o.rect()) : inner.overlaps(o.rect());
                    if (dead) {
                        res.safe = false;
                        res.failX = cx;
                        res.hitId = o.id;
                        return res;
                    }
                }

                if (!airborne && cx - b.hw > clearX) {
                    res.safe = true;
                    return res;
                }
            }
            return res;   // never got past the obstacle
        }

        // Press jump with the player's centre at x = cx.
        inline Result jump(float cx, Params const& pr, Box const& b, std::vector<Obstacle> const& obs,
                           float clearX, bool wantTrail, bool useOrbs = false) {
            return run(cx, pr, b, obs, clearX, true, wantTrail, useOrbs);
        }

        // Do nothing. Tells you whether a jump is needed at all.
        inline Result roll(float cx, Params const& pr, Box const& b, std::vector<Obstacle> const& obs,
                           float untilX, bool wantTrail) {
            return run(cx, pr, b, obs, untilX, false, wantTrail, false);
        }

        // Sweep start positions in [fromX, toX] and return the widest stretch where the
        // jump is safe. `center` is the best place to press jump.
        inline Window findWindow(float fromX, float toX, float stepX, Params const& pr, Box const& b,
                                 std::vector<Obstacle> const& obs, float clearX, bool useOrbs = false) {
            Window best;
            float bestWidth = -1.f;
            bool inRun = false;
            float runStart = 0.f, runEnd = 0.f;

            auto closeRun = [&]() {
                if (!inRun) return;
                float width = runEnd - runStart;
                if (width >= bestWidth) {
                    bestWidth = width;
                    best.found = true;
                    best.lo = runStart;
                    best.hi = runEnd;
                    best.center = (runStart + runEnd) * 0.5f;
                }
                inRun = false;
            };

            for (float x = fromX; x <= toX + 0.001f; x += stepX) {
                if (jump(x, pr, b, obs, clearX, false, useOrbs).safe) {
                    if (!inRun) { inRun = true; runStart = x; }
                    runEnd = x;
                } else {
                    closeRun();
                }
            }
            closeRun();
            return best;
        }
    }
}
