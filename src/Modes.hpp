#pragma once

// Hitbox-aware planner for:
//   ship, ufo, wave, swingcopter, ball, spider  (the flying / flipping gamemodes)
//   cube                                        (used for clicking orbs while in the air)
//
// Every few frames the bot looks at where the player is (position + velocity), tries many
// click patterns against the real hitboxes of the level with a small physics model of the
// current gamemode, and keeps the pattern that survives the longest with the most room to
// spare. Only the first click of that pattern is used, then it plans again (so small errors
// in the model are corrected by the next plan).
//
// All numbers are in "60 Hz frame" units with up = positive. The physics numbers below are
// starting guesses; the bot measures the real ones in-game and updates them (see Bot.cpp).
//
// This file has no Geode dependency so it can be tested on its own.

#include "Sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace acm::modes {

    enum class Kind { Ship, Ufo, Wave, Swing, Ball, Spider, Cube };

    // Tap modes react to a click event; hold modes react to the button being held down.
    inline bool isTapMode(Kind k) {
        return k != Kind::Ship && k != Kind::Wave;
    }

    // These modes flip their own gravity, so the coordinate system is never mirrored for them.
    inline bool flipsGravity(Kind k) {
        return k == Kind::Swing || k == Kind::Ball || k == Kind::Spider;
    }

    struct Phys {
        // ship
        float shipUp = 0.8f, shipDown = 0.8f;
        float shipMaxUp = 8.0f, shipMaxDown = 6.4f;
        // ufo
        float ufoImpulse = 7.2f, ufoGravity = 0.5f, ufoMaxFall = 10.0f;
        int ufoCooldown = 2;
        // wave (vertical speed = slope * horizontal speed)
        float waveSlope = 1.0f;
        // swingcopter
        float swingAccel = 0.6f, swingMax = 8.0f;
        int swingCooldown = 4;
        // ball
        float ballGravity = 0.75f, ballMax = 12.0f;
        int ballCooldown = 3;
        // spider
        float spiderGravity = 0.9f, spiderMax = 14.0f;
        int spiderCooldown = 3;
        // cube (only used for orb clicks in mid-air)
        float cubeJump = 11.180032f, cubeGravity = 0.958199f, cubeMaxFall = 15.0f;
        int cubeCooldown = 2;
    };

    struct Bounds {
        float floorY = 90.f;     // top of the level floor
        float ceilY = 390.f;     // underside of the level ceiling
    };

    struct State {
        float x = 0.f, y = 0.f;  // y = centre of the player
        float vy = 0.f;
        int grav = -1;           // +1 = pulled up, -1 = pulled down
        bool onSurf = false;
        int cd = 0;              // frames until the next click is accepted
        uint32_t used = 0;       // pads / orbs already used (bit i = i-th pad/orb in the obstacle list)
    };

    struct Config {
        int tickFrames = 3;      // frames between decisions
        int depth = 12;          // decisions looked ahead
        int beam = 24;           // paths kept per depth
    };

    inline Config configFor(Kind k) {
        switch (k) {
            case Kind::Ship:   return Config{3, 22, 24};
            case Kind::Wave:   return Config{2, 16, 24};
            case Kind::Ufo:    return Config{2, 24, 24};
            case Kind::Swing:  return Config{2, 24, 24};
            case Kind::Ball:   return Config{2, 18, 24};
            case Kind::Spider: return Config{2, 18, 24};
            case Kind::Cube:   return Config{1, 30, 16};
        }
        return Config{};
    }

    inline Rect outerRect(State const& s, sim::Box const& b) {
        return Rect{s.x - b.hw, s.y - b.hh, s.x + b.hw, s.y + b.hh};
    }

    // Does this obstacle count in the "used" bit mask? (pads and orbs the bot can plan with)
    inline bool isTriggerable(Obstacle const& o) {
        return o.pad || (o.orb && o.orbVel > 0.f);
    }

    // Moves the player out of blocks it is only grazing, and reports whether it died.
    // The level floor and ceiling never kill, they just stop the player.
    inline bool collide(State& s, sim::Box const& b, std::vector<Obstacle> const& obs, Bounds const& bd) {
        constexpr float eps = 0.5f;
        float const slop = b.hh - b.ihh;
        bool const wasSurf = s.onSurf;
        bool surf = false;

        if (s.y - b.hh <= bd.floorY + 0.01f) {
            s.y = bd.floorY + b.hh;
            if (s.vy < 0.f) s.vy = 0.f;
            if (s.grav < 0) surf = true;
        }
        if (s.y + b.hh >= bd.ceilY - 0.01f) {
            s.y = bd.ceilY - b.hh;
            if (s.vy > 0.f) s.vy = 0.f;
            if (s.grav > 0) surf = true;
        }

        Rect outer = outerRect(s, b);
        for (auto const& o : obs) {
            if (o.pad || o.orb) continue;
            if (!(outer.maxX > o.minX && outer.minX < o.maxX)) continue;   // not alongside
            Rect inner{s.x - b.ihw, s.y - b.ihh, s.x + b.ihw, s.y + b.ihh};

            if (o.slope) {
                bool dead = o.hazard ? slopeOverlaps(o, outer) : slopeOverlaps(o, inner);
                if (dead) return true;
                if (o.hazard) continue;

                if (!o.slopeCeil) {
                    float top = slopeTopOver(o, outer.minX, outer.maxX);
                    float penDown = top - outer.minY;
                    if (penDown > -eps && penDown <= slop + eps) {        // standing on / riding the slope
                        if (penDown > 0.f) s.y += penDown;
                        if (s.vy < 0.f) s.vy = 0.f;
                        if (s.grav < 0) surf = true;
                    } else if (wasSurf && s.grav < 0 && s.vy <= 0.f && -penDown > 0.f && -penDown <= 6.f) {
                        s.y -= -penDown;                                  // sticks to a downhill slope
                        s.vy = 0.f;
                        surf = true;
                    }
                } else {
                    float bottom = slopeBottomOver(o, outer.minX, outer.maxX);
                    float penUp = outer.maxY - bottom;
                    if (penUp > -eps && penUp <= slop + eps) {            // bumping a slanted ceiling
                        if (penUp > 0.f) s.y -= penUp;
                        if (s.vy > 0.f) s.vy = 0.f;
                        if (s.grav > 0) surf = true;
                    }
                }
                outer = outerRect(s, b);
                continue;
            }

            if (o.hazard) {
                if (outer.minY < o.maxY && outer.maxY > o.minY) return true;
                continue;
            }

            // solid block
            if (!(outer.minY < o.maxY + eps && outer.maxY > o.minY - eps)) continue;
            if (inner.overlaps(o.rect())) return true;

            float penDown = o.maxY - outer.minY;   // feet below the block's top
            float penUp = outer.maxY - o.minY;     // head above the block's underside
            if (penDown <= penUp && penDown <= slop + eps) {
                if (penDown > 0.f) s.y += penDown;     // stand on it
                if (s.vy < 0.f) s.vy = 0.f;
                if (s.grav < 0) surf = true;
                outer = outerRect(s, b);
            } else if (penUp <= slop + eps) {
                if (penUp > 0.f) s.y -= penUp;         // bump the underside
                if (s.vy > 0.f) s.vy = 0.f;
                if (s.grav > 0) surf = true;
                outer = outerRect(s, b);
            }
            // otherwise it is only a graze of the side, nothing happens
        }
        s.onSurf = surf;
        return false;
    }

    // How much free space surrounds the player (capped). Blocks count for less than
    // hazards because touching a block's top or underside is safe.
    inline float clearance(State const& s, sim::Box const& b, std::vector<Obstacle> const& obs, float cap) {
        Rect outer = outerRect(s, b);
        float best = cap;
        for (auto const& o : obs) {
            if (o.pad || o.orb) continue;
            float dx = std::max({0.f, o.minX - outer.maxX, outer.minX - o.maxX});
            float dy = std::max({0.f, o.minY - outer.maxY, outer.minY - o.maxY});
            float gap = std::max(dx, dy);
            best = std::min(best, o.hazard ? gap : gap * 2.f);
        }
        return best;
    }

    // Advances one 60 Hz frame. `action` = 1 means "button held" (hold modes) or
    // "click now" (tap modes). Returns true if the player died.
    inline bool stepFrame(Kind k, Phys const& ph, State& s, int action, sim::Box const& b,
                          std::vector<Obstacle> const& obs, Bounds const& bd, float speed) {
        if (s.cd > 0) --s.cd;

        switch (k) {
            case Kind::Ship: {
                float a = action ? ph.shipUp : -ph.shipDown;
                s.vy = std::clamp(s.vy + a, -ph.shipMaxDown, ph.shipMaxUp);
                break;
            }
            case Kind::Wave: {
                s.vy = (action ? 1.f : -1.f) * ph.waveSlope * speed;
                break;
            }
            case Kind::Ufo: {
                if (action && s.cd == 0) {
                    s.vy = ph.ufoImpulse;
                    s.cd = ph.ufoCooldown;
                } else {
                    s.vy = std::max(s.vy - ph.ufoGravity, -ph.ufoMaxFall);
                }
                break;
            }
            case Kind::Swing: {
                if (action && s.cd == 0) {
                    s.grav = -s.grav;
                    s.cd = ph.swingCooldown;
                }
                s.vy = std::clamp(s.vy + static_cast<float>(s.grav) * ph.swingAccel, -ph.swingMax, ph.swingMax);
                break;
            }
            case Kind::Ball: {
                if (action && s.onSurf && s.cd == 0) {
                    s.grav = -s.grav;
                    s.onSurf = false;
                    s.cd = ph.ballCooldown;
                    s.vy = 0.f;
                }
                if (s.onSurf) s.vy = 0.f;
                else s.vy = std::clamp(s.vy + static_cast<float>(s.grav) * ph.ballGravity, -ph.ballMax, ph.ballMax);
                break;
            }
            case Kind::Spider: {
                if (action && s.onSurf && s.cd == 0) {
                    // Teleport to the surface on the other side.
                    s.grav = -s.grav;
                    float target;
                    if (s.grav > 0) {
                        target = bd.ceilY - b.hh;
                        for (auto const& o : obs) {
                            if (o.hazard || o.pad || o.orb || o.slope) continue;
                            if (s.x + b.hw > o.minX && s.x - b.hw < o.maxX && o.minY >= s.y + b.hh - 0.01f) {
                                target = std::min(target, o.minY - b.hh);
                            }
                        }
                    } else {
                        target = bd.floorY + b.hh;
                        for (auto const& o : obs) {
                            if (o.hazard || o.pad || o.orb || o.slope) continue;
                            if (s.x + b.hw > o.minX && s.x - b.hw < o.maxX && o.maxY <= s.y - b.hh + 0.01f) {
                                target = std::max(target, o.maxY + b.hh);
                            }
                        }
                    }
                    s.y = target;
                    s.vy = 0.f;
                    s.onSurf = true;
                    s.cd = ph.spiderCooldown;
                } else if (s.onSurf) {
                    s.vy = 0.f;
                } else {
                    s.vy = std::clamp(s.vy + static_cast<float>(s.grav) * ph.spiderGravity, -ph.spiderMax, ph.spiderMax);
                }
                break;
            }
            case Kind::Cube: {
                // Pads launch on touch, orbs launch when clicked on, and a click on the
                // ground is a normal jump.
                bool launched = false;
                Rect outer = outerRect(s, b);
                int bit = 0;
                for (auto const& o : obs) {
                    if (!isTriggerable(o)) continue;
                    uint32_t mask = bit < 32 ? (1u << bit) : 0u;
                    ++bit;
                    if (mask == 0u || (s.used & mask) || !outer.overlaps(o.rect())) continue;
                    if (o.pad) {
                        if (o.padStop) continue;
                        s.vy = o.padVel;
                        s.used |= mask;
                        s.onSurf = false;
                        launched = true;
                    } else if (action && s.cd == 0 && !launched) {
                        s.vy = o.orbVel;
                        s.used |= mask;
                        s.onSurf = false;
                        s.cd = ph.cubeCooldown;
                        launched = true;
                    }
                }
                if (!launched && action && s.cd == 0 && s.onSurf) {
                    s.vy = ph.cubeJump;
                    s.onSurf = false;
                    s.cd = ph.cubeCooldown;
                    launched = true;
                }
                if (!launched) {
                    if (s.onSurf) s.vy = 0.f;
                    else s.vy = std::max(s.vy - ph.cubeGravity, -ph.cubeMaxFall);
                }
                break;
            }
        }

        s.x += speed;
        s.y += s.vy;
        return collide(s, b, obs, bd);
    }

    struct Decision {
        int action = 0;                  // what to do right now (1 = hold / click)
        bool aliveToHorizon = false;     // the best plan survives the whole look-ahead
        int framesAlive = 0;
        std::vector<Rect> path;          // where the plan expects the hitbox to go
    };

    // Looks ahead and picks what to do right now.
    inline Decision decide(Kind k, Phys const& ph, State const& start, int lastAction, sim::Box const& b,
                           std::vector<Obstacle> const& obs, Bounds const& bd, float speed, Config const& cfg) {
        struct Node {
            State s;
            int first = 0;
            int last = 0;
            float score = 0.f;
            std::vector<Rect> path;
        };

        bool const tap = isTapMode(k);
        std::vector<Node> beam;
        {
            Node root;
            root.s = start;
            root.last = lastAction;
            beam.push_back(std::move(root));
        }

        Decision bestDead;
        int bestDeadFrames = -1;
        float bestDeadScore = -1e9f;
        bool reachedHorizon = true;

        for (int depth = 0; depth < cfg.depth; ++depth) {
            std::vector<Node> next;
            next.reserve(beam.size() * 2);

            for (auto const& n : beam) {
                for (int a = 0; a < 2; ++a) {
                    Node c = n;
                    if (depth == 0) c.first = a;

                    bool dead = false;
                    int survived = 0;
                    for (int f = 0; f < cfg.tickFrames; ++f) {
                        int act = tap ? (f == 0 ? a : 0) : a;
                        if (stepFrame(k, ph, c.s, act, b, obs, bd, speed)) { dead = true; break; }
                        ++survived;
                    }

                    if (dead) {
                        int frames = depth * cfg.tickFrames + survived;
                        if (frames > bestDeadFrames || (frames == bestDeadFrames && c.score > bestDeadScore)) {
                            bestDeadFrames = frames;
                            bestDeadScore = c.score;
                            bestDead.action = c.first;
                            bestDead.path = c.path;
                        }
                        continue;
                    }

                    c.score += clearance(c.s, b, obs, 24.f);
                    if (tap) {
                        if (a == 1) c.score -= 0.4f;                   // do not click for no reason
                    } else if (a != c.last) {
                        c.score -= 0.3f;                               // avoid chattering
                    }
                    if (k == Kind::Ship || k == Kind::Ufo || k == Kind::Swing) {
                        c.score -= 0.03f * std::fabs(c.s.vy);          // prefer calm flying
                        // Stay away from the floor and ceiling: from the middle you can reach
                        // any gap that appears, from the floor you often cannot.
                        float room = std::min(c.s.y - b.hh - bd.floorY, bd.ceilY - (c.s.y + b.hh));
                        c.score += 0.6f * std::clamp(room, 0.f, 90.f) / 90.f;
                    }
                    c.last = a;
                    c.path.push_back(outerRect(c.s, b));
                    next.push_back(std::move(c));
                }
            }

            if (next.empty()) { reachedHorizon = false; break; }

            // Keep the best paths, but only one per "situation" (height, speed, gravity), so the
            // beam stays diverse instead of filling up with near-identical paths.
            std::sort(next.begin(), next.end(), [](Node const& l, Node const& r) { return l.score > r.score; });
            std::vector<Node> kept;
            kept.reserve(static_cast<size_t>(cfg.beam));
            std::vector<long long> seen;
            for (auto& cand : next) {
                long long key = (static_cast<long long>(std::floor(cand.s.y / 5.f)) * 4099LL)
                              + (static_cast<long long>(std::floor(cand.s.vy / 1.0f)) * 131LL)
                              + (cand.s.grav > 0 ? 7LL : 0LL) + (cand.s.onSurf ? 3LL : 0LL)
                              + (cand.s.cd > 0 ? 1LL : 0LL)
                              + static_cast<long long>(cand.s.used) * 100003LL;
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                seen.push_back(key);
                kept.push_back(std::move(cand));
                if (static_cast<int>(kept.size()) >= cfg.beam) break;
            }
            beam = std::move(kept);
        }

        Decision out;
        if (reachedHorizon && !beam.empty()) {
            out.action = beam.front().first;
            out.aliveToHorizon = true;
            out.framesAlive = cfg.depth * cfg.tickFrames;
            out.path = beam.front().path;
        } else {
            out.action = bestDead.action;
            out.aliveToHorizon = false;
            out.framesAlive = std::max(0, bestDeadFrames);
            out.path = bestDead.path;
        }
        return out;
    }
}
