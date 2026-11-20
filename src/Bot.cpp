#include "Bot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace geode::prelude;

namespace acm {

    namespace {
        // Fallback guesses, only used when the hitbox simulation finds no safe jump.
        constexpr float kSpikeLead = 64.f;
        constexpr float kBlockLead = 77.f;

        constexpr float kLookahead = 260.f;     // how far the cube looks ahead (level units at 1x)
        constexpr float kUnitSpeed = 5.1949f;   // units per 60 Hz frame at 1x speed
        constexpr float kNormalSpeed = 0.9f;    // PlayerObject::m_playerSpeed at 1x
        constexpr float kMaxEarly = 80.f;
        constexpr float kMaxLate = -40.f;
        constexpr int kJumpButton = 1;
        constexpr size_t kTrailLength = 160;
        constexpr size_t kNone = static_cast<size_t>(-1);
        constexpr uint32_t kNever = 0xFFFFFFFFu;
        constexpr float kPlanMargin = 1.5f;     // extra room the flight planner keeps around the hitbox

        std::string sanitize(std::string s) {
            for (auto& c : s) {
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            }
            return s;
        }

        ccColor4F col(float r, float g, float b, float a) {
            return ccColor4F{r, g, b, a};
        }

        CCRect toCCRect(Rect const& r, float shiftX = 0.f) {
            return CCRect(r.minX + shiftX, r.minY, r.maxX - r.minX, r.maxY - r.minY);
        }

        Rect unflipRect(Rect r, bool flip) {
            if (flip) {
                float lo = -r.maxY;
                float hi = -r.minY;
                r.minY = lo;
                r.maxY = hi;
            }
            return r;
        }

        Mode modeOf(PlayerObject* p) {
            if (p->m_isRobot) return Mode::Robot;
            if (p->m_isSpider) return Mode::Spider;
            if (p->m_isSwing) return Mode::Swing;
            if (p->m_isDart) return Mode::Wave;
            if (p->m_isBall) return Mode::Ball;
            if (p->m_isBird) return Mode::Ufo;
            if (p->m_isShip) return Mode::Ship;
            return Mode::Cube;
        }

        char const* modeName(Mode m) {
            switch (m) {
                case Mode::Cube: return "Cube";
                case Mode::Robot: return "Robot";
                case Mode::Ship: return "Ship";
                case Mode::Ufo: return "UFO";
                case Mode::Wave: return "Wave";
                case Mode::Swing: return "Swing";
                case Mode::Ball: return "Ball";
                case Mode::Spider: return "Spider";
            }
            return "?";
        }

        modes::Kind kindOf(Mode m) {
            switch (m) {
                case Mode::Ship: return modes::Kind::Ship;
                case Mode::Ufo: return modes::Kind::Ufo;
                case Mode::Wave: return modes::Kind::Wave;
                case Mode::Swing: return modes::Kind::Swing;
                case Mode::Ball: return modes::Kind::Ball;
                case Mode::Spider: return modes::Kind::Spider;
                default: return modes::Kind::Ship;
            }
        }

        // Works out the diagonal of a slope from how the object is flipped and rotated.
        // An unrotated slope has its right angle in the bottom-right corner.
        bool slopeGeometry(GameObject* obj, Obstacle& ob) {
            float q = obj->getRotation() / 90.f;
            int k = static_cast<int>(std::lround(q));
            if (std::fabs(q - static_cast<float>(k)) > 0.02f) return false;   // tilted at a strange angle
            k = ((k % 4) + 4) % 4;

            int cx = +1, cy = -1;                    // where the right angle is (+x = right, +y = up)
            if (obj->isFlipX()) cx = -cx;
            if (obj->isFlipY()) cy = -cy;
            for (int i = 0; i < k; ++i) {            // turn 90 degrees clockwise
                int nx = cy;
                int ny = -cx;
                cx = nx;
                cy = ny;
            }

            float cornerY = cy > 0 ? ob.maxY : ob.minY;
            float otherY = cy > 0 ? ob.minY : ob.maxY;
            ob.slope = true;
            ob.slopeCeil = cy > 0;                   // right angle on top: solid above the diagonal
            ob.hazard = obj->m_slopeIsHazard;
            if (cx > 0) {
                ob.yL = cornerY;
                ob.yR = otherY;
            } else {
                ob.yL = otherY;
                ob.yR = cornerY;
            }
            return true;
        }

        // Colour of an orb for the overlay.
        ccColor4F orbColor(int kind, float alpha) {
            switch (kind) {
                case 1: return ccColor4F{1.f, 0.9f, 0.1f, alpha};    // yellow
                case 2: return ccColor4F{1.f, 0.4f, 0.8f, alpha};    // pink
                case 3: return ccColor4F{1.f, 0.25f, 0.15f, alpha};  // red
                case 4: return ccColor4F{0.3f, 0.6f, 1.f, alpha};    // blue (gravity)
                case 5: return ccColor4F{0.2f, 1.f, 0.4f, alpha};    // green
                case 6: return ccColor4F{0.5f, 0.5f, 0.55f, alpha};  // black (drop)
                case 7: return ccColor4F{1.f, 0.6f, 0.1f, alpha};    // dash
                case 8: return ccColor4F{0.2f, 0.9f, 0.9f, alpha};   // gravity dash
                case 9: return ccColor4F{0.7f, 0.3f, 1.f, alpha};    // spider
                default: return ccColor4F{0.8f, 0.8f, 0.8f, alpha};
            }
        }

        // Moves a measured physics value a little towards a new sample, keeping it sane.
        void ema(float& value, float sample, float lo, float hi) {
            value = std::clamp(value + 0.15f * (sample - value), lo, hi);
        }
    }

    Bot& Bot::get() {
        static Bot instance;
        return instance;
    }

    std::string Bot::keyFor(GJGameLevel* level) {
        if (!level) return "unknown";
        int id = level->m_levelID.value();
        if (id > 0) return "level_" + std::to_string(id);
        std::string name = level->m_levelName;
        return "local_" + sanitize(name);
    }

    std::filesystem::path Bot::macroPath() const {
        return Mod::get()->getSaveDir() / "macros" / (m_macroKey + ".acm");
    }

    // ------------------------------------------------------------------
    // Level lifecycle
    // ------------------------------------------------------------------

    // Collects the real hitbox of every hazard, block and jump pad in the level.
    void Bot::buildObstacles(PlayLayer* pl) {
        m_obstacles.clear();
        m_obstacleIndex.clear();
        m_head.clear();
        m_plans.clear();
        if (!pl->m_objects) return;

        std::vector<std::pair<Obstacle, GameObject*>> raw;

        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!obj) continue;

            Obstacle ob;
            auto const& r = obj->getObjectRect();   // the rectangle the game collides with
            ob.minX = r.getMinX();
            ob.maxX = r.getMaxX();
            ob.minY = r.getMinY();
            ob.maxY = r.getMaxY();

            switch (obj->m_objectType) {
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:   // spikes, saws, animated hazards
                    ob.hazard = true;
                    break;
                case GameObjectType::Solid:            // blocks
                    break;
                case GameObjectType::YellowJumpPad:
                    ob.pad = true;
                    ob.padVel = 16.f;
                    break;
                case GameObjectType::PinkJumpPad:
                    ob.pad = true;
                    ob.padVel = 10.4f;
                    break;
                case GameObjectType::RedJumpPad:
                    ob.pad = true;
                    ob.padVel = 20.4f;
                    break;
                case GameObjectType::GravityPad:       // these change gravity: the bot stays out of it
                case GameObjectType::SpiderPad:
                    ob.pad = true;
                    ob.padStop = true;
                    break;

                // Orbs. Yellow, pink and red ones launch the player and the bot plans with them.
                // The other kinds (gravity, green, black, dash, spider...) are detected and drawn,
                // but the bot does not click on them.
                case GameObjectType::YellowJumpRing:
                    ob.orb = true; ob.orbKind = 1; ob.orbVel = 11.18f;
                    break;
                case GameObjectType::PinkJumpRing:
                    ob.orb = true; ob.orbKind = 2; ob.orbVel = 7.0f;
                    break;
                case GameObjectType::RedJumpRing:
                    ob.orb = true; ob.orbKind = 3; ob.orbVel = 13.7f;
                    break;
                case GameObjectType::GravityRing:
                    ob.orb = true; ob.orbKind = 4;
                    break;
                case GameObjectType::GreenRing:
                    ob.orb = true; ob.orbKind = 5;
                    break;
                case GameObjectType::DropRing:
                    ob.orb = true; ob.orbKind = 6;
                    break;
                case GameObjectType::DashRing:
                    ob.orb = true; ob.orbKind = 7;
                    break;
                case GameObjectType::GravityDashRing:
                    ob.orb = true; ob.orbKind = 8;
                    break;
                case GameObjectType::SpiderOrb:
                    ob.orb = true; ob.orbKind = 9;
                    break;
                case GameObjectType::CustomRing:
                case GameObjectType::TeleportOrb:
                    ob.orb = true; ob.orbKind = 10;
                    break;

                case GameObjectType::Slope:            // diagonal blocks
                    if (!slopeGeometry(obj, ob)) continue;
                    break;

                default:
                    continue;
            }

            raw.emplace_back(ob, obj);
        }

        std::sort(raw.begin(), raw.end(), [](auto const& a, auto const& b) {
            if (a.first.minX != b.first.minX) return a.first.minX < b.first.minX;
            return a.first.minY < b.first.minY;
        });

        for (auto& item : raw) {
            item.first.id = static_cast<uint32_t>(m_obstacles.size());
            m_obstacleIndex[item.second] = m_obstacles.size();
            m_obstacles.push_back(item.first);
        }

        // Neighbouring spikes are *grouped* (same head index) so they share one jump
        // decision and one learned timing. Every spike keeps its own real hitbox.
        m_head.assign(m_obstacles.size(), 0);
        size_t head = 0;
        float clusterMaxX = 0.f;
        for (size_t i = 0; i < m_obstacles.size(); ++i) {
            auto const& o = m_obstacles[i];
            bool join = false;
            if (i > 0 && o.hazard) {
                auto const& prev = m_obstacles[i - 1];
                join = prev.hazard && std::abs(prev.minY - o.minY) < 10.f && o.minX - clusterMaxX < 40.f;
            }
            if (join) {
                m_head[i] = head;
                clusterMaxX = std::max(clusterMaxX, o.maxX);
            } else {
                head = i;
                m_head[i] = i;
                clusterMaxX = o.maxX;
            }
        }
    }

    // Hitboxes between two x positions. With `flip` the level is mirrored vertically,
    // so an upside-down player can be treated as an upright one.
    std::vector<Obstacle> Bot::gather(float fromX, float toX, bool flip) const {
        std::vector<Obstacle> out;
        auto it = std::lower_bound(
            m_obstacles.begin(), m_obstacles.end(), fromX - 400.f,
            [](Obstacle const& o, float x) { return o.minX < x; }
        );
        for (; it != m_obstacles.end() && it->minX <= toX; ++it) {
            if (it->maxX < fromX) continue;
            Obstacle o = *it;
            if (flip) flipVertically(o);
            out.push_back(o);
        }
        return out;
    }

    void Bot::resetModeState() {
        m_prevValid = false;
        m_obsValid = false;
        m_modeSteps = 0;
        m_tickAccum = 0.f;
        m_flightPlanned = false;
        m_lastAction = 0;
        m_tapCd60 = 0.f;
        m_tapWatch60 = 0.f;
        m_stall = 0;
        m_vyObs = 0.f;
        m_vyPrev = 0.f;
        m_lastObsFrame = kNever;
    }

    void Bot::onLevelLoaded(PlayLayer* pl) {
        m_key = keyFor(pl->m_level);
        buildObstacles(pl);

        auto stored = m_adjustStore.find(m_key);
        if (stored != m_adjustStore.end() && stored->second.size() == m_obstacles.size()) {
            m_adjust = stored->second;
        } else {
            m_adjust.assign(m_obstacles.size(), Adjust{});
        }

        // A macro belongs to one level. Drop the old one and try the saved file.
        if (m_macroKey != m_key) {
            m_macro.clear();
            m_macroKey = m_key;
            this->loadForCurrentLevel();
        }

        m_cursor = 0;
        m_frame = 0;
        m_attempt = 0;
        m_deathAttempt = 0;
        m_used = false;
        m_botControls = false;
        m_autoHolding = false;
        m_mode = Mode::Cube;
        m_floorKnown = false;
        m_ceilKnown = false;
        this->resetModeState();
        m_trail.clear();
        m_predicted.clear();
        m_predictedIdx = kNone;
    }

    void Bot::onReset(PlayLayer* pl) {
        ++m_attempt;
        // If the bot was holding the button when the attempt ended, make sure the new
        // attempt does not start with it stuck down.
        m_releaseOnStart = autoPath || replay;
        m_usedOrbs.clear();
        m_pendingOrb = kNoId;
        m_autoHolding = false;
        m_autoReleaseFrame = 0;
        m_cooldownUntil = 0;
        m_hasLastPress = false;
        m_used = false;
        m_botControls = false;
        this->resetModeState();
        m_trail.clear();
        m_predicted.clear();
        m_predictedIdx = kNone;

        uint32_t start = pl->m_gameState.m_currentProgress;
        m_frame = start;

        if (replay) {
            auto it = std::lower_bound(
                m_macro.begin(), m_macro.end(), start,
                [](Input const& in, uint32_t f) { return in.frame < f; }
            );
            m_cursor = static_cast<size_t>(it - m_macro.begin());
        }
        else if (record || autoPath) {
            // Fresh attempt: throw away the old take. Mid-level (practice checkpoint):
            // keep everything up to the checkpoint.
            if (start == 0) {
                m_macro.clear();
            } else {
                m_macro.erase(
                    std::remove_if(m_macro.begin(), m_macro.end(), [start](Input const& in) {
                        return in.frame > start;
                    }),
                    m_macro.end()
                );
            }
        }
    }

    void Bot::onLevelExit() {
        // Never carry the bot into the next level by accident.
        autoPath = false;
        record = false;
        replay = false;
        m_used = false;
        m_botControls = false;
        m_autoHolding = false;
        m_obstacles.clear();
        m_obstacleIndex.clear();
        m_head.clear();
        m_plans.clear();
        m_trail.clear();
        m_predicted.clear();
        m_predictedIdx = kNone;
    }

    void Bot::onLevelComplete(PlayLayer*) {
        if ((record || autoPath) && !m_macro.empty()) {
            size_t count = m_macro.size();
            if (this->saveMacro()) {
                Notification::create(
                    "Auto Macro Bot: path saved (" + std::to_string(count) + " inputs)",
                    NotificationIcon::Success
                )->show();
            }
        }
        replay = false;
    }

    // ------------------------------------------------------------------
    // Per-step logic
    // ------------------------------------------------------------------

    void Bot::inject(PlayLayer* pl, bool down, int button, bool player1) {
        m_injecting = true;
        pl->handleButton(down, button, player1);
        m_injecting = false;
        m_used = true;
    }

    void Bot::sampleTrail(PlayLayer* pl) {
        if (!(autoPath || replay || record)) return;
        if (m_frame % 4 != 0) return;
        if (!Mod::get()->getSettingValue<bool>("hitbox-trail")) return;

        PlayerObject* p = pl->m_player1;
        if (!p) return;
        auto const& r = p->getObjectRect();
        m_trail.push_back(Rect{r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()});
        if (m_trail.size() > kTrailLength) m_trail.pop_front();
    }

    void Bot::beforeStep(PlayLayer* pl) {
        m_frame = pl->m_gameState.m_currentProgress;
        this->sampleTrail(pl);

        if (m_releaseOnStart) {
            m_releaseOnStart = false;
            if (autoPath || replay) this->inject(pl, false, kJumpButton, true);
        }

        if (replay) {
            m_botControls = true;
            this->replayStep(pl);
        }
        else if (autoPath) {
            this->autoStep(pl);
        }
        else {
            m_botControls = false;
        }
    }

    void Bot::replayStep(PlayLayer* pl) {
        while (m_cursor < m_macro.size() && m_macro[m_cursor].frame <= m_frame) {
            Input in = m_macro[m_cursor++];
            this->inject(pl, in.down, in.button, in.player1);
        }
    }

    void Bot::releaseHeld(PlayLayer* pl) {
        if (m_autoHolding) {
            this->inject(pl, false, kJumpButton, true);
            m_autoHolding = false;
        }
    }

    // Number of physics steps that make up `frames60` frames at 60 Hz.
    uint32_t Bot::stepsFor(float frames60) const {
        float steps = frames60 / std::max(0.05f, m_dt60);
        return std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(steps)));
    }

    // ------------------------------------------------------------------
    // Measuring what the game actually does
    // ------------------------------------------------------------------

    // Called once per physics step. Works out how long a step is and how fast the player
    // is really moving up/down, from how far the player actually moved.
    void Bot::observe(PlayerObject* p) {
        float px = p->getPositionX();
        float py = p->getPositionY();
        float speed = kUnitSpeed * std::max(0.3f, p->m_playerSpeed) / kNormalSpeed;

        m_vyPrev = m_vyObs;
        m_obsValid = false;
        if (m_prevValid) {
            float dt60 = (px - m_prevX) / speed;
            if (dt60 > 0.02f && dt60 < 2.f) {
                m_dt60 = dt60;
                m_vyObs = (py - m_prevY) / dt60;
                m_obsValid = true;
            }
        }
        m_prevX = px;
        m_prevY = py;
        m_prevValid = true;
    }

    // Learns the real physics of the current gamemode from how the player actually moved,
    // so the planner's predictions match the game.
    void Bot::calibrate(PlayerObject* p, Mode mode, bool flip, bool holdPrev) {
        if (!m_obsValid || m_modeSteps < 4) return;
        if (p->m_isOnGround) return;   // touching a surface: velocities are not free motion

        modes::Phys const def;
        float speed = kUnitSpeed * std::max(0.3f, p->m_playerSpeed) / kNormalSpeed;
        float a = (m_vyObs - m_vyPrev) / m_dt60;       // world acceleration, units per frame^2
        float sign = flip ? -1.f : 1.f;
        float aN = a * sign;                            // same, with "up" = positive
        float vN = m_vyObs * sign;
        float gdir = p->m_isUpsideDown ? 1.f : -1.f;    // world direction gravity pulls
        float ag = a * gdir;                            // acceleration along gravity (> 0 = falling)

        switch (mode) {
            case Mode::Ship:
                if (holdPrev) {
                    if (aN > 0.05f && aN < 3.f && vN < def.shipMaxUp * 0.9f)
                        ema(m_phys.shipUp, aN, def.shipUp * 0.4f, def.shipUp * 2.5f);
                } else {
                    if (-aN > 0.05f && -aN < 3.f && vN > -def.shipMaxDown * 0.9f)
                        ema(m_phys.shipDown, -aN, def.shipDown * 0.4f, def.shipDown * 2.5f);
                }
                break;
            case Mode::Ufo:
                if (m_tapWatch60 <= 0.f && -aN > 0.05f && -aN < 3.f && -vN < def.ufoMaxFall * 0.9f)
                    ema(m_phys.ufoGravity, -aN, def.ufoGravity * 0.4f, def.ufoGravity * 2.5f);
                break;
            case Mode::Swing:
                if (ag > 0.05f && ag < 3.f && std::fabs(m_vyObs) < def.swingMax * 0.9f)
                    ema(m_phys.swingAccel, ag, def.swingAccel * 0.4f, def.swingAccel * 2.5f);
                break;
            case Mode::Ball:
                if (ag > 0.05f && ag < 3.f && std::fabs(m_vyObs) < def.ballMax * 0.9f)
                    ema(m_phys.ballGravity, ag, def.ballGravity * 0.4f, def.ballGravity * 2.5f);
                break;
            case Mode::Spider:
                if (ag > 0.05f && ag < 3.f && std::fabs(m_vyObs) < def.spiderMax * 0.9f)
                    ema(m_phys.spiderGravity, ag, def.spiderGravity * 0.4f, def.spiderGravity * 2.5f);
                break;
            case Mode::Wave: {
                float slope = std::fabs(m_vyObs) / speed;
                if (slope > 0.3f && slope < 3.f) ema(m_phys.waveSlope, slope, 0.5f, 3.f);
                break;
            }
            default:
                break;
        }

        // How strong is a ufo tap? Watch the peak speed shortly after we tapped.
        if (mode == Mode::Ufo && m_tapWatch60 > 0.f) {
            m_tapPeak = std::max(m_tapPeak, vN);
            m_tapWatch60 -= m_dt60;
            if (m_tapWatch60 <= 0.f && m_tapPeak > 2.f) {
                ema(m_phys.ufoImpulse, m_tapPeak + 0.5f * m_phys.ufoGravity,
                    def.ufoImpulse * 0.4f, def.ufoImpulse * 2.5f);
            }
        }
    }

    // Finds the floor and ceiling of the flying section by noticing where the player
    // touches them.
    void Bot::learnBounds(PlayerObject* p, Mode mode, float halfHeight) {
        float py = p->getPositionY();

        if (p->m_isOnGround) {
            if (!p->m_isUpsideDown) {
                float feet = py - halfHeight;
                m_floorY = m_floorKnown ? std::min(m_floorY, feet) : feet;
                m_floorKnown = true;
            } else {
                float top = py + halfHeight;
                m_ceilY = m_ceilKnown ? std::max(m_ceilY, top) : top;
                m_ceilKnown = true;
            }
            m_stall = 0;
            return;
        }

        // Holding up but not moving up at all: we are pressed against the ceiling.
        bool pushingUp = m_autoHolding && !p->m_isUpsideDown && (mode == Mode::Ship || mode == Mode::Wave);
        if (pushingUp && m_obsValid && std::fabs(m_vyObs) < 0.05f) {
            if (++m_stall >= 6) {
                float top = py + halfHeight;
                m_ceilY = m_ceilKnown ? std::max(m_ceilY, top) : top;
                m_ceilKnown = true;
            }
        } else {
            m_stall = 0;
        }
    }

    modes::Bounds Bot::boundsFor(float py, float halfHeight, bool flip) const {
        float floorW = m_floorKnown ? m_floorY : std::min(90.f, py - halfHeight);
        floorW = std::min(floorW, py - halfHeight);
        float ceilW = m_ceilKnown ? m_ceilY : floorW + 300.f;
        ceilW = std::max(ceilW, py + halfHeight + 30.f);   // never trap the player under a guessed ceiling

        modes::Bounds b;
        if (flip) {
            b.floorY = -ceilW;
            b.ceilY = -floorW;
        } else {
            b.floorY = floorW;
            b.ceilY = ceilW;
        }
        return b;
    }

    // ------------------------------------------------------------------
    // Cube / robot: plan jumps from the real hitboxes
    // ------------------------------------------------------------------

    // The level's ground as the simulation should see it. Learned from where the player has
    // actually stood; when upside down the "ground" is the ceiling, mirrored.
    float Bot::groundFor(bool flip, float feet) const {
        float ground;
        if (!flip) ground = m_floorKnown ? m_floorY : std::min(90.f, feet);
        else ground = m_ceilKnown ? -m_ceilY : feet;
        return std::min(ground, feet);
    }

    // Works out where to press jump for obstacle `idx` by simulating the player's real
    // hitbox against the real hitboxes around it. Cached until the player's size,
    // speed, standing height or orientation changes.
    Plan const& Bot::planFor(size_t idx, sim::Box const& box, float feet, float ground, float speed, bool flip) {
        uint32_t sig = (static_cast<uint32_t>(std::lround(feet)) * 73856093u)
                     ^ (static_cast<uint32_t>(std::lround(ground)) * 2654435761u)
                     ^ (static_cast<uint32_t>(std::lround(speed * 100.f)) * 19349663u)
                     ^ (static_cast<uint32_t>(std::lround(box.hw * 2.f)) * 83492791u)
                     ^ (flip ? 0x9E3779B1u : 0u);

        auto cached = m_plans.find(idx);
        if (cached != m_plans.end() && cached->second.sig == sig) return cached->second;

        Obstacle target = m_obstacles[idx];
        if (flip) flipVertically(target);

        // The jump has to clear the whole group of spikes this one belongs to.
        float clearX = target.maxX;
        if (target.hazard) {
            for (size_t j = idx + 1; j < m_obstacles.size() && m_head[j] == m_head[idx]; ++j) {
                clearX = std::max(clearX, m_obstacles[j].maxX);
            }
        }

        float speedScale = speed / kUnitSpeed;
        float ref = target.refX();
        float from = ref - 200.f * speedScale;   // earliest start we consider
        float to = ref + 10.f;                   // latest start we consider

        // Hitboxes the jump could touch: everything between the start and a bit past the target.
        std::vector<Obstacle> nearby = this->gather(from - 60.f, clearX + 260.f * speedScale, flip);

        sim::Params params;
        params.speed = speed;
        params.startFoot = feet;
        params.floorY = ground;

        Plan plan;
        plan.sig = sig;
        auto window = sim::findWindow(from, to, 3.f, params, box, nearby, clearX, false);
        bool withOrbs = false;
        if (!window.found) {
            // Not possible with plain jumps: try again clicking the orbs on the way.
            bool anyOrb = false;
            for (auto const& o : nearby) anyOrb = anyOrb || (o.orb && o.orbVel > 0.f);
            if (anyOrb) {
                window = sim::findWindow(from, to, 3.f, params, box, nearby, clearX, true);
                withOrbs = window.found;
            }
        }
        if (window.found) {
            plan.found = true;
            plan.usesOrbs = withOrbs;
            plan.pressX = window.center;
            plan.lo = window.lo;
            plan.hi = window.hi;
            plan.arc = sim::jump(window.center, params, box, nearby, clearX, true, withOrbs).trail;
            for (auto& r : plan.arc) r = unflipRect(r, flip);
        } else {
            plan.pressX = ref - (target.hazard ? kSpikeLead : kBlockLead) * speedScale;
        }

        return m_plans[idx] = std::move(plan);
    }

    void Bot::autoCube(PlayLayer* pl, PlayerObject* p, bool robot) {
        // In the air the only decision left is whether to click an orb.
        if (!p->m_isOnGround) {
            this->cubeAir(pl, p, robot);
            return;
        }
        if (m_frame < m_cooldownUntil || m_obstacles.empty()) return;

        bool flip = p->m_isUpsideDown;
        m_lastFlip = flip;

        // The player's real hitbox size drives the simulation.
        auto const& hit = p->getObjectRect();
        sim::Box box;
        box.hw = std::clamp(hit.size.width * 0.5f, 8.f, 20.f);
        box.hh = std::clamp(hit.size.height * 0.5f, 8.f, 20.f);
        box.ihw = box.hw * 0.3f;
        box.ihh = box.hh * 0.3f;

        float px = p->getPositionX();
        float py = p->getPositionY();
        float feet = flip ? -(py + box.hh) : (py - box.hh);
        float speed = kUnitSpeed * std::max(0.3f, p->m_playerSpeed) / kNormalSpeed;
        float speedScale = speed / kUnitSpeed;

        // Step 1: what happens if the bot does nothing? Jump pads are part of this, so a
        // pad that already carries the player over an obstacle means: no click.
        float untilX = px + kLookahead * speedScale;
        std::vector<Obstacle> nearby = this->gather(px - 60.f, untilX + 80.f, flip);

        float ground = this->groundFor(flip, feet);
        sim::Params params;
        params.speed = speed;
        params.startFoot = feet;
        params.floorY = ground;
        auto rolled = sim::roll(px, params, box, nearby, untilX, true);

        if (rolled.padHit) {
            m_predicted.clear();
            for (auto const& r : rolled.trail) m_predicted.push_back(unflipRect(r, flip));
            m_predictedFound = true;
            m_predictedIdx = kNone;
            m_predictedShift = 0.f;
            return;   // a pad is next to the obstacle: let the pad do the work, do not click
        }

        if (rolled.safe || rolled.hitId == kNoId || rolled.hitId >= m_obstacles.size()) {
            m_predicted.clear();
            m_predictedIdx = kNone;
            return;   // nothing in the way
        }

        // Step 2: something would kill us. Plan the jump from the real hitboxes.
        size_t idx = rolled.hitId;
        Plan const& plan = this->planFor(idx, box, feet, ground, speed, flip);
        size_t group = idx < m_head.size() ? m_head[idx] : idx;
        float adjust = group < m_adjust.size() ? m_adjust[group].value : 0.f;

        // What the hitbox trail shows: the path we expect to take.
        if (m_predictedIdx != idx) {
            m_predicted = plan.arc;
            m_predictedIdx = idx;
            m_predictedFound = plan.found;
        }
        m_predictedShift = -adjust;

        if (px >= plan.pressX - adjust) {
            uint32_t hold = this->stepsFor(robot ? 16.f : 2.f);   // robot jumps higher the longer you hold
            this->inject(pl, true, kJumpButton, true);
            m_autoHolding = true;
            m_autoReleaseFrame = m_frame + hold;
            m_cooldownUntil = m_frame + hold + this->stepsFor(1.f);
            m_hasLastPress = true;
            m_lastPressObstacle = idx;
            m_lastPressX = px;
        }
    }

    // The cube / robot is in the air: is there an orb to click? A short look-ahead search
    // with the real hitboxes decides whether clicking it now is the way to survive.
    void Bot::cubeAir(PlayLayer* pl, PlayerObject* p, bool robot) {
        if (m_autoHolding || m_frame < m_cooldownUntil || m_obstacles.empty()) return;

        bool flip = p->m_isUpsideDown;
        float px = p->getPositionX();
        float py = p->getPositionY();
        std::vector<Obstacle> nearby = this->gather(px - 60.f, px + 330.f, flip);

        bool anyOrb = false;
        for (auto const& o : nearby) {
            if (o.orb && o.orbVel > 0.f && !m_usedOrbs.count(o.id) && o.maxX >= px - 20.f && o.minX <= px + 200.f) {
                anyOrb = true;
                break;
            }
        }
        if (!anyOrb) return;

        auto const& hit = p->getObjectRect();
        sim::Box box;
        box.hw = std::clamp(hit.size.width * 0.5f, 8.f, 20.f);
        box.hh = std::clamp(hit.size.height * 0.5f, 8.f, 20.f);
        box.ihw = box.hw * 0.3f;
        box.ihh = box.hh * 0.3f;

        float speed = kUnitSpeed * std::max(0.3f, p->m_playerSpeed) / kNormalSpeed;
        float feet = flip ? -(py + box.hh) : (py - box.hh);

        modes::State s;
        s.x = px;
        s.y = flip ? -py : py;
        s.vy = flip ? -m_vyObs : m_vyObs;
        s.grav = -1;
        s.onSurf = false;
        s.cd = 0;

        // Pads and orbs that already did their job must not fire again in the prediction.
        int bit = 0;
        for (auto const& o : nearby) {
            if (!modes::isTriggerable(o)) continue;
            if (bit < 32) {
                bool done = (o.orb && m_usedOrbs.count(o.id)) || (o.pad && o.minX < px + box.hw);
                if (done) s.used |= (1u << bit);
            }
            ++bit;
        }

        modes::Bounds bounds;
        bounds.floorY = this->groundFor(flip, feet);
        bounds.ceilY = 100000.f;

        modes::Config cfg = modes::configFor(modes::Kind::Cube);
        modes::Decision d = modes::decide(modes::Kind::Cube, m_phys, s, 0, box, nearby, bounds, speed, cfg);

        m_predicted.clear();
        for (auto const& r : d.path) m_predicted.push_back(unflipRect(r, flip));
        m_predictedFound = d.aliveToHorizon;
        m_predictedIdx = kNone;
        m_predictedShift = 0.f;

        if (d.action != 1) return;

        // Click only if an orb is really under us right now.
        Rect now{px - box.hw, (flip ? -py : py) - box.hh, px + box.hw, (flip ? -py : py) + box.hh};
        for (auto const& o : nearby) {
            if (!(o.orb && o.orbVel > 0.f) || m_usedOrbs.count(o.id) || !now.overlaps(o.rect())) continue;
            uint32_t hold = this->stepsFor(robot ? 16.f : 2.f);
            this->inject(pl, true, kJumpButton, true);
            m_autoHolding = true;
            m_autoReleaseFrame = m_frame + hold;
            m_cooldownUntil = m_frame + hold + this->stepsFor(1.f);
            m_usedOrbs.insert(o.id);
            m_pendingOrb = o.id;          // checked next step: did the orb really fire?
            m_pendingOrbFrame = m_frame;
            break;
        }
    }

    // ------------------------------------------------------------------
    // Ship / ufo / wave / swing / ball / spider
    // ------------------------------------------------------------------

    void Bot::autoFlight(PlayLayer* pl, PlayerObject* p, Mode mode) {
        modes::Kind kind = kindOf(mode);
        modes::Config cfg = modes::configFor(kind);
        bool tap = modes::isTapMode(kind);
        bool flip = !modes::flipsGravity(kind) && p->m_isUpsideDown;
        m_lastFlip = flip;

        // Plan every few frames; in between, whatever was decided stays in effect.
        if (m_tickAccum < static_cast<float>(cfg.tickFrames) && m_flightPlanned) return;
        m_tickAccum = 0.f;
        m_flightPlanned = true;

        auto const& hit = p->getObjectRect();
        float hw0 = std::clamp(hit.size.width * 0.5f, 4.f, 24.f);
        float hh0 = std::clamp(hit.size.height * 0.5f, 4.f, 24.f);
        sim::Box box;
        box.hw = hw0 + kPlanMargin;
        box.hh = hh0 + kPlanMargin;
        box.ihw = hw0 * 0.3f;
        box.ihh = hh0 * 0.3f;

        float px = p->getPositionX();
        float py = p->getPositionY();
        float speed = kUnitSpeed * std::max(0.3f, p->m_playerSpeed) / kNormalSpeed;

        modes::State s;
        s.x = px;
        s.y = flip ? -py : py;
        s.vy = flip ? -m_vyObs : m_vyObs;
        s.grav = modes::flipsGravity(kind) ? (p->m_isUpsideDown ? 1 : -1) : -1;
        s.onSurf = p->m_isOnGround;
        s.cd = static_cast<int>(std::ceil(std::max(0.f, m_tapCd60)));

        float horizon = static_cast<float>(cfg.depth * cfg.tickFrames) * speed;
        std::vector<Obstacle> nearby = this->gather(px - 40.f, px + horizon + 60.f, flip);
        modes::Bounds bounds = this->boundsFor(py, hh0, flip);

        modes::Decision d = modes::decide(kind, m_phys, s, m_lastAction, box, nearby, bounds, speed, cfg);
        m_lastAction = d.action;

        // What the hitbox trail shows: the path the plan expects.
        m_predicted.clear();
        for (auto const& r : d.path) m_predicted.push_back(unflipRect(r, flip));
        m_predictedFound = d.aliveToHorizon;
        m_predictedIdx = kNone;
        m_predictedShift = 0.f;

        if (!tap) {
            // ship / wave: hold the button while the plan wants to go up
            bool want = d.action == 1;
            if (want && !m_autoHolding) {
                this->inject(pl, true, kJumpButton, true);
                m_autoHolding = true;
                m_autoReleaseFrame = kNever;
            } else if (!want && m_autoHolding) {
                this->inject(pl, false, kJumpButton, true);
                m_autoHolding = false;
            }
        } else if (d.action == 1 && !m_autoHolding && m_tapCd60 <= 0.f) {
            // ufo / swing / ball / spider: one click
            this->inject(pl, true, kJumpButton, true);
            m_autoHolding = true;
            m_autoReleaseFrame = m_frame + this->stepsFor(2.f);
            switch (mode) {
                case Mode::Ufo: m_tapCd60 = static_cast<float>(m_phys.ufoCooldown); break;
                case Mode::Swing: m_tapCd60 = static_cast<float>(m_phys.swingCooldown); break;
                case Mode::Ball: m_tapCd60 = static_cast<float>(m_phys.ballCooldown); break;
                default: m_tapCd60 = static_cast<float>(m_phys.spiderCooldown); break;
            }
            if (mode == Mode::Ufo) {
                m_tapWatch60 = 6.f;
                m_tapPeak = -1e9f;
            }
        }
    }

    // ------------------------------------------------------------------
    // Auto Path: decide which gamemode logic is in charge
    // ------------------------------------------------------------------

    void Bot::autoStep(PlayLayer* pl) {
        m_botControls = false;

        PlayerObject* p = pl->m_player1;
        if (!p || p->m_isDead || pl->m_isPlatformer || pl->m_gameState.m_isDualMode || p->m_isSideways) {
            // Not supported here (dual mode, platformer, sideways gravity): you stay in control.
            this->releaseHeld(pl);
            m_prevValid = false;
            return;
        }

        Mode mode = modeOf(p);
        if (mode != m_mode) {
            this->releaseHeld(pl);
            m_mode = mode;
            m_floorKnown = false;
            m_ceilKnown = false;
            this->resetModeState();
        }
        m_botControls = true;

        bool holdPrev = m_autoHolding;
        bool newStep = (m_frame != m_lastObsFrame);
        if (newStep) {
            m_lastObsFrame = m_frame;
            this->observe(p);
            ++m_modeSteps;
            m_tickAccum += m_dt60;
            m_tapCd60 = std::max(0.f, m_tapCd60 - m_dt60);

            float hh = std::clamp(p->getObjectRect().size.height * 0.5f, 4.f, 24.f);
            this->learnBounds(p, mode, hh);
            if (mode != Mode::Cube && mode != Mode::Robot) {
                this->calibrate(p, mode, !modes::flipsGravity(kindOf(mode)) && p->m_isUpsideDown, holdPrev);
            }

            // Did the orb we just clicked really launch us? If not, allow another try.
            if (m_pendingOrb != kNoId && m_frame > m_pendingOrbFrame) {
                float gain = (p->m_isUpsideDown ? -1.f : 1.f) * (m_vyObs - m_vyPrev);
                if (!m_obsValid || gain < 1.5f) m_usedOrbs.erase(m_pendingOrb);
                m_pendingOrb = kNoId;
            }
        }

        // Scheduled release of a click (cube / robot / tap modes).
        if (m_autoHolding && m_frame >= m_autoReleaseFrame) {
            this->inject(pl, false, kJumpButton, true);
            m_autoHolding = false;
        }

        if (mode == Mode::Cube || mode == Mode::Robot) {
            this->autoCube(pl, p, mode == Mode::Robot);
        } else {
            this->autoFlight(pl, p, mode);
        }
    }

    void Bot::onButton(bool down, int button, bool player1) {
        if (replay) return;
        if (!(record || autoPath)) return;
        Input in;
        in.frame = m_frame;
        in.down = down;
        in.button = button;
        in.player1 = player1;
        m_macro.push_back(in);
    }

    bool Bot::shouldBlockInput() const {
        if (m_injecting) return false;
        return replay || (autoPath && m_botControls);
    }

    // The bot learns from every cube / robot death: if it died on an obstacle it nudges
    // the jump timing for that obstacle and tries again on the next attempt.
    void Bot::onDeath(PlayLayer* pl, PlayerObject* player, GameObject* by) {
        if (!autoPath || !m_botControls) return;
        if (m_mode != Mode::Cube && m_mode != Mode::Robot) return;
        if (!player || player != pl->m_player1) return;
        if (m_deathAttempt == m_attempt) return;   // already handled this death
        m_deathAttempt = m_attempt;

        float px = player->getPositionX();

        size_t idx = kNone;
        auto found = m_obstacleIndex.find(by);
        if (by && found != m_obstacleIndex.end()) {
            idx = found->second;
        }
        else if (m_hasLastPress && px - m_lastPressX < 240.f) {
            idx = m_lastPressObstacle;   // died shortly after a jump
        }
        else {
            for (size_t i = 0; i < m_obstacles.size(); ++i) {   // died before reacting
                if (m_obstacles[i].refX() >= px - 12.f && !m_obstacles[i].pad) { idx = i; break; }
            }
        }
        if (idx != kNone && idx < m_head.size()) idx = m_head[idx];
        if (idx == kNone || idx >= m_adjust.size()) return;

        bool rising = m_lastFlip ? (m_vyObs < 0.f) : (m_vyObs > 0.f);
        int dir;
        if (player->m_isOnGround) dir = +1;   // hit it on the ground: too late
        else if (rising) dir = +1;            // still rising: too late
        else dir = -1;                        // already falling: too early

        Adjust& a = m_adjust[idx];
        if (a.lastDir != 0 && a.lastDir != dir) a.step = std::max(1.f, a.step * 0.5f);
        a.value = std::clamp(a.value + static_cast<float>(dir) * a.step, kMaxLate, kMaxEarly);
        a.lastDir = dir;

        m_adjustStore[m_key] = m_adjust;
    }

    // ------------------------------------------------------------------
    // Hitbox overlay: level hitboxes, the player's hitbox, and the hitbox trail
    // ------------------------------------------------------------------

    void Bot::drawOverlay(PlayLayer* pl, CCDrawNode* node) {
        if (!node) return;
        node->clear();
        if (!(autoPath || replay || record)) return;

        bool showBoxes = Mod::get()->getSettingValue<bool>("show-hitboxes");
        bool showTrail = Mod::get()->getSettingValue<bool>("hitbox-trail");
        PlayerObject* p = pl->m_player1;
        if (!p || !(showBoxes || showTrail)) return;

        float px = p->getPositionX();

        if (showBoxes) {
            // Level hitboxes near the screen: red = hazards, blue = blocks, green = slopes, yellow = jump pads, coloured circles = orbs.
            auto it = std::lower_bound(
                m_obstacles.begin(), m_obstacles.end(), px - 800.f,
                [](Obstacle const& o, float x) { return o.minX < x; }
            );
            for (; it != m_obstacles.end() && it->minX <= px + 420.f; ++it) {
                if (it->maxX < px - 220.f) continue;
                CCRect r(it->minX, it->minY, it->maxX - it->minX, it->maxY - it->minY);
                if (it->pad) {
                    node->drawRect(r, col(1.f, 0.9f, 0.1f, 0.2f), 0.6f, col(1.f, 0.9f, 0.2f, 0.95f));
                } else if (it->orb) {
                    // circle in the colour of the orb; dimmer if the bot does not click this kind
                    float radius = std::max(it->maxX - it->minX, it->maxY - it->minY) * 0.5f;
                    float alpha = it->orbVel > 0.f ? 0.45f : 0.2f;
                    CCPoint centre((it->minX + it->maxX) * 0.5f, (it->minY + it->maxY) * 0.5f);
                    node->drawDot(centre, radius, orbColor(it->orbKind, alpha));
                } else if (it->slope) {
                    // the real triangle, not its bounding box
                    bool rising = it->yR > it->yL;
                    float cornerX = it->slopeCeil ? (rising ? it->minX : it->maxX) : (rising ? it->maxX : it->minX);
                    float cornerY = it->slopeCeil ? it->maxY : it->minY;
                    CCPoint tri[3] = {CCPoint(it->minX, it->yL), CCPoint(it->maxX, it->yR), CCPoint(cornerX, cornerY)};
                    if (it->hazard) {
                        node->drawPolygon(tri, 3, col(1.f, 0.1f, 0.1f, 0.18f), 0.6f, col(1.f, 0.25f, 0.25f, 0.95f));
                    } else {
                        node->drawPolygon(tri, 3, col(0.1f, 0.9f, 0.6f, 0.15f), 0.6f, col(0.2f, 0.95f, 0.65f, 0.95f));
                    }
                } else if (it->hazard) {
                    node->drawRect(r, col(1.f, 0.1f, 0.1f, 0.18f), 0.6f, col(1.f, 0.25f, 0.25f, 0.95f));
                } else {
                    node->drawRect(r, col(0.2f, 0.5f, 1.f, 0.12f), 0.6f, col(0.35f, 0.65f, 1.f, 0.9f));
                }
            }

            // The player: outer hitbox (yellow) and the small inner box (cyan).
            auto const& hit = p->getObjectRect();
            node->drawRect(hit, col(1.f, 0.95f, 0.f, 0.12f), 0.7f, col(1.f, 0.95f, 0.2f, 0.95f));
            float ihw = hit.size.width * 0.15f;
            float ihh = hit.size.height * 0.15f;
            node->drawRect(
                CCRect(hit.getMidX() - ihw, hit.getMidY() - ihh, ihw * 2.f, ihh * 2.f),
                col(0.f, 1.f, 1.f, 0.25f), 0.5f, col(0.f, 1.f, 1.f, 0.9f)
            );
        }

        if (showTrail) {
            // Where the hitbox has been: fades out with age.
            size_t n = m_trail.size();
            for (size_t i = 0; i < n; ++i) {
                float t = static_cast<float>(i + 1) / static_cast<float>(n);
                node->drawRect(
                    toCCRect(m_trail[i]), col(0.f, 0.f, 0.f, 0.f), 0.5f,
                    col(0.4f, 0.9f, 1.f, 0.08f + 0.5f * t)
                );
            }

            // Where the bot expects it to go: green = safe plan found, orange = best guess.
            if (!m_predicted.empty()) {
                ccColor4F c = m_predictedFound ? col(0.2f, 1.f, 0.3f, 0.85f) : col(1.f, 0.6f, 0.1f, 0.85f);
                for (auto const& r : m_predicted) {
                    node->drawRect(toCCRect(r, m_predictedShift), col(0.f, 0.f, 0.f, 0.f), 0.5f, c);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // HUD text
    // ------------------------------------------------------------------

    std::string Bot::hudText() const {
        if (replay) {
            return fmt::format("Auto Macro Bot | Replay {}/{}", m_cursor, m_macro.size());
        }
        if (autoPath) {
            if (!m_botControls) return "Auto Macro Bot | Auto Path (you drive this section)";
            return fmt::format("Auto Macro Bot | Auto Path | {} | {} inputs", modeName(m_mode), m_macro.size());
        }
        if (record) {
            return fmt::format("Auto Macro Bot | Recording | {} inputs", m_macro.size());
        }
        return "";
    }

    // ------------------------------------------------------------------
    // Macro management + files
    // ------------------------------------------------------------------

    bool Bot::startReplay() {
        if (m_macro.empty()) this->loadForCurrentLevel();
        if (m_macro.empty()) return false;
        replay = true;
        autoPath = false;
        record = false;
        return true;
    }

    void Bot::stopAll() {
        autoPath = false;
        record = false;
        replay = false;
    }

    bool Bot::saveMacro() {
        if (m_macro.empty() || m_macroKey.empty()) return false;

        std::error_code ec;
        std::filesystem::create_directories(Mod::get()->getSaveDir() / "macros", ec);

        std::ofstream out(this->macroPath(), std::ios::trunc);
        if (!out) return false;

        out << "ACM1\n";
        for (auto const& in : m_macro) {
            out << in.frame << ' ' << (in.down ? 1 : 0) << ' '
                << in.button << ' ' << (in.player1 ? 1 : 0) << '\n';
        }
        return out.good();
    }

    bool Bot::loadForCurrentLevel() {
        m_macroKey = m_key;
        std::ifstream in(this->macroPath());
        if (!in) return false;

        std::string magic;
        std::getline(in, magic);
        if (magic != "ACM1") return false;

        std::vector<Input> loaded;
        uint32_t frame = 0;
        int down = 0, button = 0, p1 = 0;
        while (in >> frame >> down >> button >> p1) {
            Input item;
            item.frame = frame;
            item.down = down != 0;
            item.button = button;
            item.player1 = p1 != 0;
            loaded.push_back(item);
        }
        if (loaded.empty()) return false;

        std::stable_sort(loaded.begin(), loaded.end(), [](Input const& a, Input const& b) {
            return a.frame < b.frame;
        });
        m_macro = std::move(loaded);
        m_cursor = 0;
        return true;
    }

    void Bot::clearMacro(bool deleteFile) {
        m_macro.clear();
        m_cursor = 0;
        if (deleteFile && !m_macroKey.empty()) {
            std::error_code ec;
            std::filesystem::remove(this->macroPath(), ec);
        }
    }

}
