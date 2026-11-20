#pragma once

#include "Bot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>

using namespace geode::prelude;
using acm::Bot;

// The "Auto Macro Bot" panel you open from the pause menu.
class BotPopup : public geode::Popup {
protected:
    PauseLayer* m_pause = nullptr;
    CCLabelBMFont* m_levelLabel = nullptr;
    CCLabelBMFont* m_macroLabel = nullptr;
    CCLabelBMFont* m_modeLabel = nullptr;
    CCMenuItemToggler* m_autoToggle = nullptr;
    CCMenuItemToggler* m_recordToggle = nullptr;

    bool init(PauseLayer* pause) {
        if (!Popup::init(340.f, 320.f)) return false;
        m_pause = pause;
        this->setTitle("Auto Macro Bot");

        // --- status text ---
        m_levelLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_levelLabel->setScale(.55f);
        m_mainLayer->addChildAtPosition(m_levelLabel, Anchor::Top, {0.f, -48.f});

        m_macroLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_macroLabel->setScale(.55f);
        m_mainLayer->addChildAtPosition(m_macroLabel, Anchor::Top, {0.f, -64.f});

        // --- toggles ---
        m_autoToggle = this->addToggle("Auto Path (bot clicks for you)", Bot::get().autoPath,
                                       menu_selector(BotPopup::onToggleAuto), 64.f);
        m_recordToggle = this->addToggle("Record my own clicks", Bot::get().record,
                                         menu_selector(BotPopup::onToggleRecord), 32.f);
        this->addToggle("Show hitboxes", Mod::get()->getSettingValue<bool>("show-hitboxes"),
                        menu_selector(BotPopup::onToggleHitboxes), 0.f);
        this->addToggle("Hitbox trail", Mod::get()->getSettingValue<bool>("hitbox-trail"),
                        menu_selector(BotPopup::onToggleTrail), -32.f);

        m_modeLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_modeLabel->setScale(.5f);
        m_mainLayer->addChildAtPosition(m_modeLabel, Anchor::Center, {0.f, -62.f});

        // --- buttons ---
        this->addButton("Replay", menu_selector(BotPopup::onReplay), {-60.f, 74.f}, .8f);
        this->addButton("Stop", menu_selector(BotPopup::onStop), {60.f, 74.f}, .8f);
        this->addButton("Save", menu_selector(BotPopup::onSave), {-100.f, 38.f}, .6f);
        this->addButton("Load", menu_selector(BotPopup::onLoad), {0.f, 38.f}, .6f);
        this->addButton("Clear", menu_selector(BotPopup::onClear), {100.f, 38.f}, .6f);

        auto credit = CCLabelBMFont::create("Auto Click Mod by Lucasdouge", "chatFont.fnt");
        credit->setScale(.45f);
        credit->setOpacity(140);
        m_mainLayer->addChildAtPosition(credit, Anchor::Bottom, {0.f, 12.f});

        this->refresh();
        return true;
    }

    CCMenuItemToggler* addToggle(char const* text, bool on, SEL_MenuHandler cb, float y) {
        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, cb, .7f);
        toggler->toggle(on);
        m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, {-140.f, y});

        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(.42f);
        label->setAnchorPoint({0.f, .5f});
        m_mainLayer->addChildAtPosition(label, Anchor::Center, {-118.f, y});
        return toggler;
    }

    void addButton(char const* text, SEL_MenuHandler cb, CCPoint offset, float scale) {
        auto spr = ButtonSprite::create(text, "goldFont.fnt", "GJ_button_01.png", scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Bottom, offset);
    }

    void refresh() {
        auto& bot = Bot::get();
        m_levelLabel->setString(("Level: " + bot.levelKey()).c_str());
        m_macroLabel->setString(
            bot.hasMacro()
                ? fmt::format("Saved path: {} inputs", bot.macroSize()).c_str()
                : "Saved path: none yet"
        );

        char const* mode = "Idle";
        if (bot.replay) mode = "Replaying";
        else if (bot.autoPath) mode = "Auto Path ON (restart the attempt to start fresh)";
        else if (bot.record) mode = "Recording (restart the attempt to start fresh)";
        m_modeLabel->setString(fmt::format("Mode: {}", mode).c_str());
    }

    void onToggleAuto(CCObject* sender) {
        bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        auto& bot = Bot::get();
        bot.autoPath = on;
        if (on) bot.replay = false;
        this->refresh();
    }

    void onToggleRecord(CCObject* sender) {
        bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        auto& bot = Bot::get();
        bot.record = on;
        if (on) bot.replay = false;
        this->refresh();
    }

    void onToggleHitboxes(CCObject* sender) {
        bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSettingValue<bool>("show-hitboxes", on);
    }

    void onToggleTrail(CCObject* sender) {
        bool on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSettingValue<bool>("hitbox-trail", on);
    }

    void onReplay(CCObject*) {
        if (!Bot::get().startReplay()) {
            Notification::create("No saved path for this level yet", NotificationIcon::Warning)->show();
            return;
        }
        // Grab what we need before closing: closing can free this popup.
        PauseLayer* pause = m_pause;
        this->onClose(nullptr);
        if (pause) pause->onRestartFull(nullptr);
    }

    void onStop(CCObject*) {
        Bot::get().stopAll();
        m_autoToggle->toggle(false);
        m_recordToggle->toggle(false);
        this->refresh();
        Notification::create("Auto Macro Bot stopped", NotificationIcon::Info)->show();
    }

    void onSave(CCObject*) {
        if (Bot::get().saveMacro()) {
            Notification::create("Path saved", NotificationIcon::Success)->show();
        } else {
            Notification::create("Nothing to save yet", NotificationIcon::Warning)->show();
        }
        this->refresh();
    }

    void onLoad(CCObject*) {
        if (Bot::get().loadForCurrentLevel()) {
            Notification::create("Path loaded", NotificationIcon::Success)->show();
        } else {
            Notification::create("No saved path found", NotificationIcon::Warning)->show();
        }
        this->refresh();
    }

    void onClear(CCObject*) {
        Bot::get().clearMacro(true);
        Notification::create("Path cleared", NotificationIcon::Info)->show();
        this->refresh();
    }

public:
    static BotPopup* create(PauseLayer* pause) {
        auto ret = new BotPopup();
        if (ret->init(pause)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};
