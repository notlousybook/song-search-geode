#include <Geode/Geode.hpp>
#include <Geode/modify/CustomSongLayer.hpp>
#include <Geode/modify/CustomSongWidget.hpp>

#include <sstream>
#include <deque>
#include <algorithm>
#include <cmath>

#define FTS_FUZZY_MATCH_IMPLEMENTATION
#include "fts_fuzzy_match.h"

using namespace geode::prelude;

constexpr int n_widgets = 6;
constexpr float widget_width = 350.f;
constexpr float widget_height = 50.f;
constexpr float widget_spacing = 5.f;

// Filter modes (ported from Alphalaneous/Improved-Song-Browser's SortMode).
enum class FilterMode {
    Priority,
    Name,
    Artist,
    ID
};

// Data scraped from Jukebox's own CustomSongWidget. Jukebox ships no public
// API (no headers bundled), but it tags its disc button with the ID
// "nong-button", and the disc frame comes from a spritesheet Jukebox
// registers globally - so we can copy the icon and metadata it already
// resolved instead of re-implementing NONG storage.
struct NongInfo {
    std::string discFrame;   // "JB_PinDisc.png" or "JB_PinDiscGold.png"
    bool verified = false;
    bool defaultActive = true;
    std::string name;
    std::string artist;
};

// songID -> NongInfo, filled by the spy hook below. Empty when Jukebox is
// not installed or the song has no NONG configured.
static std::unordered_map<int, NongInfo>& nongCache() {
    static std::unordered_map<int, NongInfo> cache;
    return cache;
}

static bool jukeboxPresent() {
    return Loader::get()->getLoadedMod("fleym.nongd") != nullptr;
}

// Collect the disc Jukebox added to the widget it just built, plus the song
// metadata it resolved. Jukebox has no public API, so read what it already
// drew: its disc carries the ID "nong-button", and the sprite it swaps in for
// verified NONGs is the gold variant.
static void harvestJukeboxWidget(CustomSongWidget* widget, SongInfoObject* song) {
    if (!jukeboxPresent() || !widget || !song) return;
    auto node = widget->getChildByID("nong-button");
    if (!node) return; // Jukebox found no NONG data for this song
    auto item = static_cast<CCMenuItemSpriteExtra*>(node);
    // getNormalImage() returns a CCNode*; Jukebox swaps this sprite for the
    // gold variant when a NONG is verified.
    auto spr = static_cast<CCSprite*>(item->getNormalImage());
    if (!spr) return;
    std::string frameName = "JB_PinDisc.png";
    if (auto* frame = spr->displayFrame()) {
        frameName = frame->getFrameName();
    }
    NongInfo info;
    info.discFrame = frameName;
    // Jukebox swaps in the gold disc for verified NONGs (JBSongWidget::setVerifiedUI)
    info.verified = frameName.find("Gold") != std::string::npos;
    info.defaultActive = true;
    info.name = song->m_songName;
    info.artist = song->m_artistName;
    nongCache()[song->m_songID] = info;
}

// Walk the live scene and harvest from every CustomSongWidget in it. Called on
// a delay because Jukebox's own $modify hook may run either before or after
// ours - there is no ordering guarantee between mods - so reading its UI
// immediately at init time is a race we would sometimes lose.
static void harvestSceneWidgets() {
    if (!jukeboxPresent()) return;
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return;
    // CCArrayExt<CustomSongWidget*> iterating the scene's children and keeps
    // only nodes of that type, which is exactly the filter we need here.
    for (auto widget : CCArrayExt<CustomSongWidget*>(scene->getChildren())) {
        if (!widget) continue;
        if (auto song = widget->m_songInfoObject) {
            harvestJukeboxWidget(widget, song);
        }
    }
}

class SimpleSongWidget;

class CustomSongSearchLayer : public FLAlertLayer, public TextInputDelegate, public FLAlertLayerProtocol {
public:
    CustomSongLayer* m_parent = nullptr;
    CCTextInputNode* m_input = nullptr;
    CCNode* m_div = nullptr;
    std::vector<std::pair<std::string, SongInfoObject*>> m_downloaded_songs;
    float m_y_scroll = 0;
    float m_prev_y_scroll = 0;
    // Smoothed scrolling: input moves the *target*, and update() eases the
    // rendered scroll toward it with expo-out so dragging and the wheel both
    // glide instead of snapping.
    float m_target_scroll = 0;
    float m_max_scroll = 0.f;
    std::deque<SimpleSongWidget*> m_scroll_elements;
    std::vector<SongInfoObject*> m_sorted;
    CCLabelBMFont* m_total_songs_label = nullptr;
    CCPoint m_prev_mouse_pos = CCPointZero;
    CCMenuItemSpriteExtra* m_delete_button = nullptr;
    // Local preview state. 2.2081 bindings removed GameSoundManager and
    // FMODAudioEngine::isBackgroundMusicPlaying, so track it ourselves.
    // Preview plays on channel 0 via FMODAudioEngine::playMusic/stopMusic.
    gd::string m_preview_path;
    bool m_previewing = false;

    // Filtering (ported from Improved-Song-Browser's SortMode togglers)
    FilterMode m_filterMode = FilterMode::Priority;
    bool m_ascending = false;
    CCMenu* m_filterMenu = nullptr;
    std::vector<CCMenuItemToggler*> m_filterTogglers;
    CCMenuItemToggler* m_orderToggler = nullptr;

    bool isPreviewing(const gd::string& path) const {
        return m_previewing && m_preview_path == path;
    }

    void stopPreview() {
        if (m_previewing) {
            FMODAudioEngine::sharedEngine()->stopMusic(0);
            m_previewing = false;
            m_preview_path = "";
        }
    }

    void startPreview(const gd::string& path) {
        stopPreview();
        FMODAudioEngine::sharedEngine()->playMusic(path, false, 0.f, 0);
        m_preview_path = path;
        m_previewing = true;
    }

    bool init(CustomSongLayer* parent) {
        // NB: FLAlertLayer::init(int opacity) is the Windows-bound init
        // (CCLayerColor::initWithColor has no win binding in 2.2081).
        if (!FLAlertLayer::init(150)) return false;
        m_mainLayer = CCLayer::create();
        addChild(m_mainLayer);

        m_parent = parent;

        auto win_size = CCDirector::sharedDirector()->getWinSize();

        this->registerWithTouchDispatcher();
        this->incrementForcePrio();

        m_div = CCNode::create();
        m_div->setPosition({win_size.width / 2, 0});

        constexpr float total_height = widget_height * (n_widgets - 1);

        // look, i really tried to use a CCDrawNode here
        // but it just kept fucking up
        // so instead make my own 1x1 white dot sprite
        // cant rely on square.png as some people replace it
        // for no particles
        // (kept from original matcool/small-gd-mods src/song-search.cpp)

        auto texture = new CCTexture2D();
        texture->autorelease();
        const unsigned char data[3] = {255, 255, 255};
        texture->initWithData(data, kCCTexture2DPixelFormat_RGB888, 1, 1, {1, 1});
        texture->setAliasTexParameters();
        auto stencil = CCSprite::createWithTexture(texture);
        stencil->setScaleX(win_size.width / stencil->getContentSize().width);
        stencil->setScaleY(total_height / stencil->getContentSize().height);
        stencil->setAnchorPoint({0, 0});

        auto clip = CCClippingNode::create(stencil);
        clip->addChild(m_div);
        clip->setAlphaThreshold(0.1f);
        m_mainLayer->addChild(clip);

        m_input = CCTextInputNode::create(200.f, 50.f, "Song Search", "bigFont.fnt");
        m_input->setPosition({win_size.width / 2, total_height + 28.51f});
        m_input->setLabelPlaceholderColor({200, 200, 200});
        m_input->setAllowedChars("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQSRTUVWXYZ0123456789-_+=$!#&*\"\':@()[]{}.,;/?\\~^ <>%");
        m_input->setDelegate(this);
        m_mainLayer->addChild(m_input);

        auto bg = cocos2d::extension::CCScale9Sprite::create("GJ_square05.png");
        bg->setContentSize({widget_width + 50.f, 60.f});
        bg->setPosition(m_input->getPosition());
        bg->setZOrder(-1);
        m_mainLayer->addChild(bg);

        setKeypadEnabled(true);
        setTouchEnabled(true);
        setMouseEnabled(true);

        auto songs_ = MusicDownloadManager::sharedState()->getDownloadedSongs();
        for (auto ob : CCArrayExt<SongInfoObject*>(songs_)) {
            auto song = ob;
            if (!song) continue;
            std::stringstream builder;
            builder << song->m_songName.c_str() << " " << song->m_artistName.c_str();
            m_downloaded_songs.push_back({builder.str(), song});
        }

        m_total_songs_label = CCLabelBMFont::create("", "goldFont.fnt");
        m_total_songs_label->setAnchorPoint({0, 1});
        m_total_songs_label->setScale(0.4f);
        // Top-left corner so the filter strip can own the top-right.
        m_total_songs_label->setPosition({30.f, win_size.height});
        m_mainLayer->addChild(m_total_songs_label);

        // Filter/sort toggler strip, togglable from settings
        if (Mod::get()->getSettingValue<bool>("show-filters")) {
            this->setupFilterMenu(win_size);
        }

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto close_button = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png"),
            this, menu_selector(CustomSongSearchLayer::on_close)
        );
        close_button->setPosition(25, win_size.height - 25);
        menu->addChild(close_button);

        m_delete_button = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_deleteSongBtn_001.png"),
            this, menu_selector(CustomSongSearchLayer::on_delete)
        );
        auto s = 0.5f;
        m_delete_button->getNormalImage()->setScale(s);
        m_delete_button->setPositionX(widget_width / 2.f + m_delete_button->getContentSize().width * s / 2.f + 2.f);
        auto delete_menu = CCMenu::create();
        delete_menu->setPosition({win_size.width / 2.f, 0});
        delete_menu->addChild(m_delete_button);
        clip->addChild(delete_menu);

        schedule(schedule_selector(CustomSongSearchLayer::check_mouse_movement), 1.f/30.f); // 30fps cuz ynot
        // Eased scrolling runs from update(); setKeypadEnabled etc. already
        // gate input, this only drives the scroll interpolation.
        this->scheduleUpdate();

        do_search();
        return true;
    }

    void check_mouse_movement(float) {
        const auto director = CCDirector::sharedDirector();
        const auto win_size = director->getWinSize();
        auto gl = director->getOpenGLView();
        const auto frame_size = gl->getFrameSize();
        auto mouse_pos = gl->getMousePosition();
        mouse_pos.x = (mouse_pos.x / frame_size.width) * win_size.width;
        mouse_pos.y = (1.f - mouse_pos.y / frame_size.height) * win_size.height;
        if (mouse_pos.y != m_prev_mouse_pos.y) {
            m_prev_mouse_pos = mouse_pos;
            move_delete_btn();
        }

        m_prev_mouse_pos = mouse_pos;
    }

    void move_delete_btn() {
        constexpr float height = widget_height + widget_spacing;
        constexpr float clip_height = widget_height * (n_widgets - 1);
        int i = static_cast<int>((clip_height - m_prev_mouse_pos.y + m_y_scroll) / height);
        bool e = (m_prev_mouse_pos.y < clip_height) && (i >= 0 && i < static_cast<int>(m_sorted.size()));
        if (e) {
            m_delete_button->setPositionY((n_widgets - i) * height - height * 2.f + height / 2.f - 13.f + m_y_scroll);
            m_delete_button->setTag(i);
        }
        m_delete_button->setVisible(e);
        m_delete_button->setEnabled(e);
    }

    void textChanged(CCTextInputNode* input) override {
        (void)input;
        do_search();
    }

    // The search/index string depends on the active filter mode, exactly like
    // Improved-Song-Browser's handleSearch switches on SortMode.
    std::string filterKeyFor(SongInfoObject* song) const {
        switch (m_filterMode) {
            case FilterMode::Priority:
            case FilterMode::Name:
                return std::string(song->m_songName.c_str());
            case FilterMode::Artist:
                return std::string(song->m_artistName.c_str());
            case FilterMode::ID:
                return std::to_string(song->m_songID);
        }
        return "";
    }

    void do_search() {
        if (m_downloaded_songs.empty()) return;
        std::vector<std::pair<int, SongInfoObject*>> songs;
        songs.reserve(m_downloaded_songs.size());
        auto input_value = m_input->getString();
        const bool empty_input = input_value.empty();
        for (const auto& [index_str, song] : m_downloaded_songs) {
            int score = 0;
            const std::string key = filterKeyFor(song);
            if (!empty_input && input_value != "." && !fts::fuzzy_match(input_value.c_str(), key.c_str(), score)) continue;
            songs.push_back({score, song});
        }
        if (!empty_input && songs.size() > 1) {
            std::sort(songs.begin(), songs.end(), [&](const auto& a, const auto& b) {
                return a.first > b.first;
            });
        } else {
            // No query (or '.' intentional empty search): fall back to the
            // mode's natural ordering so the list is still useful.
            std::stable_sort(songs.begin(), songs.end(),
                [this](const auto& a, const auto& b) { return sortLess(a.second, b.second); });
            if (m_ascending) {
                std::reverse(songs.begin(), songs.end());
            }
        }
        m_sorted.clear();
        m_sorted.reserve(songs.size());
        for (const auto& [score, song] : songs) {
            (void)score;
            m_sorted.push_back(song);
        }
        // New result set: reset both the rendered scroll and the easing target
        // so a previous position can't linger.
        m_y_scroll = 0.f;
        m_target_scroll = 0.f;
        update_scroll_elements(true);

        std::stringstream stream;
        stream << m_sorted.size() << " result";
        if (m_sorted.size() != 1)
            stream << "s";
        m_total_songs_label->setString(stream.str().c_str(), true);

        move_delete_btn();
    }

    bool sortLess(SongInfoObject* a, SongInfoObject* b) const {
        switch (m_filterMode) {
            case FilterMode::Priority:
                return false; // priority ordering is GD's, leave as-is
            case FilterMode::Artist: {
                std::string x(a->m_artistName.c_str()), y(b->m_artistName.c_str());
                for (auto& c : x) c = static_cast<char>(std::tolower((unsigned char)c));
                for (auto& c : y) c = static_cast<char>(std::tolower((unsigned char)c));
                return x < y;
            }
            case FilterMode::Name: {
                std::string x(a->m_songName.c_str()), y(b->m_songName.c_str());
                for (auto& c : x) c = static_cast<char>(std::tolower((unsigned char)c));
                for (auto& c : y) c = static_cast<char>(std::tolower((unsigned char)c));
                return x < y;
            }
            case FilterMode::ID:
                return a->m_songID < b->m_songID;
        }
        return false;
    }

    void setFilterMode(FilterMode mode, CCMenuItemToggler* toggler) {
        m_filterMode = mode;
        for (auto* t : m_filterTogglers) {
            if (t) t->toggle(false);
        }
        if (toggler) toggler->toggle(true);
        do_search();
    }

    void toggleOrder(CCObject*) {
        m_ascending = !m_ascending;
        if (m_orderToggler) {
            m_orderToggler->toggle(m_ascending);
        }
        do_search();
    }

    // Icon-only toggler. Our own resources are packaged as
    // resources/<mod.id>/<name>, and Geode adds that dir to the search path,
    // so they are addressed by bare filename - NOT "resources/<name>".
    CCMenuItemToggler* createToggler(
        ZStringView sprite, bool useFrameName,
        geode::Function<void(CCMenuItemToggler*)> callback
    ) {
        CCSprite* onSpr = useFrameName
            ? CCSprite::createWithSpriteFrameName(sprite.c_str())
            : CCSprite::create(sprite.c_str());
        CCSprite* offSpr = useFrameName
            ? CCSprite::createWithSpriteFrameName(sprite.c_str())
            : CCSprite::create(sprite.c_str());
        if (!onSpr || !offSpr) return nullptr;
        onSpr->setScale(0.95f);
        offSpr->setScale(0.75f);
        onSpr->setColor({255, 255, 255});
        offSpr->setColor({120, 120, 120});
        auto toggler = CCMenuItemExt::createToggler(onSpr, offSpr, std::move(callback));
        return toggler;
    }

    void setupFilterMenu(const CCSize& win_size) {
        if (m_filterMenu) return;
        m_filterMenu = CCMenu::create();
        m_filterMenu->setID("filter-menu"_spr);
        m_filterMenu->setPosition({win_size.width - 100.f, win_size.height - 25.f});
        m_filterMenu->setScale(0.45f);
        m_filterMenu->setLayout(
            SimpleRowLayout::create()
                ->setMainAxisAlignment(MainAxisAlignment::Start)
                ->setMainAxisScaling(AxisScaling::Fit)
                ->setCrossAxisScaling(AxisScaling::Fit)
                ->setGap(3.f)
        );
        m_mainLayer->addChild(m_filterMenu);

        m_orderToggler = this->createToggler(
            "GJ_sortIcon_001.png", true,
            [this](CCMenuItemToggler* t) {
                m_ascending = !m_ascending;
                t->toggle(m_ascending);
                do_search();
            }
        );
        if (m_orderToggler) {
            m_orderToggler->setID("order-btn"_spr);
            m_filterMenu->addChild(m_orderToggler);
        }

        auto spacer = CCNode::create();
        spacer->setContentSize({1.f, 3.f});
        m_filterMenu->addChild(spacer);

        auto* prio = this->createToggler(
            "GJ_timeIcon_001.png", true,
            [this](CCMenuItemToggler* t) { this->setFilterMode(FilterMode::Priority, t); }
        );
        if (prio) { prio->setID("priority-btn"_spr); prio->toggle(true); m_filterTogglers.push_back(prio); m_filterMenu->addChild(prio); }

        auto* name = this->createToggler(
            "GJ_noteIcon_001.png", true,
            [this](CCMenuItemToggler* t) { this->setFilterMode(FilterMode::Name, t); }
        );
        if (name) { name->setID("name-btn"_spr); m_filterTogglers.push_back(name); m_filterMenu->addChild(name); }

        // our own resources, addressed by bare filename (Geode adds the mod's
        // resource dir to the search path)
        auto* artist = this->createToggler(
            "artist_icon.png", false,
            [this](CCMenuItemToggler* t) { this->setFilterMode(FilterMode::Artist, t); }
        );
        if (artist) { artist->setID("artist-btn"_spr); m_filterTogglers.push_back(artist); m_filterMenu->addChild(artist); }

        auto* id = this->createToggler(
            "id_icon.png", false,
            [this](CCMenuItemToggler* t) { this->setFilterMode(FilterMode::ID, t); }
        );
        if (id) { id->setID("id-btn"_spr); m_filterTogglers.push_back(id); m_filterMenu->addChild(id); }

        m_filterMenu->updateLayout();
    }

    void on_close(CCObject*) {
        keyBackClicked();
    }

    void on_delete(CCObject*) {
        auto alert = FLAlertLayer::create(this, "Warning", "Are you sure you want to delete this?", "No", "Yes");
        alert->setTag(m_delete_button->getTag());
        alert->show();
    }

    void FLAlert_Clicked(FLAlertLayer* alert, bool btn2) override {
        if (btn2) {
            int i = alert->getTag();
            if (i < 0 || i >= static_cast<int>(m_sorted.size())) return; // sanity check
            auto song = m_sorted[i];
            int songID = song->m_songID;
            m_sorted.erase(m_sorted.begin() + i);
            auto mdm = MusicDownloadManager::sharedState();
            auto path = mdm->pathForSong(songID);
            if (isPreviewing(path)) {
                stopPreview();
            }
            mdm->deleteSong(songID);
            // Remove the now-deleted song from the search index too,
            // otherwise it lingers until the layer is reopened.
            m_downloaded_songs.erase(
                std::remove_if(m_downloaded_songs.begin(), m_downloaded_songs.end(),
                    [&](const auto& e) { return e.second == song; }),
                m_downloaded_songs.end()
            );
            update_scroll_elements(true);

            if (m_parent && m_parent->m_songWidget) {
                m_parent->m_songWidget->updateSongObject(song);
            }
        }
    }

    void update_scroll_elements(bool force = false);

    void ccTouchMoved(CCTouch *pTouch, CCEvent *pEvent) override {
        FLAlertLayer::ccTouchMoved(pTouch, pEvent);
        // Drag drives the target; update() does the easing.
        m_target_scroll += pTouch->getDelta().y;
        clampTargetScroll();
    }

    void scrollWheel(float y, float x) override {
        (void)x;
        m_target_scroll += y * 1.5f;
        clampTargetScroll();
    }

    // Keep the target inside the list bounds so easing never overshoots.
    void clampTargetScroll() {
        constexpr float height = widget_height + widget_spacing;
        const auto maxScroll = height * (static_cast<int>(m_sorted.size()) - n_widgets + 3);
        m_max_scroll = maxScroll < 0 ? 0.f : maxScroll;
        m_target_scroll = std::clamp(m_target_scroll, 0.f, m_max_scroll);
    }

    // Expo-out easing toward m_target_scroll. Time-corrected, so it feels the
    // same at any framerate: factor = 1 - exp(-stiffness * dt).
    void update(float dt) override {
        if (m_sorted.empty()) return;
        float factor;
        if (dt > 0.f) {
            factor = 1.f - std::exp(-18.f * dt);
        } else {
            factor = 1.f;
        }
        const float delta = m_target_scroll - m_y_scroll;
        // Snap when the remaining distance is sub-pixel so we stop scheduling
        // redundant work once settled.
        if (std::abs(delta) < 0.01f) {
            if (m_y_scroll != m_target_scroll) {
                m_y_scroll = m_target_scroll;
                update_scroll_elements();
                move_delete_btn();
            }
            return;
        }
        m_y_scroll += delta * factor;
        update_scroll_elements();
        move_delete_btn();
    }

    void keyBackClicked() override {
        stopPreview();
        FLAlertLayer::keyBackClicked();
    }

    static CustomSongSearchLayer* create(CustomSongLayer* parent) {
        auto ret = new CustomSongSearchLayer();
        if (ret && ret->init(parent)) {
            ret->autorelease();
        } else {
            CC_SAFE_DELETE(ret);
        }
        return ret;
    }
};

class SimpleSongWidget : public CCNode {
protected:
    SongInfoObject* m_song = nullptr;
    CCLabelBMFont* m_name_label = nullptr;
    CCLabelBMFont* m_artist_label = nullptr;
    CCSprite* m_nong_icon = nullptr;
    CCMenuItemSpriteExtra* m_play_button = nullptr;
    CCMenuItemSpriteExtra* m_select_button = nullptr;
    CustomSongSearchLayer* m_parent = nullptr;
public:
    bool init(CustomSongSearchLayer* s) {
        m_parent = s;
        m_song = nullptr;
        constexpr float width = 350.f;
        constexpr float height = widget_height;
        auto bg = cocos2d::extension::CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({width, height});
        bg->setZOrder(-1);
        addChild(bg);
        m_name_label = CCLabelBMFont::create("?", "bigFont.fnt");
        m_name_label->setAnchorPoint({0, 0.5f});
        m_name_label->setPosition({-width / 2.f + 8.f, height / 2.f - 13.5f});
        addChild(m_name_label);
        m_artist_label = CCLabelBMFont::create("who", "goldFont.fnt");
        m_artist_label->setAnchorPoint({0, 1});
        m_artist_label->setPosition({-width / 2.f + 8.f, height / 2.f - 21.6f});
        m_artist_label->setScale(0.6f);
        addChild(m_artist_label);
        // NONG disc for the row. Jukebox registers JB_PinDisc.png in a global
        // spritesheet, so the frame resolves whenever it is installed; the
        // sprite just stays hidden otherwise.
        m_nong_icon = CCSprite::createWithSpriteFrameName("JB_PinDisc.png");
        if (m_nong_icon) {
            m_nong_icon->setPosition({-width / 2.f + 16.f, height / 2.f + 15.f});
            m_nong_icon->setScale(0.45f);
            m_nong_icon->setID("nong-symbol"_spr);
            m_nong_icon->setVisible(false);
            addChild(m_nong_icon);
        }
        auto menu = CCMenu::create();
        addChild(menu);
        m_play_button = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_playMusicBtn_001.png"),
            this,
            menu_selector(SimpleSongWidget::on_play)
        );
        menu->addChild(m_play_button);
        m_select_button = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_selectSongBtn_001.png"),
            this,
            menu_selector(SimpleSongWidget::on_select)
        );
        m_play_button->setPositionX(width / 2.f - m_play_button->getScaledContentSize().width / 2.f - 3.f);
        m_select_button->setPositionX(
            m_play_button->getPositionX() - m_play_button->getScaledContentSize().width / 2.f - m_select_button->getScaledContentSize().width / 2.f - 2.f
        );
        menu->setPosition({0, 1});
        menu->addChild(m_select_button);
        return true;
    }

    // Swaps the select button between "Use" and "In Use" for whichever song the
    // level currently has set, mirroring CustomSongWidget::songStateChanged.
    void update_select_btn() {
        if (!m_song || !m_select_button) return;
        int currentID = 0;
        bool known = false;
        if (auto editorUI = EditorUI::get()) {
            if (editorUI->m_editorLayer &&
                editorUI->m_editorLayer->m_levelSettings &&
                editorUI->m_editorLayer->m_levelSettings->m_level) {
                currentID = editorUI->m_editorLayer->m_levelSettings->m_level->m_songID;
                known = true;
            }
        }
        const bool selected = known && currentID == m_song->m_songID;
        m_select_button->setEnabled(!selected);
        m_select_button->setSprite(CCSprite::createWithSpriteFrameName(
            selected ? "GJ_selectSongOnBtn_001.png" : "GJ_selectSongBtn_001.png"
        ));
        m_select_button->updateSprite();
    }

    void set_song(SongInfoObject* song) {
        m_song = song;
        auto nong_it = nongCache().find(song->m_songID);
        const bool hasNong = nong_it != nongCache().end();
        const bool useNongNames =
            hasNong && Mod::get()->getSettingValue<bool>("use-nong-metadata");
        // Jukebox may have already swapped name/artist on the SongInfoObject;
        // the cache lets us prefer those without re-reading its state.
        // NB: Geode 5.11's CCLabelBMFont::setString takes (const char*, bool).
        const std::string artistText =
            useNongNames ? nong_it->second.artist : song->m_artistName.c_str();
        const std::string nameText =
            useNongNames ? nong_it->second.name : song->m_songName.c_str();
        m_artist_label->setString(artistText.c_str(), true);
        m_name_label->setString(nameText.c_str(), true);
        float scale = (std::min)(0.8f, 250.f / m_name_label->getContentSize().width);
        m_name_label->setScale((std::min)(scale, 15.f / m_name_label->getContentSize().height));
        update_nong_icon();
        update_select_btn();
        update_play_btn();
    }

    // Renders Jukebox's own disc sprite in the row. No-op when Jukebox is
    // missing or the setting is off, so the mod stays fully functional.
    void update_nong_icon() {
        if (!m_nong_icon) return;
        bool show = false;
        if (m_song && Mod::get()->getSettingValue<bool>("nong-symbols")) {
            auto it = nongCache().find(m_song->m_songID);
            if (it != nongCache().end()) {
                auto frame = CCSpriteFrameCache::get()->spriteFrameByName(it->second.discFrame.c_str());
                if (frame) {
                    m_nong_icon->setDisplayFrame(frame);
                    m_nong_icon->setColor(it->second.verified ? ccColor3B{255, 236, 140} : ccColor3B{255, 255, 255});
                    show = true;
                }
            }
        }
        m_nong_icon->setVisible(show);
    }

    void update_play_btn() {
        if (!m_song || !m_parent) return;
        auto song_path = MusicDownloadManager::sharedState()->pathForSong(m_song->m_songID);
        const auto sprite = m_parent->isPreviewing(song_path) ? "GJ_stopMusicBtn_001.png" : "GJ_playMusicBtn_001.png";
        // Prefer setSprite (Geode addition) over setNormalImage, which skips
        // CCMenuItemSpriteExtra sizing operations.
        m_play_button->setSprite(CCSprite::createWithSpriteFrameName(sprite));
        m_play_button->updateSprite();
    }

    void on_select(CCObject* obj) {
        (void)obj;
        if (!m_parent || !m_parent->m_parent || !m_song) return;
        // Forward through the layer's CustomSongDelegate (mirrors
        // CustomSongWidget::onSelect -> m_songDelegate->songIDChanged).
        if (auto delegate = m_parent->m_parent->m_songDelegate) {
            delegate->songIDChanged(m_song->m_songID);
        }
        MusicDownloadManager::sharedState()->songStateChanged();
        if (m_parent->m_parent->m_songWidget) {
            m_parent->m_parent->m_songWidget->updateSongObject(m_song);
        }
        // The level's songID just changed, so every row's "Use" state is stale
        for (auto& widget : m_parent->m_scroll_elements) {
            if (widget) widget->update_select_btn();
        }
    }

    void on_play(CCObject* obj) {
        (void)obj;
        if (!m_song || !m_parent) return;
        auto song_path = MusicDownloadManager::sharedState()->pathForSong(m_song->m_songID);
        if (m_parent->isPreviewing(song_path)) {
            m_parent->stopPreview();
            update_play_btn();
            return;
        }
        m_parent->startPreview(song_path);
        for (auto& widget : m_parent->m_scroll_elements) {
            if (widget) widget->update_play_btn();
        }
    }

    static SimpleSongWidget* create(CustomSongSearchLayer* parent) {
        auto ret = new SimpleSongWidget();
        if (ret && ret->init(parent)) {
            ret->autorelease();
        } else {
            CC_SAFE_DELETE(ret);
        }
        return ret;
    }
};

void CustomSongSearchLayer::update_scroll_elements(bool force) {
    constexpr float spacing = widget_spacing;
    constexpr float height = widget_height + spacing;
    if (m_scroll_elements.empty()) {
        for (int i = 0; i < n_widgets; ++i) {
            auto widget = SimpleSongWidget::create(this);
            m_div->addChild(widget);
            m_scroll_elements.push_back(widget);
        }
    }
    const auto m = height * (static_cast<int>(m_sorted.size()) - n_widgets + 3);
    // std::clamp with a max < min is weird
    if (m < 0)
        m_y_scroll = 0.f;
    else
        m_y_scroll = std::clamp(m_y_scroll, 0.f, m);
    // Track the bound so input can clamp the target against the same limit.
    m_max_scroll = m < 0 ? 0.f : m;
    const auto i_offset = static_cast<size_t>(m_y_scroll / height);
    int diff = static_cast<int>(m_prev_y_scroll / height) - static_cast<int>(i_offset);
    // :worried:
    // all of this to avoid calling set_song when not needed,
    // which ends up saving like 10 FPS on my machine
    if (!force && diff != 0) {
        if (diff < 0) {
            for (int i = -diff; i > 0; --i) {
                auto element = m_scroll_elements.front();
                m_scroll_elements.pop_front();
                const auto j = i_offset + n_widgets - i;
                if (j < m_sorted.size())
                    element->set_song(m_sorted[j]);
                m_scroll_elements.push_back(element);
            }
        } else {
            for (int i = 0; i < diff; ++i) {
                auto element = m_scroll_elements.back();
                m_scroll_elements.pop_back();
                const auto j = i_offset + (diff - i - 1);
                if (j < m_sorted.size())
                    element->set_song(m_sorted[j]);
                m_scroll_elements.push_front(element);
            }
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(n_widgets); ++i) {
        auto widget = m_scroll_elements[i];
        const size_t actual_i = i + i_offset;
        if (actual_i < m_sorted.size()) {
            auto song = m_sorted[actual_i];
            widget->setVisible(true);
            if (force) widget->set_song(song);
            widget->setPositionY((n_widgets - i) * height + ::fmod(m_y_scroll, height) - height * 2.f);
        } else {
            widget->setVisible(false);
        }
    }
    m_prev_y_scroll = m_y_scroll;
}

class $modify(SongSearchCustomSongLayer, CustomSongLayer) {
    void onSongBrowser(CCObject* sender) {
        // Settings live in Geode's registry, so a disabled mod is a silent
        // pass-through to the original browser.
        if (Mod::get()->getSettingValue<bool>("disable-mod")) {
            CustomSongLayer::onSongBrowser(sender);
            return;
        }
        (void)sender;
        // Refresh NONG data before the dialog opens: our rows are built from
        // the cache, and GJSongBrowser's widgets are the only place Jukebox
        // has drawn discs at this point.
        harvestSceneWidgets();
        CustomSongSearchLayer::create(this)->show();
    }
};

// Spy hook: Jukebox draws its own NONG disc on CustomSongWidget and ships no
// public API, so harvest the icon and metadata it already resolved and
// repaint them in our rows. Only active while Jukebox is installed; without
// it this hook is a cheap no-op.
class $modify(SongSearchJukeboxSpy, CustomSongWidget) {
    bool init(SongInfoObject* song, CustomSongDelegate* delegate, bool showSelect,
              bool showPlay, bool showDownload, bool isRobtopSong, bool unk1,
              bool isMusicLibrary, int unk2) {
        if (!CustomSongWidget::init(song, delegate, showSelect, showPlay, showDownload,
                                    isRobtopSong, unk1, isMusicLibrary, unk2)) {
            return false;
        }
        // Hook order between mods is not guaranteed, so defer harvesting until
        // Jukebox has laid out its disc whether its hook ran before or after
        // ours. One frame is plenty.
        this->scheduleOnce(schedule_selector(SongSearchJukeboxSpy::delayedHarvest), 0.05f);
        return true;
    }

    void delayedHarvest(float) {
        if (!jukeboxPresent()) return;
        if (!Mod::get()->getSettingValue<bool>("nong-symbols")) return;
        harvestJukeboxWidget(this, this->m_songInfoObject);
    }
};
