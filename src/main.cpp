// Fooycord Geode mod.
//
// v0.1  Link your GD account from the main menu (green chat button, type the code).
// v0.2  Report what you are doing to Fooycord so your friends' automations can fire:
//       session start, editor open, first block of the session, blocks placed (batched),
//       level saved, level complete. Auth is the mod token handed out at link time.
// v0.3  Take orders from Fooycord: poll for commands every few seconds while the game runs
//       (which is also how the website knows the game is open) and open levels sent from chat.
// v0.4  Share. A Share button on every level page, pause menu and editor: pick a chat, type a
//       note, send. Online levels go as cards; unpublished levels go as .gmd files. A shared
//       .gmd double-clicked in Fooycord lands in your saved levels and opens on screen.

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>
#include <Geode/binding/LevelBrowserLayer.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/SongInfoObject.hpp>
#include <Geode/binding/LevelTools.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#include <hjfod.gmd-api/include/GMD.hpp>
#include <chrono>
#include <random>
#include <set>

using namespace geode::prelude;

// ---------------------------------------------------------------- helpers

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

static std::string base64Encode(std::vector<uint8_t> const& in) {
    static char const* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += tbl[n & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t n = in[i] << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += '=';
    }
    return out;
}

static void startTicker(); // defined with the command runner below

static std::string g_session;          // random per game launch
static bool g_firstBlockSent = false;  // once per launch
static int g_pendingBlocks = 0;        // batched block_placed count
static std::chrono::steady_clock::time_point g_lastBlockFlush;

static std::string modToken() {
    return Mod::get()->getSavedValue<std::string>("token", "");
}

static int objectCountOf(LevelEditorLayer* lel) {
    if (!lel || !lel->m_objects) return 0;
    return static_cast<int>(lel->m_objects->count());
}

static std::string levelNameOf(GJGameLevel* level) {
    if (!level) return "";
    return std::string(level->m_levelName);
}

static bool inGameplay() {
    return PlayLayer::get() != nullptr || LevelEditorLayer::get() != nullptr;
}

static web::WebRequest authedRequest(std::chrono::seconds timeout = std::chrono::seconds(15)) {
    web::WebRequest req;
    req.header("Content-Type", "application/json");
    req.header("Authorization", "Bearer " + modToken());
    req.userAgent("fooycord-mod");
    req.timeout(timeout);
    return req;
}

// Fire-and-forget event to /api/mod/events. Silently does nothing if not linked.
static void postEvent(std::string const& type, matjson::Value data) {
    if (modToken().empty()) return;
    std::vector<matjson::Value> events;
    events.push_back(matjson::makeObject({ { "type", type }, { "data", data } }));
    auto body = matjson::makeObject({
        { "session", g_session },
        { "events", matjson::Value(events) },
    });
    auto req = authedRequest(std::chrono::seconds(10));
    req.bodyJSON(body);
    (void)async::spawn(req.post(serverUrl() + "/api/mod/events"), [type](web::WebResponse res) {
        if (!res.ok()) log::warn("fooycord: event {} failed, HTTP {}", type, res.code());
    });
}

// ---------------------------------------------------------------- link popup

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

        auto token = modToken();
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

        this->setStatus("Talking to Fooycord...", ccc3(255, 255, 255));
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
                postEvent("session_start", matjson::makeObject({}));
                startTicker();
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

// ---------------------------------------------------------------- share sheet

struct FooyChatEntry {
    std::string channelId;
    std::string label;
};

static std::vector<FooyChatEntry> g_chatCache;
static std::chrono::steady_clock::time_point g_chatCacheAt;

static std::string songTitleOf(GJGameLevel* level) {
    int songId = static_cast<int>(level->m_songID);
    if (songId > 0) {
        if (auto info = MusicDownloadManager::sharedState()->getSongInfoObject(songId)) {
            return std::string(info->m_songName);
        }
        return fmt::format("Song {}", songId);
    }
    return std::string(LevelTools::getAudioTitle(static_cast<int>(level->m_audioTrack)));
}

static std::string songArtistOf(GJGameLevel* level) {
    int songId = static_cast<int>(level->m_songID);
    if (songId > 0) {
        if (auto info = MusicDownloadManager::sharedState()->getSongInfoObject(songId)) {
            return std::string(info->m_artistName);
        }
    }
    return "";
}

// A menu that ignores touches landing outside a given node (the visible part of a scroll list),
// so rows scrolled out of view cannot be pressed.
class FooyClippedMenu : public CCMenu {
public:
    CCNode* m_clipTo = nullptr;
    static FooyClippedMenu* create(CCNode* clipTo) {
        auto ret = new FooyClippedMenu();
        if (ret->init()) {
            ret->autorelease();
            ret->m_clipTo = clipTo;
            return ret;
        }
        delete ret;
        return nullptr;
    }
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        if (m_clipTo) {
            auto local = m_clipTo->convertToNodeSpace(touch->getLocation());
            auto size = m_clipTo->getContentSize();
            if (!CCRect(0, 0, size.width, size.height).containsPoint(local)) return false;
        }
        return CCMenu::ccTouchBegan(touch, event);
    }
};

static constexpr float LIST_W = 340.f;
static constexpr float LIST_H = 112.f;
static constexpr float ROW_H = 30.f;

class FooySharePopup : public geode::Popup {
protected:
    Ref<GJGameLevel> m_level;
    bool m_online = false;
    std::vector<FooyChatEntry> m_chats;
    std::vector<CCScale9Sprite*> m_rowBgs;
    int m_selected = -1;
    CCMenu* m_listMenu = nullptr;
    ScrollLayer* m_scroll = nullptr;
    TextInput* m_msg = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_sendBtn = nullptr;
    async::TaskHolder<web::WebResponse> m_req;

    bool init(GJGameLevel* level) {
        if (!Popup::init(380.f, 270.f)) return false;
        m_level = level;
        m_online = static_cast<int>(level->m_levelID) > 0;
        this->setTitle("Share to Fooycord");

        std::string what = m_online
            ? fmt::format("{} (#{})", std::string(level->m_levelName), static_cast<int>(level->m_levelID))
            : fmt::format("{} as a .gmd file", std::string(level->m_levelName));
        auto whatLabel = CCLabelBMFont::create(what.c_str(), "goldFont.fnt");
        whatLabel->limitLabelWidth(330.f, 0.55f, 0.2f);
        m_mainLayer->addChildAtPosition(whatLabel, Anchor::Center, ccp(0, 92));

        // chat list: a dark box with a scroll layer exactly on top of it
        auto bg = CCScale9Sprite::create("square02b_001.png");
        bg->setContentSize({ LIST_W, LIST_H });
        bg->setColor(ccc3(0, 0, 0));
        bg->setOpacity(90);
        m_mainLayer->addChildAtPosition(bg, Anchor::Center, ccp(0, 24));

        m_scroll = ScrollLayer::create({ LIST_W, LIST_H });
        m_scroll->setAnchorPoint({ 0.f, 0.f });
        m_scroll->ignoreAnchorPointForPosition(false);
        m_mainLayer->addChildAtPosition(m_scroll, Anchor::Center, ccp(-LIST_W / 2, 24 - LIST_H / 2));

        m_listMenu = FooyClippedMenu::create(m_scroll);
        m_listMenu->ignoreAnchorPointForPosition(true);
        m_listMenu->setPosition(0, 0);
        m_listMenu->setContentSize({ LIST_W, LIST_H });
        m_scroll->m_contentLayer->addChild(m_listMenu);

        m_msg = TextInput::create(250.f, "Add a message (optional)", "chatFont.fnt");
        m_msg->setCommonFilter(CommonFilter::Any);
        m_msg->setMaxCharCount(200);
        m_mainLayer->addChildAtPosition(m_msg, Anchor::Center, ccp(-40, -62));

        auto sendSpr = ButtonSprite::create("Send", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        m_sendBtn = CCMenuItemSpriteExtra::create(sendSpr, this, menu_selector(FooySharePopup::onSend));
        auto menu = CCMenu::create();
        menu->addChild(m_sendBtn);
        menu->setPosition(0, 0);
        m_mainLayer->addChildAtPosition(menu, Anchor::Center, ccp(130, -62));

        m_status = CCLabelBMFont::create("Loading your chats...", "chatFont.fnt");
        m_status->setScale(0.55f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, ccp(0, -98));

        this->loadChats();
        return true;
    }

    void setStatus(std::string const& text, ccColor3B color) {
        m_status->setString(text.c_str());
        m_status->setColor(color);
    }

    void loadChats() {
        auto age = std::chrono::steady_clock::now() - g_chatCacheAt;
        if (!g_chatCache.empty() && age < std::chrono::minutes(2)) {
            this->fillChats(g_chatCache);
            return;
        }
        auto req = authedRequest();
        m_req.spawn(req.get(serverUrl() + "/api/mod/chats"), [this](web::WebResponse res) {
            auto json = res.json();
            if (!res.ok() || !json.isOk()) {
                this->setStatus("Could not load your chats. Is Fooycord running?", ccc3(255, 90, 90));
                return;
            }
            std::vector<FooyChatEntry> out;
            auto chats = json.unwrap()["chats"];
            if (chats.isArray()) {
                for (auto const& c : chats.asArray().unwrap()) {
                    auto name = c["name"].asString().unwrapOr("chat");
                    auto channels = c["channels"];
                    if (!channels.isArray()) continue;
                    auto arr = channels.asArray().unwrap();
                    for (auto const& ch : arr) {
                        auto id = ch["id"].asString().unwrapOr("");
                        if (id.empty()) continue;
                        std::string label = arr.size() > 1 ? fmt::format("{} - {}", name, ch["name"].asString().unwrapOr("")) : name; // bigFont has no # glyph
                        out.push_back({ id, label });
                    }
                }
            }
            g_chatCache = out;
            g_chatCacheAt = std::chrono::steady_clock::now();
            this->fillChats(out);
        });
    }

    // Rows are placed by hand from the top down; the content layer grows to fit and starts scrolled to the top.
    void fillChats(std::vector<FooyChatEntry> const& chats) {
        m_chats = chats;
        m_rowBgs.clear();
        m_selected = -1;
        m_listMenu->removeAllChildren();
        float contentH = std::max(LIST_H, ROW_H * static_cast<float>(m_chats.size()) + 4.f);
        m_scroll->m_contentLayer->setContentSize({ LIST_W, contentH });
        m_listMenu->setContentSize({ LIST_W, contentH });
        int i = 0;
        for (auto const& c : m_chats) {
            auto rowBg = CCScale9Sprite::create("GJ_button_04.png");
            rowBg->setContentSize({ LIST_W - 16.f, ROW_H - 4.f });
            auto label = CCLabelBMFont::create(c.label.c_str(), "bigFont.fnt");
            label->limitLabelWidth(LIST_W - 48.f, 0.5f, 0.2f);
            label->setPosition(rowBg->getContentSize().width / 2.f, rowBg->getContentSize().height / 2.f);
            rowBg->addChild(label);
            auto item = CCMenuItemSpriteExtra::create(rowBg, this, menu_selector(FooySharePopup::onPick));
            item->setTag(i);
            item->setPosition(LIST_W / 2.f, contentH - 2.f - ROW_H / 2.f - ROW_H * static_cast<float>(i));
            m_listMenu->addChild(item);
            m_rowBgs.push_back(rowBg);
            i++;
        }
        m_scroll->moveToTop();
        if (m_chats.empty()) this->setStatus("You are not in any chats yet.", ccc3(255, 200, 90));
        else this->setStatus("Pick a chat.", ccc3(255, 255, 255));
    }

    void onPick(CCObject* sender) {
        m_selected = static_cast<CCNode*>(sender)->getTag();
        for (size_t i = 0; i < m_rowBgs.size(); i++) {
            m_rowBgs[i]->setColor(static_cast<int>(i) == m_selected ? ccc3(120, 255, 140) : ccc3(255, 255, 255));
        }
        this->setStatus(fmt::format("Sending to {}", m_chats[m_selected].label), ccc3(255, 255, 255));
    }

    void onSend(CCObject*) {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_chats.size())) {
            this->setStatus("Pick a chat first.", ccc3(255, 90, 90));
            return;
        }
        auto const& target = m_chats[m_selected];
        auto body = matjson::makeObject({
            { "channelId", target.channelId },
            { "message", trimmed(std::string(m_msg->getString())) },
        });
        if (m_online) {
            body["levelId"] = static_cast<int>(m_level->m_levelID);
        } else {
            auto bytes = gmd::ExportGmdFile::from(m_level).setType(gmd::GmdFileType::Gmd).intoBytes();
            if (!bytes) {
                this->setStatus(fmt::format("Could not export: {}", bytes.unwrapErr()), ccc3(255, 90, 90));
                return;
            }
            body["gmd"] = base64Encode(bytes.unwrap());
            body["meta"] = matjson::makeObject({
                { "name", std::string(m_level->m_levelName) },
                { "author", std::string(m_level->m_creatorName) },
                { "description", std::string(m_level->m_levelDesc) },
                { "objects", static_cast<int>(m_level->m_objectCount) },
                { "length", static_cast<int>(m_level->m_levelLength) },
                { "song", songTitleOf(m_level) },
                { "songArtist", songArtistOf(m_level) },
                { "attempts", static_cast<int>(m_level->m_attempts) },
                { "editorSeconds", static_cast<int>(m_level->m_workingTime) + static_cast<int>(m_level->m_workingTime2) },
                { "version", static_cast<int>(m_level->m_levelVersion) },
                { "twoPlayer", static_cast<bool>(m_level->m_twoPlayerMode) },
            });
        }
        this->setStatus("Sending...", ccc3(255, 255, 255));
        m_sendBtn->setEnabled(false);
        auto req = authedRequest(std::chrono::seconds(60));
        req.bodyJSON(body);
        std::string label = target.label;
        m_req.spawn(req.post(serverUrl() + "/api/mod/share"), [this, label](web::WebResponse res) {
            m_sendBtn->setEnabled(true);
            if (res.ok()) {
                Notification::create(fmt::format("Sent to {}", label), NotificationIcon::Success)->show();
                this->onClose(nullptr);
            } else {
                std::string err = fmt::format("HTTP {}", res.code());
                auto json = res.json();
                if (json.isOk()) err = json.unwrap()["error"].asString().unwrapOr(err);
                this->setStatus(err, ccc3(255, 90, 90));
            }
        });
    }

public:
    static FooySharePopup* create(GJGameLevel* level) {
        auto ret = new FooySharePopup();
        if (ret->init(level)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Holds the level for a share button; lives as the button's user object.
class FooyShareTarget : public CCObject {
public:
    Ref<GJGameLevel> m_level;
    static FooyShareTarget* create(GJGameLevel* level) {
        auto t = new FooyShareTarget();
        t->m_level = level;
        t->autorelease();
        return t;
    }
    void onShare(CCObject*) {
        if (modToken().empty()) { FooyLinkPopup::create()->show(); return; }
        if (!m_level) return;
        FooySharePopup::create(m_level)->show();
    }
};

static CCMenuItemSpriteExtra* makeShareButton(GJGameLevel* level, float scale) {
    auto target = FooyShareTarget::create(level);
    CCSprite* spr = nullptr;
    if (CCSpriteFrameCache::get()->spriteFrameByName("GJ_shareBtn_001.png")) {
        spr = CircleButtonSprite::createWithSpriteFrameName("GJ_shareBtn_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::Medium);
    }
    if (!spr) spr = ButtonSprite::create("Share", "goldFont.fnt", "GJ_button_01.png", 0.7f);
    spr->setScale(scale);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, menu_selector(FooyShareTarget::onShare));
    btn->setID("share-button"_spr);
    btn->setUserObject("target"_spr, target);
    return btn;
}

// Add the button to a node-ids menu if it exists, else a menu of our own in a corner.
static void addShareButton(CCLayer* layer, GJGameLevel* level, std::initializer_list<char const*> menuIds, CCPoint fallbackPos, float scale = 0.85f) {
    if (!layer || !level) return;
    auto btn = makeShareButton(level, scale);
    for (auto id : menuIds) {
        if (auto menu = typeinfo_cast<CCMenu*>(layer->getChildByID(id))) {
            menu->addChild(btn);
            menu->updateLayout();
            return;
        }
    }
    auto menu = CCMenu::create();
    menu->setID("share-menu"_spr);
    menu->addChild(btn);
    menu->setPosition(fallbackPos);
    layer->addChild(menu, 100);
}

// ---------------------------------------------------------------- commands from Fooycord

static void markCommandDone(std::string const& id, bool ok, std::string const& error) {
    if (modToken().empty()) return;
    auto req = authedRequest(std::chrono::seconds(10));
    req.bodyJSON(matjson::makeObject({ { "id", id }, { "ok", ok }, { "error", error } }));
    (void)async::spawn(req.post(serverUrl() + "/api/mod/commands/pending"), [](web::WebResponse) {});
}

static std::set<std::string> g_inflight;

// Fetches one level by id and jumps to its page. Falls back to a search-results page if the
// direct fetch fails (RobTop's servers have moods).
class FooyLevelOpener : public CCObject, public LevelManagerDelegate {
public:
    std::string m_cmdId;
    int m_levelId = 0;
    static inline FooyLevelOpener* s_active = nullptr;

    static void open(std::string const& cmdId, int levelId) {
        if (s_active) return; // one at a time
        auto self = new FooyLevelOpener();
        self->m_cmdId = cmdId;
        self->m_levelId = levelId;
        self->retain();
        s_active = self;
        Notification::create(fmt::format("Fooycord: opening level {}", levelId), NotificationIcon::Loading, 2.f)->show();
        auto glm = GameLevelManager::get();
        glm->m_levelManagerDelegate = self;
        glm->getOnlineLevels(GJSearchObject::create(SearchType::Search, std::to_string(levelId)));
    }

    void detach() {
        auto glm = GameLevelManager::get();
        if (glm->m_levelManagerDelegate == this) glm->m_levelManagerDelegate = nullptr;
        s_active = nullptr;
        g_inflight.erase(m_cmdId);
        this->release();
    }

    void loadLevelsFinished(CCArray* levels, char const*, int) override {
        if (levels && levels->count() > 0 && !inGameplay()) {
            auto level = static_cast<GJGameLevel*>(levels->objectAtIndex(0));
            auto scene = LevelInfoLayer::scene(level, false);
            CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, scene));
            Notification::create(fmt::format("Fooycord: {}", std::string(level->m_levelName)), NotificationIcon::Success)->show();
            markCommandDone(m_cmdId, true, "");
        } else {
            this->fallback();
        }
        this->detach();
    }

    void loadLevelsFailed(char const*, int) override {
        this->fallback();
        this->detach();
    }

    void setupPageInfo(gd::string, char const*) override {}

    void fallback() {
        if (inGameplay()) { markCommandDone(m_cmdId, false, "player was mid-level"); return; }
        auto scene = LevelBrowserLayer::scene(GJSearchObject::create(SearchType::Search, std::to_string(m_levelId)));
        CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, scene));
        markCommandDone(m_cmdId, true, "opened as search");
    }
};

// Download a shared .gmd, drop it into the local level list, put it on screen.
static async::TaskHolder<web::WebResponse> g_import;

static void importLevel(std::string const& cmdId, std::string url, std::string const& name) {
    if (url.rfind("http", 0) != 0) url = serverUrl() + url;
    Notification::create(fmt::format("Fooycord: downloading {}", name), NotificationIcon::Loading, 2.f)->show();
    web::WebRequest req;
    req.userAgent("fooycord-mod");
    req.timeout(std::chrono::seconds(120));
    g_import.spawn(req.get(url), [cmdId, name](web::WebResponse res) {
        g_inflight.erase(cmdId);
        if (!res.ok()) {
            markCommandDone(cmdId, false, fmt::format("download failed, HTTP {}", res.code()));
            Notification::create("Fooycord: could not download the level", NotificationIcon::Error)->show();
            return;
        }
        auto dir = Mod::get()->getSaveDir() / "imports";
        (void)file::createDirectoryAll(dir);
        auto path = dir / (cmdId + ".gmd");
        auto written = file::writeBinary(path, res.data());
        if (!written) {
            markCommandDone(cmdId, false, "could not write file");
            return;
        }
        auto imported = gmd::ImportGmdFile::from(path).setType(gmd::GmdFileType::Gmd).intoLevel();
        if (!imported) {
            markCommandDone(cmdId, false, fmt::format("import failed: {}", imported.unwrapErr()));
            Notification::create("Fooycord: that file is not a level", NotificationIcon::Error)->show();
            return;
        }
        auto level = imported.unwrap();
        LocalLevelManager::get()->m_localLevels->insertObject(level, 0);
        markCommandDone(cmdId, true, "");
        Notification::create(fmt::format("Fooycord: {} is in your levels", std::string(level->m_levelName)), NotificationIcon::Success)->show();
        if (!inGameplay()) {
            CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, EditLevelLayer::scene(level)));
        }
    });
}

static async::TaskHolder<web::WebResponse> g_poll;
static bool g_tickerStarted = false;

static void runCommand(matjson::Value const& cmd) {
    auto id = cmd["id"].asString().unwrapOr("");
    auto type = cmd["type"].asString().unwrapOr("");
    if (id.empty() || g_inflight.contains(id)) return;
    if (type == "open_level") {
        int levelId = static_cast<int>(cmd["payload"]["id"].asInt().unwrapOr(0));
        if (levelId <= 0) { markCommandDone(id, false, "bad level id"); return; }
        if (inGameplay()) return; // leave it pending, try again once they are back in a menu
        g_inflight.insert(id);
        FooyLevelOpener::open(id, levelId);
    } else if (type == "import_level") {
        auto url = cmd["payload"]["url"].asString().unwrapOr("");
        auto name = cmd["payload"]["name"].asString().unwrapOr("level");
        if (url.empty()) { markCommandDone(id, false, "no url"); return; }
        if (inGameplay()) return;
        g_inflight.insert(id);
        importLevel(id, url, name);
    } else {
        markCommandDone(id, false, "unknown command " + type);
    }
}

static void pollCommands() {
    if (modToken().empty()) return;
    auto req = authedRequest(std::chrono::seconds(8));
    g_poll.spawn(req.get(serverUrl() + "/api/mod/commands/pending"), [](web::WebResponse res) {
        if (!res.ok()) return;
        auto json = res.json();
        if (!json.isOk()) return;
        auto cmds = json.unwrap()["commands"];
        if (!cmds.isArray()) return;
        for (auto const& c : cmds.asArray().unwrap()) runCommand(c);
    });
}

class FooyTicker : public CCObject {
public:
    void tick(float) { pollCommands(); }
};

static void startTicker() {
    if (g_tickerStarted) return;
    g_tickerStarted = true;
    auto t = new FooyTicker();
    t->retain();
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(FooyTicker::tick), t, 3.f, false);
    pollCommands();
}

// ---------------------------------------------------------------- hooks

class $modify(FooyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        startTicker();

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

// Online level page: the screen with the big play button
class $modify(FooyLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        auto win = CCDirector::get()->getWinSize();
        addShareButton(this, level, { "left-side-menu", "right-side-menu" }, ccp(win.width - 30.f, win.height / 2.f - 100.f));
        return true;
    }
};

// Pause menu while playing anything
class $modify(FooyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_level) return;
        auto win = CCDirector::get()->getWinSize();
        addShareButton(this, pl->m_level, { "right-button-menu", "left-button-menu" }, ccp(win.width - 30.f, win.height / 2.f), 0.7f);
    }
};

// Your own level's page (the one with Edit and Play)
class $modify(FooyEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        auto win = CCDirector::get()->getWinSize();
        addShareButton(this, level, { "level-actions-menu", "level-edit-menu" }, ccp(win.width - 30.f, 30.f), 0.7f);
        return true;
    }
};

// Editor opened
class $modify(FooyLevelEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        postEvent("editor_open", matjson::makeObject({
            { "level", levelNameOf(level) },
            { "objects", objectCountOf(this) },
        }));
        return true;
    }
};

// Object placed from the object tab. First one per launch is its own event; the rest are batched.
class $modify(FooyEditorUI, EditorUI) {
    void onCreateObject(int id) {
        EditorUI::onCreateObject(id);
        auto lel = m_editorLayer;
        auto data = matjson::makeObject({
            { "level", lel ? levelNameOf(lel->m_level) : std::string("") },
            { "objects", objectCountOf(lel) },
            { "objectId", id },
        });
        if (!g_firstBlockSent) {
            g_firstBlockSent = true;
            postEvent("first_block", data);
        }
        // batch block_placed: flush at most every 2 s
        g_pendingBlocks++;
        auto now = std::chrono::steady_clock::now();
        if (now - g_lastBlockFlush > std::chrono::seconds(2)) {
            g_lastBlockFlush = now;
            data["count"] = g_pendingBlocks;
            g_pendingBlocks = 0;
            postEvent("block_placed", data);
        }
    }
};

// Editor pause menu: share button + level saved event
class $modify(FooyEditorPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorPauseLayer::init(lel)) return false;
        if (lel && lel->m_level) {
            auto win = CCDirector::get()->getWinSize();
            addShareButton(this, lel->m_level, { "actions-menu", "small-actions-menu" }, ccp(win.width - 30.f, 30.f), 0.6f);
        }
        return true;
    }

    void saveLevel() {
        EditorPauseLayer::saveLevel();
        auto lel = m_editorLayer;
        postEvent("level_saved", matjson::makeObject({
            { "level", lel ? levelNameOf(lel->m_level) : std::string("") },
            { "objects", objectCountOf(lel) },
        }));
    }
};

// Level completed (any level, including playtests of your own)
class $modify(FooyPlayLayer, PlayLayer) {
    void levelComplete() {
        PlayLayer::levelComplete();
        postEvent("level_complete", matjson::makeObject({
            { "level", levelNameOf(m_level) },
            { "levelId", m_level ? static_cast<int>(m_level->m_levelID) : 0 },
            { "attempts", m_level ? static_cast<int>(m_level->m_attempts) : 0 },
        }));
    }
};

$execute {
    std::mt19937_64 rng(std::random_device{}());
    g_session = fmt::format("{:016x}", rng());
    g_lastBlockFlush = std::chrono::steady_clock::now();
    log::info("fooycord mod loaded, server = {}, session = {}", serverUrl(), g_session);
    if (!modToken().empty()) postEvent("session_start", matjson::makeObject({}));
}
