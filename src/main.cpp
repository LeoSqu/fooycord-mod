// fooycord Geode mod, v0.1: account linking only.
//
// Adds a green chat button to the main menu. Tap it, type the link code fooycord gave you,
// and the mod sends your Geometry Dash account id + username to the fooycord server.
// The server links the account to whoever generated that code and hands back a mod token,
// which we keep for later versions (presence, overlay notifications, opening levels).

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

using namespace geode::prelude;

static std::string serverUrl() {
    auto url = Mod::get()->getSettingValue<std::string>("server");
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

static std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    return s;
}

class FooyLinkPopup : public geode::Popup {
protected:
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_linkBtn = nullptr;
    async::TaskHolder<web::WebResponse> m_request;

    bool init() {
        if (!Popup::init(320.f, 200.f)) return false;
        this->setTitle("Link to Fooycord");

        auto am = GJAccountManager::get();
        bool loggedIn = am->m_accountID > 0;
        std::string who = loggedIn
            ? fmt::format("Logged in as {} (#{})", std::string(am->m_username), am->m_accountID)
            : "Not logged into a GD account!";
        auto whoLabel = CCLabelBMFont::create(who.c_str(), "bigFont.fnt");
        whoLabel->setScale(0.4f);
        whoLabel->setColor(loggedIn ? ccc3(255, 255, 255) : ccc3(255, 90, 90));
        m_mainLayer->addChildAtPosition(whoLabel, Anchor::Center, ccp(0, 45));

        auto hint = CCLabelBMFont::create("Type the code from Fooycord, Settings > Profile", "chatFont.fnt");
        hint->setScale(0.6f);
        hint->setOpacity(190);
        m_mainLayer->addChildAtPosition(hint, Anchor::Center, ccp(0, 22));

        m_input = TextInput::create(220.f, "fooy-abc123", "chatFont.fnt");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(24);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, ccp(0, -5));

        auto btnSpr = ButtonSprite::create("Link", "goldFont.fnt", "GJ_button_01.png", 0.9f);
        m_linkBtn = CCMenuItemSpriteExtra::create(btnSpr, this, menu_selector(FooyLinkPopup::onLink));
        auto menu = CCMenu::create();
        menu->addChild(m_linkBtn);
        menu->setPosition(0, 0);
        m_mainLayer->addChildAtPosition(menu, Anchor::Center, ccp(0, -45));

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(0.55f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, ccp(0, -75));

        auto token = Mod::get()->getSavedValue<std::string>("token", "");
        auto linkedAs = Mod::get()->getSavedValue<std::string>("fooy_username", "");
        if (!token.empty() && !linkedAs.empty()) {
            this->setStatus(fmt::format("Already linked as {} on Fooycord. Linking again is fine.", linkedAs), ccc3(120, 255, 140));
        }
        return true;
    }

    void setStatus(std::string const& text, ccColor3B color) {
        m_status->setString(text.c_str());
        m_status->setColor(color);
    }

    void onLink(CCObject*) {
        auto am = GJAccountManager::get();
        if (am->m_accountID <= 0) {
            this->setStatus("Log into your GD account first.", ccc3(255, 90, 90));
            return;
        }
        std::string code = trimmed(std::string(m_input->getString()));
        if (code.size() < 6) {
            this->setStatus("That code is too short.", ccc3(255, 90, 90));
            return;
        }

        this->setStatus("Talking to fooycord...", ccc3(255, 255, 255));
        m_linkBtn->setEnabled(false);

        auto body = matjson::makeObject({
            { "code", code },
            { "accountId", am->m_accountID },
            { "username", std::string(am->m_username) },
            { "modVersion", Mod::get()->getVersion().toVString() },
        });

        web::WebRequest req;
        req.header("Content-Type", "application/json");
        req.userAgent("fooycord-mod");
        req.timeout(std::chrono::seconds(15));
        req.bodyJSON(body);

        auto url = serverUrl() + "/api/mod/link";
        log::info("fooycord: linking account {} via {}", am->m_accountID, url);

        m_request.spawn(req.post(url), [this](web::WebResponse res) {
            m_linkBtn->setEnabled(true);
            auto json = res.json();
            if (res.ok() && json.isOk()) {
                auto j = json.unwrap();
                auto token = j["token"].asString().unwrapOr("");
                auto user = j["fooyUsername"].asString().unwrapOr("?");
                if (!token.empty()) {
                    Mod::get()->setSavedValue<std::string>("token", token);
                    Mod::get()->setSavedValue<std::string>("fooy_username", user);
                }
                this->setStatus(fmt::format("Linked! You are {} on Fooycord.", user), ccc3(120, 255, 140));
                Notification::create("Linked to Fooycord", NotificationIcon::Success)->show();
                log::info("fooycord: linked as {}", user);
            } else {
                std::string err = "Server said no.";
                if (json.isOk()) err = json.unwrap()["error"].asString().unwrapOr(err);
                else if (res.code() == 0 || res.code() >= 500) err = "Could not reach Fooycord. Check the server URL in mod settings.";
                else err = fmt::format("HTTP {}", res.code());
                this->setStatus(err, ccc3(255, 90, 90));
                log::warn("fooycord: link failed, code {} body {}", res.code(), res.string().unwrapOr(""));
            }
        });
    }

public:
    static FooyLinkPopup* create() {
        auto ret = new FooyLinkPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(FooyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_chatBtn_001.png", 0.9f, CircleBaseColor::Green, CircleBaseSize::Medium
        );
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FooyMenuLayer::onFooycord));
        btn->setID("fooycord-button"_spr);

        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto fallback = CCMenu::create();
            fallback->addChild(btn);
            fallback->setPosition(40, 40);
            this->addChild(fallback);
        }
        return true;
    }

    void onFooycord(CCObject*) {
        FooyLinkPopup::create()->show();
    }
};

$execute {
    log::info("fooycord mod loaded, server = {}", serverUrl());
}
