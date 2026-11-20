/**
 * Auto Click Mod  -  by Lucasdouge
 *
 * Auto Macro Bot for Geometry Dash:
 *   - Auto Path : the bot clicks for you through jump-style sections and learns
 *                 from every death, so the path gets better each attempt.
 *   - Record    : records your own clicks, frame by frame.
 *   - Replay    : plays the stored path back so you can watch the bot run it.
 */

#include "Bot.hpp"
#include "BotPopup.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;
using namespace acm;

// ---------------------------------------------------------------------------
// Input: inject bot clicks every physics step, record/block button events.
// ---------------------------------------------------------------------------
class $modify(ACMBaseGameLayer, GJBaseGameLayer) {
    bool isMainPlayLayer() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (this->isMainPlayLayer()) {
            Bot::get().beforeStep(PlayLayer::get());
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (this->isMainPlayLayer()) {
            auto& bot = Bot::get();
            if (bot.shouldBlockInput()) return;   // the bot is driving
            bot.onButton(down, button, isPlayer1);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// ---------------------------------------------------------------------------
// PlayLayer: level lifecycle, death learning, HUD and safe mode.
// ---------------------------------------------------------------------------
class $modify(ACMPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hud = nullptr;
        CCDrawNode* overlay = nullptr;   // hitboxes + hitbox trail
        std::string lastHudText;
        bool origTestMode = false;
    };

    // Safe Mode: a bot-assisted run is treated like a test run, so it never
    // saves progress or completions.
    void syncSafeMode() {
        auto& bot = Bot::get();
        bool safe = Mod::get()->getSettingValue<bool>("safe-mode");
        bool botRun = bot.isActive() || bot.usedThisAttempt();
        m_isTestMode = (safe && botRun) ? true : m_fields->origTestMode;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        m_fields->origTestMode = m_isTestMode;
        Bot::get().onLevelLoaded(this);

        auto winSize = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID("hud-label"_spr);
        label->setScale(.32f);
        label->setOpacity(170);
        label->setAnchorPoint({0.f, 1.f});
        label->setPosition({6.f, winSize.height - 6.f});
        label->setVisible(false);
        m_uiLayer->addChild(label, 50);
        m_fields->hud = label;

        // Hitbox overlay lives in the object layer so it scrolls with the level.
        auto overlay = CCDrawNode::create();
        overlay->setID("hitbox-overlay"_spr);
        m_objectLayer->addChild(overlay, 1000);
        m_fields->overlay = overlay;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::get().onReset(this);
        this->syncSafeMode();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        Bot::get().onDeath(this, player, object);   // learn before the state is reset
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        this->syncSafeMode();
        PlayLayer::levelComplete();
        Bot::get().onLevelComplete(this);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        Bot::get().drawOverlay(this, m_fields->overlay);

        auto hud = m_fields->hud;
        if (!hud) return;

        std::string text;
        if (Mod::get()->getSettingValue<bool>("show-hud")) {
            text = Bot::get().hudText();
        }
        if (text != m_fields->lastHudText) {
            m_fields->lastHudText = text;
            hud->setString(text.c_str());
            hud->setVisible(!text.empty());
        }
    }

    void onQuit() {
        Bot::get().onLevelExit();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Pause menu: the [Replay] button and the "Auto Bot" panel button.
// ---------------------------------------------------------------------------
class $modify(ACMPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setID("menu"_spr);
        menu->setPosition({0.f, 0.f});

        auto replaySpr = ButtonSprite::create("Replay", "goldFont.fnt", "GJ_button_01.png", .7f);
        auto replayBtn = CCMenuItemSpriteExtra::create(
            replaySpr, this, menu_selector(ACMPauseLayer::onACMReplay)
        );
        replayBtn->setID("replay-button"_spr);
        replayBtn->setPosition({72.f, 76.f});
        menu->addChild(replayBtn);

        auto botSpr = ButtonSprite::create("Auto Bot", "goldFont.fnt", "GJ_button_02.png", .7f);
        auto botBtn = CCMenuItemSpriteExtra::create(
            botSpr, this, menu_selector(ACMPauseLayer::onACMOpen)
        );
        botBtn->setID("bot-button"_spr);
        botBtn->setPosition({72.f, 36.f});
        menu->addChild(botBtn);

        this->addChild(menu, 100);
    }

    void onACMReplay(CCObject*) {
        if (!Bot::get().startReplay()) {
            Notification::create(
                "No saved path yet - turn on Auto Path or Record in Auto Bot first",
                NotificationIcon::Warning
            )->show();
            return;
        }
        this->onRestartFull(nullptr);   // restart from the start so the bot replays the whole path
    }

    void onACMOpen(CCObject*) {
        BotPopup::create(this)->show();
    }
};
