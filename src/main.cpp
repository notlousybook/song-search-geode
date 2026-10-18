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
// Rows sit this much lower than the raw list height math would put them, so
// the top row's corner disc stays clear of the search bar above (which now
// draws over the rows) instead of tucking under it at rest.
constexpr float list_top_pad = 12.f;

// Jukebox draws a small pin disc on every CustomSongWidget it manages - it is
// the entry point to its nong picker, not a "song has a nong" badge. Verified
// against Jukebox 3.8.0's source: setupJBSW() -> createSongLabels() ->
// addPopupOpener() adds the disc unconditionally, and nong data only hits
// disk (manifest/<songID>.json) once one is actually added or activated.
// Our rows mirror that look with our own copy of the texture.
static bool jukeboxPresent() {
    return Loader::get()->getLoadedMod("fleym.nongd") != nullptr;
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
    // Seconds since the current preview started; used together with a
    // channel-playing poll to flip the button back when the track ends.
    float m_preview_elapsed = 0.f;

    // Song the editor currently has set, mirrored on the layer so every
    // "Use/In Use" paint reads one source. Seeded on open, set optimistically
    // when a row is clicked, and corrected by the 30fps tick only when the
    // level's own value actually moves (an external change).
    int m_ui_song_id = 0;
    bool m_ui_song_known = false;
    // Last value observed from the editor's level. The 30fps sync repaints
    // only when this actually moves (an external change) - an optimistic row
    // click must not be undone just because the level write behind it hasn't
    // landed yet.
    int m_last_level_song_id = 0;
    bool m_last_level_song_known = false;
    static constexpr float total_height_const = widget_height * (n_widgets - 1);
    // X center the whole column is laid out on (screen center, or the parent
    // dialog's content center when that reads sane - resolved in init).
    float m_content_cx = 0.f;

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
        m_preview_elapsed = 0.f;
    }

    bool init(CustomSongLayer* parent) {
        // NB: FLAlertLayer::init(int opacity) is the Windows-bound init
        // (CCLayerColor::initWithColor has no win binding in 2.2081).
        if (!FLAlertLayer::init(150)) return false;
        m_mainLayer = CCLayer::create();
        addChild(m_mainLayer);

        m_parent = parent;

        auto win_size = CCDirector::sharedDirector()->getWinSize();

        // The vanilla CustomSongLayer dialog behind us draws a frame our UI
        // lives inside; its content is not guaranteed to hug the screen
        // center. Center our column on the parent's content area when that
        // reads sane, and fall back to the plain screen center otherwise.
        m_content_cx = win_size.width / 2.f;
        if (m_parent && m_parent->m_mainLayer) {
            const auto frame_size = m_parent->m_mainLayer->getContentSize();
            const float frame_cx = m_parent->m_mainLayer->getPositionX();
            if (frame_size.width >= 360.f && frame_size.width <= 560.f &&
                frame_cx >= win_size.width / 2.f - 48.f &&
                frame_cx <= win_size.width / 2.f + 48.f) {
                m_content_cx = frame_cx;
            }
            log::info("SongSearch: parent frame {}x{} at x={} -> content cx={}",
                frame_size.width, frame_size.height, frame_cx, m_content_cx);
        }

        this->registerWithTouchDispatcher();
        this->incrementForcePrio();

        m_div = CCNode::create();
        m_div->setPosition({m_content_cx, 0});

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
        // +12 units of headroom: the top row's corner nong disc pokes above
        // its row and would otherwise be clipped flat at the list's top edge.
        stencil->setScaleY((total_height + 12.f) / stencil->getContentSize().height);
        stencil->setAnchorPoint({0, 0});

        auto clip = CCClippingNode::create(stencil);
        clip->addChild(m_div);
        clip->setAlphaThreshold(0.1f);
        // Draws under every other UI node (search bar, close button, label):
        // rows scroll behind the search bar instead of riding over its box.
        clip->setZOrder(-5);
        m_mainLayer->addChild(clip);

        m_input = CCTextInputNode::create(200.f, 50.f, "Song Search", "bigFont.fnt");
        m_input->setPosition({m_content_cx, total_height + 28.51f});
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
        m_total_songs_label->setAnchorPoint({1, 1});
        m_total_songs_label->setScale(0.5f);
        // Top-right of OUR content column, right-aligned with the search bar.
        // The old spot was the raw screen corner, outside the CustomSongLayer
        // frame the whole search UI sits in.
        m_total_songs_label->setPosition({m_content_cx + 194.f, total_height + 64.f});
        m_mainLayer->addChild(m_total_songs_label);

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
        delete_menu->setPosition({m_content_cx, 0});
        delete_menu->addChild(m_delete_button);
        clip->addChild(delete_menu);

        schedule(schedule_selector(CustomSongSearchLayer::check_mouse_movement), 1.f/30.f); // 30fps cuz ynot
        // Seed the in-use state so the first paint uses the real level value
        syncLevelSongID();
        // Eased scrolling runs from update(); setKeypadEnabled etc. already
        // gate input, this only drives the scroll interpolation.
        this->scheduleUpdate();

        do_search();
        return true;
    }

    // Also keeps the "in use" state live: the song can be changed by the
    // original browser, the editor, or another mod, so the rows are re-painted
    // whenever the level's songID is observed to change.
    void check_mouse_movement(float) {
#ifdef GEODE_IS_WINDOWS
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
#endif

        syncLevelSongID();
        refreshRowText();
    }

    // Pulls the song the editor has set and repaints the rows when it moved.
    // A repaint only happens when the level's own value CHANGES; comparing it
    // against m_ui_song_id instead would undo an optimistic row click before
    // the level write behind it has landed. While no editor is open the value
    // stays unreadable and the last known state is kept.
    void syncLevelSongID() {
        int levelSongID = 0;
        if (auto editorUI = EditorUI::get()) {
            if (editorUI->m_editorLayer &&
                editorUI->m_editorLayer->m_levelSettings &&
                editorUI->m_editorLayer->m_levelSettings->m_level) {
                levelSongID = editorUI->m_editorLayer->m_levelSettings->m_level->m_songID;
            } else {
                return;
            }
        } else {
            return;
        }
        if (m_last_level_song_known && levelSongID == m_last_level_song_id) return;
        m_last_level_song_known = true;
        m_last_level_song_id = levelSongID;
        if (!m_ui_song_known || levelSongID != m_ui_song_id) {
            m_ui_song_known = true;
            m_ui_song_id = levelSongID;
            refreshSelectButtons();
        }
    }

    void move_delete_btn() {
#ifndef GEODE_IS_WINDOWS
        // Row hover needs the mouse position, which the GD cocos builds for
        // macOS and mobile don't expose; without it there's no way to tell
        // which row is pointed at, so the delete button stays hidden.
        m_delete_button->setVisible(false);
        m_delete_button->setEnabled(false);
        return;
#endif
        constexpr float height = widget_height + widget_spacing;
        constexpr float clip_height = widget_height * (n_widgets - 1);
        // Mouse -> row index, compensated for the rows' list_top_pad offset:
        // a row's on-screen spot is "mouse + pad" in the original scheme.
        int i = static_cast<int>((clip_height - m_prev_mouse_pos.y - list_top_pad + m_y_scroll) / height);
        bool e = (m_prev_mouse_pos.y < clip_height - list_top_pad) && (i >= 0 && i < static_cast<int>(m_sorted.size()));
        if (e) {
            m_delete_button->setPositionY((n_widgets - i) * height - height * 2.f + height / 2.f - 13.f + m_y_scroll - list_top_pad);
            m_delete_button->setTag(i);
        }
        m_delete_button->setVisible(e);
        m_delete_button->setEnabled(e);
    }

    void textChanged(CCTextInputNode* input) override {
        (void)input;
        do_search();
    }

    void do_search() {
        if (m_downloaded_songs.empty()) return;
        std::vector<std::pair<int, SongInfoObject*>> songs;
        songs.reserve(m_downloaded_songs.size());
        auto input_value = m_input->getString();
        const bool empty_input = input_value.empty();
        for (const auto& [index_str, song] : m_downloaded_songs) {
            int score = 0;
            if (!empty_input && input_value != "." && !fts::fuzzy_match(input_value.c_str(), index_str.c_str(), score)) continue;
            songs.push_back({score, song});
        }
        if (!empty_input && songs.size() > 1) {
            std::sort(songs.begin(), songs.end(), [&](const auto& a, const auto& b) {
                return a.first > b.first;
            });
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

    void refreshSelectButtons();

    // Repaints rows whose song text was changed in place (Jukebox nong picks).
    void refreshRowText();

    // Ends a preview that finished on its own and flips the rows back; needs
    // SimpleSongWidget complete, so the body is out-of-line.
    void checkPreviewEnd(float dt);

    bool isSongInUse(int songID) const { return m_ui_song_known && m_ui_song_id == songID; }

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
        checkPreviewEnd(dt);
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
    // Name/artist as last painted. The periodic row sweep compares the bound
    // song's live strings against these, so Jukebox mutating the shared
    // object (a picked nong swaps the name/artist in place) repaints the row
    // without waiting for a scroll rebind.
    std::string m_painted_name;
    std::string m_painted_artist;
    CCLabelBMFont* m_name_label = nullptr;
    CCLabelBMFont* m_artist_label = nullptr;
    CCMenuItemSpriteExtra* m_nong_item = nullptr;
    // Widest the name/artist text may get before it would slide under the
    // buttons; measured off the real button layout in init().
    float m_text_max_width = 250.f;
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
        // Text block hugs the row's left edge and reads like the classic song
        // browser: name just above the middle of the row, artist just below
        // it. NOTE: widget space is centered (y in -25..+25, top edge = +25),
        // so these offsets are relative to the row's middle. Kept clear of the
        // corner pin disc (which only occupies the top-left corner).
        m_name_label = CCLabelBMFont::create("?", "bigFont.fnt");
        m_name_label->setAnchorPoint({0, 0.5f});
        m_name_label->setPosition({-width / 2.f + 12.5f, height / 2.f - 15.f});
        addChild(m_name_label);
        m_artist_label = CCLabelBMFont::create("who", "goldFont.fnt");
        m_artist_label->setAnchorPoint({0, 0.5f});
        m_artist_label->setPosition({-width / 2.f + 12.5f, height / 2.f - 32.f});
        m_artist_label->setScale(0.55f);
        addChild(m_artist_label);
        auto menu = CCMenu::create();
        addChild(menu);
        // NONG pin disc, pinned on the row's top-left corner the way Jukebox
        // pins it on its own song widgets - half the disc rides outside the
        // frame. Clicking it opens Jukebox's nong picker for this song.
        // Drawn from our own copy of the texture, so no cross-mod spritesheet
        // frame/load race can ever magenta it out. The scale is derived from
        // the packed frame size (sd/hd/uhd sheets differ 4x) so it always
        // lands at ~22 design units.
        if (auto frame = CCSpriteFrameCache::get()->spriteFrameByName("nong_disc.png"_spr)) {
            auto disc = CCSprite::createWithSpriteFrame(frame);
            const float frame_height = disc->getContentSize().height;
            disc->setScale(frame_height > 1.f ? 22.f / frame_height : 0.35f);
            m_nong_item = CCMenuItemExt::createSpriteExtra(disc, [this](CCMenuItemSpriteExtra*) {
                this->on_nong();
            });
            if (m_nong_item) {
                m_nong_item->setID("nong-symbol"_spr);
                // Row-local target: the row's top-left corner. The menu sits
                // at y=+1, so the -1 lands the disc dead on the corner.
                m_nong_item->setPosition({-width / 2.f, height / 2.f - 1.f});
                menu->addChild(m_nong_item);
            }
        }
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
        // How far text may reach before it would slide under the buttons:
        // the select button's real left edge, minus the label origin, minus
        // a small breathing gap. Measured here so a sheet-size change can't
        // silently reintroduce the overlap.
        const float buttons_left = m_select_button->getPositionX() - m_select_button->getScaledContentSize().width / 2.f;
        m_text_max_width = buttons_left - m_name_label->getPositionX() - 6.f;
        return true;
    }

    // Swaps the frame on a menu item's existing sprite. Rebuilding the sprite
    // through setSprite() here could leave the previous one stacked
    // underneath; setDisplayFrame just re-points the one sprite.
    static void setButtonFrame(CCMenuItemSpriteExtra* button, const char* frame_name) {
        if (!button) return;
        auto frame = CCSpriteFrameCache::get()->spriteFrameByName(frame_name);
        if (!frame) return;
        if (auto sprite = typeinfo_cast<CCSprite*>(button->getNormalImage())) {
            sprite->setDisplayFrame(frame);
            return;
        }
        button->setSprite(CCSprite::createWithSpriteFrame(frame));
        button->updateSprite();
    }

    // Swaps the select button between "Use" and "In Use" for whichever song the
    // level currently has set, mirroring CustomSongWidget::songStateChanged.
    // The layer owns the level's song state - reading EditorUI per-row went
    // stale while the delegate's write was still in flight.
    void update_select_btn() {
        if (!m_song || !m_select_button) return;
        const bool selected = m_parent && m_parent->isSongInUse(m_song->m_songID);
        m_select_button->setEnabled(!selected);
        setButtonFrame(m_select_button,
            selected ? "GJ_selectSongOnBtn_001.png" : "GJ_selectSongBtn_001.png");
    }

    // Fits a string label into `max_width`: scaled down from `base_scale` at
    // most, never below `min_scale`. Shrinking is the primary mechanism, so
    // any normal song title lands whole; the "..." cut only survives as a
    // last resort for absurd names that overflow even at `min_scale`, and
    // the cut point is whitespace-trimmed so it can't read "STRIKES ...".
    // The cut point is binary-searched, so this costs ~log2(len) rebuilds.
    static void fitLabel(CCLabelBMFont* label, const std::string& text,
                         float base_scale, float max_width, float min_scale) {
        if (!label) return;
        label->setString(text.c_str(), true);
        const float content_width = label->getContentSize().width;
        if (content_width <= 0.f || max_width <= 0.f) {
            label->setScale(base_scale);
            return;
        }
        const float scale = (std::min)(base_scale, max_width / content_width);
        if (scale >= min_scale) {
            label->setScale(scale);
            return;
        }
        static const std::string ellipsis = "...";
        const float budget = max_width / min_scale;
        size_t lo = 0, hi = text.size();
        std::string best = ellipsis;
        while (lo <= hi) {
            const size_t mid = lo + (hi - lo) / 2;
            std::string candidate = text.substr(0, mid) + ellipsis;
            label->setString(candidate.c_str(), true);
            if (label->getContentSize().width <= budget) {
                best = std::move(candidate);
                lo = mid + 1;
            } else {
                if (mid == 0) break;
                hi = mid - 1;
            }
        }
        // Trim before the ellipsis: trailing spaces read as "STRIKES ..." and
        // a dangling UTF-8 continuation byte would be malformed; keep the
        // string clean for both the label and any log use.
        size_t cut = best.size() - ellipsis.size();
        for (;;) {
            const size_t before = cut;
            while (cut > 0 && (best[cut - 1] == ' ' || best[cut - 1] == '\t')) --cut;
            while (cut > 0 && (static_cast<unsigned char>(best[cut - 1]) & 0xC0) == 0x80) --cut;
            if (cut == before) break;
        }
        if (cut < best.size() - ellipsis.size()) best = best.substr(0, cut) + ellipsis;
        label->setString(best.c_str(), true);
        label->setScale(min_scale);
    }

    void set_song(SongInfoObject* song) {
        m_song = song;
        m_painted_artist = song->m_artistName.c_str();
        m_painted_name = song->m_songName.c_str();
        // NB: Geode 5.11's CCLabelBMFont::setString takes (const char*, bool).
        fitLabel(m_artist_label, m_painted_artist, 0.55f, m_text_max_width, 0.35f);
        fitLabel(m_name_label, m_painted_name, 0.55f, m_text_max_width, 0.3f);
        update_nong_icon();
        update_select_btn();
        update_play_btn();
    }

    // Repaints the row when the bound song's text changed underneath us.
    // Jukebox's nong picker rewrites the shared SongInfoObject in place, so
    // this is what lets the row show the new name/artist the moment the pick
    // commits - no rebind, and no hook into Jukebox's own layer needed.
    void refresh_song_text() {
        if (!m_song) return;
        std::string name(m_song->m_songName.c_str());
        std::string artist(m_song->m_artistName.c_str());
        if (name == m_painted_name && artist == m_painted_artist) return;
        m_painted_name = std::move(name);
        m_painted_artist = std::move(artist);
        fitLabel(m_artist_label, m_painted_artist, 0.55f, m_text_max_width, 0.35f);
        fitLabel(m_name_label, m_painted_name, 0.55f, m_text_max_width, 0.3f);
    }

    // The disc mirrors Jukebox's own per-widget button, so it shows on every
    // row whenever Jukebox is installed and the setting is on. Without
    // Jukebox there is no picker to open, so the disc stays hidden.
    void update_nong_icon() {
        if (!m_nong_item) return;
        m_nong_item->setVisible(m_song != nullptr &&
            Mod::get()->getSettingValue<bool>("nong-symbols") && jukeboxPresent());
    }

    void update_play_btn() {
        if (!m_song || !m_parent) return;
        auto song_path = MusicDownloadManager::sharedState()->pathForSong(m_song->m_songID);
        setButtonFrame(m_play_button,
            m_parent->isPreviewing(song_path) ? "GJ_stopMusicBtn_001.png" : "GJ_playMusicBtn_001.png");
    }

    static CCNode* findNodeByID(CCNode* root, const std::string& id) {
        if (!root) return nullptr;
        if (root->getID() == id) return root;
        if (auto children = root->getChildren()) {
            for (auto* child : CCArrayExt<CCNode*>(children)) {
                if (auto* found = findNodeByID(child, id)) return found;
            }
        }
        return nullptr;
    }

    // Jukebox exposes no API for other mods, but every CustomSongWidget it
    // manages carries a disc whose callback opens its nong picker for that
    // widget's song. So: build a throwaway widget for this row's song, find
    // the disc Jukebox adds to it, and activate that. The picker keeps the
    // widget alive through a strong ref while it's shown, which is exactly
    // the lifetime the throwaway widget needs - no manual delete, ever.
    void on_nong() {
        if (!m_song) return;
        auto jb = Loader::get()->getLoadedMod("fleym.nongd");
        if (!jb) return;
        auto widget = CustomSongWidget::create(m_song, nullptr,
            false, false, false, false, false, false, 0);
        if (!widget) {
            log::warn("SongSearch: nong widget creation failed");
            return;
        }
        auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(
            findNodeByID(widget, jb->getID() + "/nong-button"));
        if (!btn) {
            // The mod-id prefix is what _spr() expands to; probe the bare
            // node id too in case that ever changes.
            btn = typeinfo_cast<CCMenuItemSpriteExtra*>(findNodeByID(widget, "nong-button"));
        }
        if (!btn) {
            log::warn("SongSearch: jukebox nong button not found");
            return;
        }
        log::info("SongSearch: opening jukebox nong picker for song {}", m_song->m_songID);
        btn->activate();
    }

    void on_select(CCObject* obj) {
        (void)obj;
        if (!m_parent || !m_parent->m_parent || !m_song) return;
        auto layer = m_parent->m_parent;
        const int songID = m_song->m_songID;
        // The 2.1 mod applied the pick by writing the level's song directly
        // (m_parent->m_levelSettings->m_level->songID). 2.2's CustomSongLayer
        // dropped m_levelSettings; its delegate exposes it via
        // getLevelSettings(). Keep the delegate notified like
        // CustomSongWidget::onSelect does, but never rely on it alone - it
        // only records pending state, which is why the row snapped back.
        LevelSettingsObject* settings = nullptr;
        if (auto delegate = layer->m_songDelegate) {
            settings = delegate->getLevelSettings();
            delegate->songIDChanged(songID);
        }
        if (!settings) {
            if (auto editorUI = EditorUI::get()) {
                if (editorUI->m_editorLayer) {
                    settings = editorUI->m_editorLayer->m_levelSettings;
                }
            }
        }
        if (settings && settings->m_level) {
            settings->m_level->m_songID = songID;
        }
        log::info("SongSearch: select {} (delegate {}, settings {})", songID,
            layer->m_songDelegate != nullptr, settings != nullptr);
        MusicDownloadManager::sharedState()->songStateChanged();
        if (layer->m_songWidget) {
            layer->m_songWidget->updateSongObject(m_song);
        }
        // Paint the click's outcome immediately; the sync above only
        // overrides this when the level's value moves somewhere else.
        m_parent->m_ui_song_known = true;
        m_parent->m_ui_song_id = songID;
        m_parent->refreshSelectButtons();
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

// Defined out-of-line because SimpleSongWidget is only forward-declared where
// the class is written.
void CustomSongSearchLayer::refreshSelectButtons() {
    for (auto& widget : m_scroll_elements) {
        if (widget) widget->update_select_btn();
    }
}

// Jukebox rewrites the shared song's name/artist in place when a nong is
// picked or cleared, so rows would otherwise keep stale text until the next
// scroll rebind. A sweep of the six bound rows is a handful of string
// compares per tick; only rows whose text actually moved are rebuilt.
void CustomSongSearchLayer::refreshRowText() {
    for (auto& widget : m_scroll_elements) {
        if (widget) widget->refresh_song_text();
    }
}

// Same reason as refreshSelectButtons: touching row members needs the complete
// type. A preview ends on its own when the file runs out (playMusic is called
// with shouldLoop false), and 2.2081 removed isBackgroundMusicPlaying - so
// poll the channel we play on directly. The grace period avoids racing the
// engine's start.
void CustomSongSearchLayer::checkPreviewEnd(float dt) {
    if (!m_previewing) return;
    m_preview_elapsed += dt;
    if (m_preview_elapsed < 0.5f) return;
    if (FMODAudioEngine::sharedEngine()->isMusicPlaying(0)) return;
    stopPreview();
    for (auto& widget : m_scroll_elements) {
        if (widget) widget->update_play_btn();
    }
}

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
            widget->setPositionY((n_widgets - i) * height + ::fmod(m_y_scroll, height) - height * 2.f - list_top_pad);
            // Rows lower on screen draw over rows higher up, so each row's
            // corner disc stays on top of the row above it even after
            // scrolling has recycled the row nodes.
            if (widget->getZOrder() != static_cast<int>(i)) {
                widget->setZOrder(static_cast<int>(i));
            }
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
        CustomSongSearchLayer::create(this)->show();
    }
};
