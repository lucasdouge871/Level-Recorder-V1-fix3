#pragma once

#include "Modes.hpp"
#include "Sim.hpp"

#include <Geode/Geode.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acm {

    // One recorded button event. `frame` is the physics step it happened on.
    struct Input {
        uint32_t frame = 0;
        bool down = false;
        int button = 1;
        bool player1 = true;
    };

    // Learned timing correction for one obstacle (cube / robot).
    // value > 0 means "jump earlier", value < 0 means "jump later" (in level units).
    struct Adjust {
        float value = 0.f;
        float step = 6.f;
        int lastDir = 0;
    };

    // Where the bot decided to jump for one obstacle, worked out from real hitboxes.
    struct Plan {
        bool found = false;          // false = no safe jump exists, bot falls back to a guess
        float pressX = 0.f;          // player x at which to press jump
        float lo = 0.f, hi = 0.f;    // the safe stretch of start positions
        bool usesOrbs = false;       // the jump relies on clicking an orb in mid-air
        uint32_t sig = 0;            // what the plan was computed for
        std::vector<Rect> arc;       // predicted hitbox path of the jump (for the trail)
    };

    enum class Mode { Cube, Robot, Ship, Ufo, Wave, Swing, Ball, Spider };

    class Bot {
    public:
        static Bot& get();

        // ---- Modes (toggled from the Auto Macro Bot popup / pause menu) ----
        bool autoPath = false;   // bot plays by itself and writes the path as it goes
        bool record = false;     // record the player's own clicks
        bool replay = false;     // play the stored macro back

        // ---- Hook entry points ----
        void onLevelLoaded(PlayLayer* pl);
        void onReset(PlayLayer* pl);
        void onLevelExit();
        void onLevelComplete(PlayLayer* pl);
        void beforeStep(PlayLayer* pl);
        void onButton(bool down, int button, bool player1);
        void onDeath(PlayLayer* pl, PlayerObject* player, GameObject* by);
        bool shouldBlockInput() const;

        // ---- Hitbox overlay (called every rendered frame) ----
        void drawOverlay(PlayLayer* pl, cocos2d::CCDrawNode* node);

        // ---- Queries ----
        bool isActive() const { return autoPath || replay; }
        bool usedThisAttempt() const { return m_used; }
        bool hasMacro() const { return !m_macro.empty(); }
        size_t macroSize() const { return m_macro.size(); }
        std::string const& levelKey() const { return m_key; }
        std::string hudText() const;

        // ---- Macro management ----
        bool startReplay();          // arms replay mode (loads from disk if needed)
        void stopAll();
        bool saveMacro();
        bool loadForCurrentLevel();
        void clearMacro(bool deleteFile);

    private:
        static std::string keyFor(GJGameLevel* level);
        void buildObstacles(PlayLayer* pl);
        std::vector<Obstacle> gather(float fromX, float toX, bool flip) const;
        void replayStep(PlayLayer* pl);
        void autoStep(PlayLayer* pl);
        void autoCube(PlayLayer* pl, PlayerObject* p, bool robot);
        void cubeAir(PlayLayer* pl, PlayerObject* p, bool robot);
        void autoFlight(PlayLayer* pl, PlayerObject* p, Mode mode);
        void releaseHeld(PlayLayer* pl);
        void inject(PlayLayer* pl, bool down, int button, bool player1);
        void sampleTrail(PlayLayer* pl);
        void observe(PlayerObject* p);
        void calibrate(PlayerObject* p, Mode mode, bool flip, bool holdPrev);
        void learnBounds(PlayerObject* p, Mode mode, float halfHeight);
        void resetModeState();
        modes::Bounds boundsFor(float py, float halfHeight, bool flip) const;
        uint32_t stepsFor(float frames60) const;
        float groundFor(bool flip, float feet) const;
        Plan const& planFor(size_t idx, sim::Box const& box, float feet, float ground, float speed, bool flip);
        std::filesystem::path macroPath() const;

        // level
        std::string m_key;
        std::string m_macroKey;

        // timeline
        uint32_t m_frame = 0;
        uint32_t m_attempt = 0;

        // macro
        std::vector<Input> m_macro;
        size_t m_cursor = 0;

        // input state
        bool m_injecting = false;
        bool m_used = false;          // bot touched the controls this attempt
        bool m_botControls = false;   // bot is currently in charge of the player

        // hitboxes of the level + the jump plans built from them
        std::vector<Obstacle> m_obstacles;   // sorted by minX
        std::unordered_map<GameObject*, size_t> m_obstacleIndex;
        std::vector<size_t> m_head;          // first obstacle of the spike group each one belongs to
        std::unordered_map<size_t, Plan> m_plans;

        // timing learned from deaths (cube / robot)
        std::vector<Adjust> m_adjust;
        std::unordered_map<std::string, std::vector<Adjust>> m_adjustStore;

        // auto path: button state
        bool m_autoHolding = false;
        uint32_t m_autoReleaseFrame = 0;
        uint32_t m_cooldownUntil = 0;
        bool m_releaseOnStart = false;   // let go of the button at the start of a new attempt
        bool m_hasLastPress = false;
        size_t m_lastPressObstacle = 0;
        float m_lastPressX = 0.f;
        uint32_t m_deathAttempt = 0;
        bool m_lastFlip = false;

        // auto path: gamemode + measured physics
        Mode m_mode = Mode::Cube;
        modes::Phys m_phys;                  // starts as a guess, refined from what the game does
        uint32_t m_modeSteps = 0;
        uint32_t m_lastObsFrame = 0xFFFFFFFFu;
        bool m_prevValid = false;
        bool m_obsValid = false;
        float m_prevX = 0.f, m_prevY = 0.f;
        float m_dt60 = 0.25f;                // length of one physics step in 60 Hz frames
        float m_vyObs = 0.f, m_vyPrev = 0.f; // measured vertical velocity (units per 60 Hz frame)
        float m_tickAccum = 0.f;
        bool m_flightPlanned = false;
        int m_lastAction = 0;
        float m_tapCd60 = 0.f;
        float m_tapWatch60 = 0.f;            // measuring the strength of a ufo tap
        float m_tapPeak = 0.f;
        int m_stall = 0;
        bool m_floorKnown = false, m_ceilKnown = false;
        float m_floorY = 90.f, m_ceilY = 390.f;

        // orbs the bot has already clicked this attempt
        std::unordered_set<uint32_t> m_usedOrbs;
        uint32_t m_pendingOrb = 0xFFFFFFFFu;
        uint32_t m_pendingOrbFrame = 0;

        // hitbox trail
        std::deque<Rect> m_trail;                 // where the player's hitbox has been
        std::vector<Rect> m_predicted;            // where the bot expects it to go
        size_t m_predictedIdx = static_cast<size_t>(-1);
        float m_predictedShift = 0.f;
        bool m_predictedFound = false;
    };

}
