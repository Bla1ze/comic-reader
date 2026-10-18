#include "App.h"

#include "AppConfig.h"
#include "AppFont.h"
#include "Covers.h"
#include "CpuPin.h"
#include "DisplayProfile.h"
#include "Gfx.h"
#include "Library.h"
#include "Lookup.h"
#include "Pages.h"
#include "Panels.h"
#include "Theme.h"
#include "Transfer.h"
#include "Update.h"
#include "Version.h"
#include "controls/Controls.h"

#include <SDL.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The playfield is a portrait canvas (720 x 1280 logical, drawn at the panel's
// own resolution: 3x on a 4KP) turned onto the landscape framebuffer, as in
// Retro Launcher. The library and menus draw in logical units; pages are drawn
// 1:1 in canvas pixels, so a 4KP shows them at the full 2160 x 3840.

namespace {

using Library::log;
std::vector<std::string> noteLines(SDL_Renderer* r, const std::string& md, float width, float size);
using CE = AtGames::ControlEvent;

constexpr int kW = AppConfig::kLogicalWidth;
constexpr int kH = AppConfig::kLogicalHeight;

enum Tab { kContinue, kComics, kManuals, kSettings, kTabCount };
const char* const kTabNames[kTabCount] = {"Continue", "Comics", "Manuals", "Settings"};

// Settings rows.
enum SetRow { kRowUpload, kRowUpdates, kRowTurn, kRowDirection, kRowForget, kRowCovers, kRowAbout, kSetRows };
enum class SetView { List, Upload, Updates, About };
enum class Confirm { None, Exit, Forget };

// The library grid.
constexpr int kCols = 3;
constexpr float kSide = 36.0f, kGap = 24.0f;
constexpr float kCellW = (kW - 2 * kSide - (kCols - 1) * kGap) / kCols;  // 200
constexpr float kCoverH = kCellW * 1.5f;                                  // 300
constexpr float kCellH = kCoverH + 86.0f;
constexpr float kRowStride = kCellH + 22.0f;
constexpr float kTabsTop = 150.0f;
constexpr float kGridTop = 290.0f;
constexpr float kGridBottom = Theme::kFooterTop - 8.0f;

constexpr size_t kMaxCovers = 90;
constexpr float kHoldDelay = 0.45f, kHoldRepeat = 0.16f;
constexpr float kSlideTime = 0.24f, kCurlTime = 0.42f, kHudTime = 2.2f;
constexpr float kPanSpeed = 900.0f;  // logical px a second at full stick

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

struct Tex {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    bool failed = false;
    std::string error;
    unsigned long long used = 0;
};

class App {
public:
    explicit App(const std::string& appDir) : m_appDir(appDir) {}
    ~App();
    int run();

private:
    bool initVideo();
    void present();
    void handle(CE ce);
    void pollHeld(float dt);
    void update(float dt);
    void render();

    // Library
    void setTab(int tab);
    void reload();
    void openSelected();
    void back();
    std::string shelfDir() const { return m_tab == kManuals ? "manuals" : "comics"; }
    void moveSel(int d);
    void renderLibrary();
    void renderTabs();
    void renderGrid();
    void renderUpload();
    void renderSettings();
    void renderUpdates();
    void renderAbout();
    void settingsInput(CE ce);
    void askConfirm(Confirm kind) { m_confirm = true; m_confirmKind = kind; m_confirmSel = 0; }

public:
    bool restart() const { return m_restartIn == 0.0f; }

private:
    Tex* cover(const std::string& rel);
    void pumpCovers();

    // Reader
    void openBook(const Library::Entry& e);
    void closeBook();
    void turn(int dir);
    void pumpPages();
    void renderReader();
    void curl(const Tex& top, const Tex& under, float t, bool fromLeft);
    void saveProgress(bool force);
    void setZoom(float z);
    void clampView();
    void pan(float dx, float dy);
    void askDetail();
    void dropDetail();
    bool fitSize(int& w, int& h) const;

    void updatePanels();
    void toast(const std::string& s) { m_toast = s; m_toastTime = 2.6f; }
    // A book's name to show: its ComicInfo series and number (or title) if it has them.
    std::string nameOf(const Library::Entry& e) const {
        if (e.series) return e.title;
        auto it = m_info.find(e.rel);
        const std::string d = it == m_info.end() ? "" : it->second.display();
        return d.empty() ? e.title : d;
    }
    void renderDetails();

    std::string m_appDir;
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_canvas = nullptr;
    int m_canvasW = kW, m_canvasH = kH;
    float m_canvasScale = 1.0f;
    DisplayProfile::Topology m_topo;
    AtGames::Controls m_controls;
    std::unique_ptr<Panels> m_panels;
    Library::ProgressStore m_progress;
    CoverLoader m_covers;
    PageRenderer m_pages;
    TransferServer m_transfer;
    unsigned long long m_frame = 0;
    float m_clock = 0.0f;
    bool m_quit = false;

    // Library state
    int m_tab = kComics;
    std::string m_dir;  // the folder shown on Comics / Manuals ("comics" or "comics/<series>")
    std::vector<Library::Entry> m_items;
    int m_sel = 0;
    std::vector<int> m_selStack;
    float m_scroll = 0.0f, m_scrollShown = 0.0f;
    std::map<std::string, Tex> m_coverTex;
    std::map<std::string, Library::BookInfo> m_info;  // ComicInfo details, as covers load
    bool m_details = false;                            // the details card is open
    Lookup m_lookup;                                   // Look up details (X on the card)
    bool m_lookupOn = false;                           // the card shows the lookup
    bool m_confirm = false;
    int m_confirmSel = 0;
    Confirm m_confirmKind = Confirm::Exit;
    // Settings
    Library::Settings m_settings;
    Updater m_updater;
    SetView m_setView = SetView::List;
    int m_setSel = 0;
    bool m_rtl = false;            // Settings > Reading direction: right to left (manga)
    bool m_readRtl = false;        // the open book reads right to left (the setting, or its ComicInfo)
    std::string m_turnStyle;       // "curl", "slide" or "none"
    float m_restartIn = -1.0f;     // after an update: seconds to the restart
    std::string m_known;           // newest version the last check found ("" if none)
    Updater::State m_seenState = Updater::State::Idle;
    int m_uploadSeen = 0;

    // Reader state
    bool m_reading = false;
    Library::Entry m_book;
    std::string m_bookSubtitle;
    int m_gen = 0, m_page = 0, m_pagesN = 0;
    std::map<int, Tex> m_pageTex;
    std::set<int> m_told;  // the held set last given to the renderer
    int m_toldPage = -1;
    int m_slideFrom = -1, m_slideDir = 0;
    float m_slideT = 1.0f;
    float m_hud = 0.0f;
    // Zoom: 1 = the page fitted. The view is the top-left of the screen in the
    // zoomed page's pixels; a sharp "detail" of the part around it is drawn on
    // request and laid over the enlarged page.
    float m_zoom = 1.0f;
    float m_vx = 0.0f, m_vy = 0.0f;
    Tex m_detail;
    int m_detailPage = -1, m_detailX = 0, m_detailY = 0;
    float m_detailZoom = 0.0f;
    SDL_Rect m_asked{0, 0, 0, 0};
    int m_askedPage = -1;
    float m_askedZoom = 0.0f;
    bool m_progressDirty = false;
    float m_progressAge = 0.0f;

    // Held buttons (hold to flip / to move).
    int m_holdDir = 0;  // reader: -1 / +1; library: arrows as -1/+1/-3/+3
    float m_holdTime = 0.0f, m_holdAcc = 0.0f;

    std::string m_toast;
    float m_toastTime = 0.0f;
};

App::~App() {
    m_transfer.stop();
    for (auto& kv : m_coverTex)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    for (auto& kv : m_pageTex)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    m_panels.reset();
    Theme::shutdown();
    Gfx::shutdown();
    if (m_canvas) SDL_DestroyTexture(m_canvas);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

bool App::initVideo() {
    m_topo = DisplayProfile::detect();
    log("model '%s' %s: main %u, backglass %u, dmd %u", m_topo.model.c_str(), m_topo.known ? "known" : "unknown",
        m_topo.main.connectorId, m_topo.backglass.connectorId, m_topo.dmd.connectorId);
    ::unsetenv("SDL2_DISPLAY_PLANE_TYPE");
    ::unsetenv("ForceConnectID");
    DisplayProfile::prepareSdlMain(m_topo);
    // Never let an SDL assertion exit the app (its default handler does).
    SDL_SetAssertionHandler([](const SDL_AssertData* d, void*) -> SDL_AssertState {
        if (d && d->trigger_count == 0) log("SDL assertion '%s' at %s:%d - ignored", d->condition, d->filename, d->linenum);
        return SDL_ASSERTION_IGNORE;
    }, nullptr);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    m_window = SDL_CreateWindow("Comic Reader", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, AppConfig::kFramebufferWidth,
                                AppConfig::kFramebufferHeight, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!m_window) { log("window: %s", SDL_GetError()); return false; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!m_renderer) m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    if (!m_renderer) { log("renderer: %s", SDL_GetError()); return false; }

    int ow = AppConfig::kFramebufferWidth, oh = AppConfig::kFramebufferHeight;
    SDL_GetRendererOutputSize(m_renderer, &ow, &oh);
    m_canvasScale = std::max(1.0f, std::min(ow / (float)kH, oh / (float)kW));
    m_canvasW = (int)std::lround(kW * m_canvasScale);
    m_canvasH = (int)std::lround(kH * m_canvasScale);
    m_canvas = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_canvasW, m_canvasH);
    if (!m_canvas) {
        m_canvasScale = 1.0f;
        m_canvasW = kW;
        m_canvasH = kH;
        m_canvas = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, m_canvasW, m_canvasH);
        if (!m_canvas) { log("canvas: %s", SDL_GetError()); return false; }
    }
    SDL_SetTextureBlendMode(m_canvas, SDL_BLENDMODE_NONE);
    SDL_RendererInfo info;
    SDL_GetRendererInfo(m_renderer, &info);
    log("output %dx%d, canvas %dx%d (scale %.2f), renderer %s, max texture %dx%d", ow, oh, m_canvasW, m_canvasH, m_canvasScale,
        info.name ? info.name : "?", info.max_texture_width, info.max_texture_height);
    Library::logInputDevices();
    Gfx::init(m_renderer, m_canvasScale);
    Theme::init(m_renderer, m_canvasScale);
    m_controls.open();
    m_panels.reset(new Panels());
    log("panels: %d up", m_panels->init(m_topo, m_appDir));
    return true;
}

int App::run() {
    if (!initVideo()) return 1;
    log("cpus: fast %s, slow %s", CpuPin::list(CpuPin::groups().fast).c_str(), CpuPin::list(CpuPin::groups().slow).c_str());
    m_progress.load(m_appDir);
    m_settings.load(m_appDir);
    m_rtl = m_settings.get("direction", "ltr") == "rtl";
    m_turnStyle = m_settings.get("turn", "curl");
    m_known = Updater::knownLatest(m_appDir);
    // A quiet check for a newer version, at most once an hour.
    if (Updater::startupCheckDue(m_appDir)) m_updater.check(m_appDir, true);
    m_covers.start(m_appDir);
    m_pages.start();
    m_dir = "comics";
    // Start on Continue when there is something to continue.
    setTab(m_progress.continueReading(1).empty() ? kComics : kContinue);

    Uint32 last = SDL_GetTicks();
    while (!m_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) m_quit = true;
            else if (ev.type == SDL_CONTROLLERDEVICEADDED) m_controls.open();
            CE ce = m_controls.event(ev, true, false);
            if (ce != CE::None) handle(ce);
        }
        Uint32 now = SDL_GetTicks();
        float dt = std::min(0.1f, (now - last) / 1000.0f);
        last = now;
        ++m_frame;
        m_clock += dt;
        pollHeld(dt);
        update(dt);
        render();
        present();
    }
    saveProgress(true);
    m_transfer.stop();
    log("exit");
    return 0;
}

void App::present() {
    int ow = AppConfig::kFramebufferWidth, oh = AppConfig::kFramebufferHeight;
    SDL_GetRendererOutputSize(m_renderer, &ow, &oh);
    float scale = std::min(ow / (float)kH, oh / (float)kW);
    int cw = (int)std::lround(m_canvasW * scale / m_canvasScale), ch = (int)std::lround(m_canvasH * scale / m_canvasScale);
    SDL_Rect dst{ow / 2 - cw / 2, oh / 2 - ch / 2, cw, ch};
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderCopyEx(m_renderer, m_canvas, nullptr, &dst, AppConfig::kFirmwareRotationDegrees, nullptr, SDL_FLIP_NONE);
    SDL_RenderPresent(m_renderer);
}

// ------------------------------------------------------------------ input

void App::handle(CE ce) {
    if (ce != CE::None) log("input: %d", (int)ce);
    if (m_confirm) {
        if (ce == CE::DpadUp || ce == CE::DpadDown) m_confirmSel ^= 1;
        else if (ce == CE::A || ce == CE::Start) {
            if (m_confirmSel == 1) {
                if (m_confirmKind == Confirm::Exit) m_quit = true;
                else if (m_confirmKind == Confirm::Forget) {
                    m_progress.clear();
                    log("settings: reading history forgotten");
                    toast("Reading history forgotten");
                }
            }
            m_confirm = false;
        } else if (ce == CE::B || ce == CE::Back || ce == CE::Guide) m_confirm = false;
        return;
    }
    if (m_reading) {
        switch (ce) {
        // Right turns forward, left back; right to left (manga) swaps them. A is always forward.
        case CE::A: turn(+1); break;
        case CE::RightShoulder: case CE::RightTrigger: turn(m_readRtl ? -1 : +1); break;
        case CE::LeftShoulder: case CE::LeftTrigger: case CE::X: turn(m_readRtl ? +1 : -1); break;
        // The joystick pans when zoomed (pollHeld); otherwise it turns or shows the page number.
        case CE::DpadRight: if (m_zoom <= 1.0f) turn(m_readRtl ? -1 : +1); break;
        case CE::DpadLeft: if (m_zoom <= 1.0f) turn(m_readRtl ? +1 : -1); break;
        case CE::DpadUp: case CE::DpadDown: if (m_zoom <= 1.0f) m_hud = m_hud > 0.5f ? 0.0f : kHudTime; break;
        case CE::Start: case CE::Y: setZoom(m_zoom >= 3.0f ? 1.0f : m_zoom + 1.0f); break;
        case CE::B: case CE::Back: case CE::Guide:
            if (m_zoom > 1.0f) setZoom(1.0f);
            else closeBook();
            break;
        default: break;
        }
        return;
    }
    if (m_details) {
        const Library::Entry& e = m_items[(size_t)m_sel];
        if (m_lookupOn) {
            const Lookup::State ls = m_lookup.state();
            if (ce == CE::B || ce == CE::Back || ce == CE::Guide) { m_lookup.stop(); m_lookupOn = false; }
            else if ((ce == CE::A || ce == CE::Start) && ls == Lookup::State::Found) {
                const Library::BookInfo found = m_lookup.result();
                Covers::saveInfo(m_appDir, e.rel, found);
                m_info[e.rel] = found;
                m_lookupOn = false;
                log("lookup: kept %s for %s", found.display().c_str(), e.rel.c_str());
                toast("Details kept");
                if (m_panels) m_panels->refresh();
            } else if (ce == CE::X && ls == Lookup::State::Found) m_lookup.next();
            else if (ce == CE::X && (ls == Lookup::State::NotFound || ls == Lookup::State::Failed)) {
                m_lookup.start(m_appDir, e.rel, e.title, Library::titleOf(e.rel.substr(0, e.rel.find_last_of('/'))));
            }
            return;
        }
        if (ce == CE::B || ce == CE::Back || ce == CE::Y || ce == CE::Guide) m_details = false;
        else if (ce == CE::A || ce == CE::Start) { m_details = false; openSelected(); }
        else if (ce == CE::X) {
            // The name from the file (e.title), not the ComicInfo name; its folder helps when the name is just a number.
            const size_t slash = e.rel.find_last_of('/'), prev = e.rel.find_last_of('/', slash - 1);
            const std::string folder = prev == std::string::npos ? "" : Library::titleOf(e.rel.substr(prev + 1, slash - prev - 1));
            m_lookup.start(m_appDir, e.rel, e.title, folder);
            m_lookupOn = true;
        }
        return;
    }
    if (ce == CE::Y && m_tab != kSettings && !m_items.empty() && !m_items[(size_t)m_sel].series) {
        m_details = true;
        return;
    }
    if (m_tab == kSettings && ce != CE::LeftShoulder && ce != CE::RightShoulder && ce != CE::LeftTrigger &&
        ce != CE::RightTrigger) {
        settingsInput(ce);
        return;
    }
    switch (ce) {
    case CE::LeftShoulder: case CE::LeftTrigger: setTab((m_tab + kTabCount - 1) % kTabCount); break;
    case CE::RightShoulder: case CE::RightTrigger: setTab((m_tab + 1) % kTabCount); break;
    case CE::DpadLeft: moveSel(-1); break;
    case CE::DpadRight: moveSel(+1); break;
    case CE::DpadUp: moveSel(-kCols); break;
    case CE::DpadDown: moveSel(+kCols); break;
    case CE::A: case CE::Start: openSelected(); break;
    case CE::B: case CE::Back: back(); break;
    case CE::Guide: askConfirm(Confirm::Exit); break;
    default: break;
    }
}

// Hold a flipper (or an arrow) to keep turning pages / moving.
void App::pollHeld(float dt) {
    bool left = false, right = false, up = false, down = false;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        SDL_GameController* gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (!gc) continue;
        left |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
        right |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
        up |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
        down |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
        if (m_reading) {
            left |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
            right |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
        }
    }
    if (m_reading && m_zoom > 1.0f) {
        // Zoomed: the arrows (and the left stick) pan; held flippers still turn pages.
        float px = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f), py = (down ? 1.0f : 0.0f) - (up ? 1.0f : 0.0f);
        bool lb = false, rb = false;
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            SDL_GameController* gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
            if (!gc) continue;
            const float ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            const float ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            if (std::fabs(ax) > 0.25f) px = ax;
            if (std::fabs(ay) > 0.25f) py = ay;
            lb |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
            rb |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
        }
        if (px != 0.0f || py != 0.0f) pan(px * kPanSpeed * m_canvasScale * dt, py * kPanSpeed * m_canvasScale * dt);
        left = lb;
        right = rb;
    }
    int dir = 0;
    if (m_reading) dir = right && !left ? 1 : left && !right ? -1 : 0;
    else if (m_tab != kSettings && !m_confirm) dir = right ? 1 : left ? -1 : down ? kCols : up ? -kCols : 0;
    if (dir != m_holdDir) {
        m_holdDir = dir;
        m_holdTime = m_holdAcc = 0.0f;
        return;
    }
    if (!dir) return;
    m_holdTime += dt;
    if (m_holdTime < kHoldDelay) return;
    m_holdAcc += dt;
    while (m_holdAcc >= kHoldRepeat) {
        m_holdAcc -= kHoldRepeat;
        if (m_reading) turn(m_readRtl ? -dir : dir);
        else moveSel(dir);
    }
}

// ------------------------------------------------------------------ library

void App::setTab(int tab) {
    m_details = false;
    if (m_lookupOn) { m_lookup.stop(); m_lookupOn = false; }
    if (m_tab == kSettings && tab != kSettings) {
        if (m_setView == SetView::Upload) {
            const int got = m_transfer.status().received;
            m_transfer.stop();
            if (got > 0) toast("Added " + std::to_string(got) + (got == 1 ? " file" : " files"));
        }
        m_setView = SetView::List;
    }
    const bool changed = tab != m_tab;
    m_tab = tab;
    if (m_tab == kSettings) {
        m_items.clear();
        return;
    }
    if (changed) {
        m_dir = shelfDir();
        m_selStack.clear();
        m_sel = 0;
        m_scroll = m_scrollShown = 0.0f;
    }
    reload();
}

void App::settingsInput(CE ce) {
    if (m_setView != SetView::List) {
        if (m_setView == SetView::Updates && (ce == CE::A || ce == CE::Start) && m_updater.state() == Updater::State::Available) {
            m_updater.install();
            return;
        }
        if (m_setView == SetView::Updates && (ce == CE::A || ce == CE::Start) &&
            (m_updater.state() == Updater::State::Failed || m_updater.state() == Updater::State::NoRelease)) {
            m_updater.check(m_appDir, false);
            return;
        }
        if (ce == CE::B || ce == CE::Back || ce == CE::Guide) {
            const Updater::State us = m_updater.state();
            if (m_setView == SetView::Updates && (us == Updater::State::Downloading || us == Updater::State::Installing)) return;
            if (m_setView == SetView::Upload) {
                const int got = m_transfer.status().received;
                m_transfer.stop();
                if (got > 0) toast("Added " + std::to_string(got) + (got == 1 ? " file" : " files"));
            }
            m_setView = SetView::List;
        }
        return;
    }
    auto cycle = [&](int d) {
        if (m_setSel == kRowTurn) {
            static const char* styles[3] = {"curl", "slide", "none"};
            int i = 0;
            while (i < 3 && m_turnStyle != styles[i]) ++i;
            m_turnStyle = styles[((i % 3) + d + 3) % 3];
            m_settings.set("turn", m_turnStyle);
        } else if (m_setSel == kRowDirection) {
            m_rtl = !m_rtl;
            m_settings.set("direction", m_rtl ? "rtl" : "ltr");
        }
    };
    switch (ce) {
    case CE::DpadUp: m_setSel = (m_setSel + kSetRows - 1) % kSetRows; break;
    case CE::DpadDown: m_setSel = (m_setSel + 1) % kSetRows; break;
    case CE::DpadLeft: cycle(-1); break;
    case CE::DpadRight: cycle(+1); break;
    case CE::A: case CE::Start:
        switch (m_setSel) {
        case kRowUpload: m_setView = SetView::Upload; m_transfer.start(m_appDir); break;
        case kRowUpdates: m_setView = SetView::Updates; m_updater.check(m_appDir, false); break;
        case kRowTurn: case kRowDirection: cycle(+1); break;
        case kRowForget: askConfirm(Confirm::Forget); break;
        case kRowCovers: {
            const int n = Library::clearCovers(m_appDir);
            for (auto& kv : m_coverTex)
                if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
            m_coverTex.clear();
            m_covers.forgetAll();
            if (m_panels) m_panels->refresh();
            log("settings: %d covers cleared", n);
            toast("Covers will be drawn again");
            break;
        }
        case kRowAbout: m_setView = SetView::About; break;
        default: break;
        }
        break;
    case CE::B: case CE::Back: case CE::Guide: askConfirm(Confirm::Exit); break;
    default: break;
    }
}

void App::reload() {
    if (m_tab == kContinue) {
        m_items.clear();
        for (const std::string& rel : m_progress.continueReading(60)) {
            if (!exists(m_appDir + "/" + rel)) continue;
            Library::Entry e;
            e.rel = e.coverRel = rel;
            e.title = Library::titleOf(rel);
            m_items.push_back(e);
        }
    } else {
        m_items = Library::list(m_appDir, m_dir);
    }
    m_sel = std::max(0, std::min(m_sel, (int)m_items.size() - 1));
}

void App::moveSel(int d) {
    if (m_items.empty()) return;
    const int n = (int)m_items.size();
    int s = m_sel + d;
    if (d == kCols || d == -kCols) {
        if (s < 0 || s >= n) {
            // Down from a short last row lands on the last item; up stops at the top.
            if (d > 0 && m_sel / kCols < (n - 1) / kCols) s = n - 1;
            else return;
        }
    }
    m_sel = std::max(0, std::min(n - 1, s));
}

void App::openSelected() {
    if (m_tab == kSettings || m_items.empty()) return;
    const Library::Entry e = m_items[(size_t)m_sel];
    if (e.series) {
        m_selStack.push_back(m_sel);
        m_dir = e.rel;
        m_sel = 0;
        m_scroll = m_scrollShown = 0.0f;
        reload();
        return;
    }
    openBook(e);
}

void App::back() {
    if (m_tab != kContinue && m_dir != shelfDir()) {
        m_dir = m_dir.substr(0, m_dir.find_last_of('/'));
        reload();
        m_sel = m_selStack.empty() ? 0 : m_selStack.back();
        if (!m_selStack.empty()) m_selStack.pop_back();
        m_sel = std::max(0, std::min(m_sel, (int)m_items.size() - 1));
        return;
    }
    askConfirm(Confirm::Exit);
}

Tex* App::cover(const std::string& rel) {
    auto it = m_coverTex.find(rel);
    if (it == m_coverTex.end()) {
        m_covers.request(rel);
        return nullptr;
    }
    it->second.used = m_frame;
    return &it->second;
}

void App::pumpCovers() {
    CoverLoader::Result r;
    int uploads = 0;
    while (uploads < 3 && m_covers.take(r)) {
        Tex t;
        t.used = m_frame;
        if (r.ok) {
            t.tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, r.w, r.h);
            if (t.tex) {
                SDL_UpdateTexture(t.tex, nullptr, r.rgba.data(), r.w * 4);
                SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
                t.w = r.w;
                t.h = r.h;
            }
            ++uploads;
        }
        t.failed = !t.tex;
        m_coverTex[r.rel] = t;
        if (r.info.known) m_info[r.rel] = r.info;
    }
    // Forget the covers used longest ago.
    while (m_coverTex.size() > kMaxCovers) {
        auto oldest = m_coverTex.begin();
        for (auto it = m_coverTex.begin(); it != m_coverTex.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        if (oldest->second.used + 2 >= m_frame) break;  // all on screen
        if (oldest->second.tex) SDL_DestroyTexture(oldest->second.tex);
        m_covers.forget(oldest->first);
        m_coverTex.erase(oldest);
    }
}

// ------------------------------------------------------------------ reader

void App::openBook(const Library::Entry& e) {
    m_reading = true;
    m_book = e;
    const size_t slash = e.rel.find('/'), last = e.rel.find_last_of('/');
    m_bookSubtitle = last != slash ? Library::titleOf(e.rel.substr(slash + 1, last - slash - 1))
                                   : (e.rel.compare(0, 7, "manuals") == 0 ? "Manuals" : "Comics");
    const Library::Progress p = m_progress.get(e.rel);
    m_page = (p.pages > 0 && p.page < p.pages - 1) ? p.page : 0;  // finished books start over
    m_pagesN = 0;
    for (auto& kv : m_pageTex)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    m_pageTex.clear();
    m_told.clear();
    m_toldPage = -1;
    m_slideT = 1.0f;
    m_slideFrom = -1;
    m_hud = kHudTime;
    m_zoom = 1.0f;
    m_vx = m_vy = 0.0f;
    dropDetail();
    {
        auto it = m_info.find(e.rel);
        m_readRtl = m_rtl || (it != m_info.end() && it->second.rtl);
    }
    m_gen = m_pages.open(m_appDir + "/" + e.rel, m_canvasW, m_canvasH);
    log("open %s at page %d", e.rel.c_str(), m_page + 1);
}

void App::closeBook() {
    saveProgress(true);
    dropDetail();
    m_zoom = 1.0f;
    m_pages.close();
    for (auto& kv : m_pageTex)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    m_pageTex.clear();
    m_reading = false;
    // The cover may now show progress; Continue may have a new book.
    if (m_tab == kContinue) reload();
}

void App::turn(int dir) {
    if (m_pagesN <= 0) return;
    const int np = std::max(0, std::min(m_pagesN - 1, m_page + dir));
    if (np == m_page) {
        toast(dir > 0 ? "Last page - B for the library" : "First page");
        return;
    }
    m_page = np;
    m_progressDirty = true;
    dropDetail();
    if (m_zoom > 1.0f) {
        // Zoomed: straight to the next page's top (left edge going forward, right going back).
        m_slideT = 1.0f;
        m_slideFrom = -1;
        m_vx = (dir > 0) != m_readRtl ? 0.0f : 1e9f;  // forward: the top corner reading starts at
        m_vy = 0.0f;
        clampView();
        return;
    }
    m_slideFrom = m_page - dir;
    m_slideDir = dir;
    m_slideT = m_turnStyle == "none" ? 1.0f : 0.0f;
}

void App::saveProgress(bool force) {
    if (!m_reading || m_pagesN <= 0) return;
    if (!m_progressDirty && !force) return;
    m_progress.set(m_book.rel, m_page, m_pagesN);
    m_progressDirty = false;
    m_progressAge = 0.0f;
}

bool App::fitSize(int& w, int& h) const {
    auto it = m_pageTex.find(m_page);
    if (it == m_pageTex.end() || !it->second.tex) return false;
    w = it->second.w;
    h = it->second.h;
    return true;
}

void App::dropDetail() {
    if (m_detail.tex) SDL_DestroyTexture(m_detail.tex);
    m_detail = Tex();
    m_detailPage = -1;
    m_askedPage = -1;
}

void App::setZoom(float z) {
    int fw, fh;
    if (z > 1.0f && !fitSize(fw, fh)) return;  // the page isn't drawn yet
    if (z == m_zoom) return;
    // Keep the point at the middle of the screen where it is.
    const float cx = m_vx + m_canvasW * 0.5f, cy = m_vy + m_canvasH * 0.5f;
    const float k = z / m_zoom;
    m_zoom = z;
    m_vx = cx * k - m_canvasW * 0.5f;
    m_vy = cy * k - m_canvasH * 0.5f;
    clampView();
    dropDetail();
    m_hud = kHudTime;
    log("zoom %.0fx on page %d", z, m_page + 1);
}

// Within the zoomed page; a side smaller than the screen is centered.
void App::clampView() {
    int fw, fh;
    if (!fitSize(fw, fh)) return;
    const float zw = fw * m_zoom, zh = fh * m_zoom;
    m_vx = zw <= m_canvasW ? (zw - m_canvasW) * 0.5f : std::max(0.0f, std::min(m_vx, zw - m_canvasW));
    m_vy = zh <= m_canvasH ? (zh - m_canvasH) * 0.5f : std::max(0.0f, std::min(m_vy, zh - m_canvasH));
}

void App::pan(float dx, float dy) {
    m_vx += dx;
    m_vy += dy;
    clampView();
}

// Asks for a sharp copy of the screen's part of the page (and a margin, so a
// little panning doesn't need another) when the one held doesn't cover it.
void App::askDetail() {
    int fw, fh;
    if (m_zoom <= 1.0f || !fitSize(fw, fh)) return;
    const int zw = (int)(fw * m_zoom), zh = (int)(fh * m_zoom);
    const SDL_Rect view{(int)std::max(0.0f, m_vx), (int)std::max(0.0f, m_vy), std::min(m_canvasW, zw), std::min(m_canvasH, zh)};
    auto covers = [&](int x, int y, int w, int h) {
        return view.x >= x && view.y >= y && view.x + view.w <= x + w && view.y + view.h <= y + h;
    };
    if (m_detail.tex && m_detailPage == m_page && m_detailZoom == m_zoom && covers(m_detailX, m_detailY, m_detail.w, m_detail.h)) return;
    if (m_askedPage == m_page && m_askedZoom == m_zoom && covers(m_asked.x, m_asked.y, m_asked.w, m_asked.h)) return;
    const int mx = m_canvasW * 15 / 100, my = m_canvasH * 15 / 100;
    const int x0 = std::max(0, view.x - mx), y0 = std::max(0, view.y - my);
    const int x1 = std::min(zw, view.x + view.w + mx), y1 = std::min(zh, view.y + view.h + my);
    m_asked = {x0, y0, x1 - x0, y1 - y0};
    m_askedPage = m_page;
    m_askedZoom = m_zoom;
    m_pages.wantDetail(m_page, m_zoom, m_asked.x, m_asked.y, m_asked.w, m_asked.h);
}

void App::pumpPages() {
    const int n = m_pages.pageCount();
    if (m_pagesN <= 0 && n > 0) {
        m_pagesN = n;
        m_page = std::min(m_page, n - 1);
        m_progressDirty = true;  // saves the page count
    }
    // One new page a frame: a 4K page is 33 MB to hand to the GPU.
    PageRenderer::Result r;
    if (m_pages.take(r) && r.detail) {
        if (r.error.empty() && r.page == m_page && r.zoom == m_zoom) {
            Tex t;
            t.tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, r.w, r.h);
            if (t.tex) {
                SDL_UpdateTexture(t.tex, nullptr, r.rgba.data(), r.w * 4);
                t.w = r.w;
                t.h = r.h;
                if (m_detail.tex) SDL_DestroyTexture(m_detail.tex);
                m_detail = t;
                m_detailPage = r.page;
                m_detailZoom = r.zoom;
                m_detailX = r.x;
                m_detailY = r.y;
            }
        } else if (!r.error.empty()) {
            log("zoom detail, page %d: %s", r.page + 1, r.error.c_str());
        }
        if (r.ms > 400) log("zoom detail %dx%d in %ld ms", r.w, r.h, r.ms);
    } else if (r.generation) {
        Tex t;
        if (r.error.empty()) {
            t.tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, r.w, r.h);
            if (t.tex) {
                SDL_UpdateTexture(t.tex, nullptr, r.rgba.data(), r.w * 4);
                // Blended (the pages are opaque), so the curl can show one faintly through paper.
                SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
                t.w = r.w;
                t.h = r.h;
            } else {
                t.error = SDL_GetError();
            }
        } else {
            t.error = r.error;
        }
        t.failed = !t.tex;
        auto old = m_pageTex.find(r.page);
        if (old != m_pageTex.end() && old->second.tex) SDL_DestroyTexture(old->second.tex);
        m_pageTex[r.page] = t;
        if (r.ms > 250) log("page %d: %dx%d in %ld ms", r.page + 1, r.w, r.h, r.ms);
        if (r.page == m_page) clampView();
    }
    askDetail();
    // Keep the pages around the one shown (and the one sliding away).
    for (auto it = m_pageTex.begin(); it != m_pageTex.end();) {
        const bool keep = (it->first >= m_page - PageRenderer::kBehind - 1 && it->first <= m_page + PageRenderer::kAhead + 1) ||
                          (it->first == m_slideFrom && m_slideT < 1.0f);
        if (keep) { ++it; continue; }
        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
        it = m_pageTex.erase(it);
    }
    std::set<int> have;
    for (const auto& kv : m_pageTex) have.insert(kv.first);
    if (have != m_told || m_page != m_toldPage) {
        m_told = have;
        m_toldPage = m_page;
        m_pages.want(m_page, have);
    }
}

// ------------------------------------------------------------------ frame

void App::update(float dt) {
    if (m_toastTime > 0.0f) m_toastTime -= dt;
    pumpCovers();
    if (m_reading) {
        pumpPages();
        if (m_slideT < 1.0f) {
            // COMIC_READER_TURN_TIME (seconds) slows the turn down, for screenshots in the simulator.
            static const float slow = std::getenv("COMIC_READER_TURN_TIME") ? (float)std::atof(std::getenv("COMIC_READER_TURN_TIME")) : 0.0f;
            const float time = slow > 0.0f ? slow : m_turnStyle == "curl" ? kCurlTime : kSlideTime;
            // COMIC_READER_TURN_HOLD (0..1) stops the turn part way: a screenshot of it.
            static const float hold = std::getenv("COMIC_READER_TURN_HOLD") ? (float)std::atof(std::getenv("COMIC_READER_TURN_HOLD")) : 1.0f;
            m_slideT = std::min(hold, std::min(1.0f, m_slideT + dt / time));
        }
        if (m_hud > 0.0f) m_hud -= dt;
        m_progressAge += dt;
        if (m_progressDirty && m_progressAge > 8.0f) saveProgress(false);
    } else if (m_tab != kSettings) {
        // Keep the selected row in view.
        const int row = m_items.empty() ? 0 : m_sel / kCols;
        const float view = kGridBottom - kGridTop;
        const float top = row * kRowStride;
        if (top < m_scroll) m_scroll = top;
        if (top + kCellH > m_scroll + view) m_scroll = top + kCellH - view;
        m_scrollShown += (m_scroll - m_scrollShown) * std::min(1.0f, dt * 14.0f);
        if (std::fabs(m_scroll - m_scrollShown) < 0.5f) m_scrollShown = m_scroll;
    } else {
        const int got = m_transfer.status().received;
        if (got != m_uploadSeen) m_uploadSeen = got;
    }
    // A check finished: remember what it found; say so once if it's new.
    const Updater::State us = m_updater.state();
    if (us != m_seenState) {
        m_seenState = us;
        if (us == Updater::State::Available || us == Updater::State::UpToDate) {
            m_known = Updater::knownLatest(m_appDir);
            if (us == Updater::State::Available && m_updater.quiet()) toast("Version " + m_updater.latest() + " is out: see Settings");
        }
    }
    // After an update: a moment to read "Updated", then start the new version.
    if (m_updater.state() == Updater::State::Done && m_restartIn < 0.0f) m_restartIn = 2.0f;
    if (m_restartIn > 0.0f && (m_restartIn -= dt) <= 0.0f) {
        m_restartIn = 0.0f;
        m_quit = true;
    }
    updatePanels();
}

void App::updatePanels() {
    if (!m_panels || !m_panels->active()) return;
    Panels::Item it;
    if (m_reading) {
        it.rel = m_book.rel;
        it.title = nameOf(m_book);
        it.subtitle = m_bookSubtitle;
        it.page = m_page;
        it.pages = m_pagesN;
        it.reading = true;
    } else if (m_tab != kSettings && !m_items.empty()) {
        const Library::Entry& e = m_items[(size_t)m_sel];
        it.rel = e.coverRel;
        it.title = nameOf(e);
        if (e.series) {
            it.subtitle = std::to_string(e.count) + (e.count == 1 ? " book" : " books");
            it.series = true;
        } else {
            const Library::Progress p = m_progress.get(e.rel);
            it.page = p.page;
            it.pages = p.pages;
            const size_t a = e.rel.find('/'), b = e.rel.find_last_of('/');
            it.subtitle = b != a ? Library::titleOf(e.rel.substr(a + 1, b - a - 1)) : kTabNames[m_tab == kManuals ? kManuals : kComics];
            auto in = m_info.find(e.rel);
            if (in != m_info.end() && !in->second.series.empty())
                it.subtitle = !in->second.title.empty() ? in->second.title : !in->second.publisher.empty() ? in->second.publisher : it.subtitle;
        }
    }
    m_panels->show(it);
}

void App::render() {
    SDL_SetRenderTarget(m_renderer, m_canvas);
    if (m_reading) {
        renderReader();
    } else {
        SDL_RenderSetScale(m_renderer, m_canvasScale, m_canvasScale);
        renderLibrary();
    }
    SDL_RenderSetScale(m_renderer, m_canvasScale, m_canvasScale);
    if (m_confirm) {
        if (m_confirmKind == Confirm::Forget)
            Theme::confirmDialog(m_renderer, "Forget reading history?", "Keep it", "Forget", m_confirmSel);
        else
            Theme::confirmDialog(m_renderer, "Exit Comic Reader?", "Keep reading", "Exit", m_confirmSel);
    }
    if (m_toastTime > 0.0f) Theme::toast(m_renderer, m_toast, m_toastTime);
    SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
    SDL_SetRenderTarget(m_renderer, nullptr);
}

void App::renderLibrary() {
    SDL_Renderer* r = m_renderer;
    Theme::paper(r);
    if (m_tab == kSettings) renderSettings();
    else renderGrid();
    // The masthead and tabs over anything scrolled up under them.
    SDL_Rect top{0, 0, kW, (int)(kGridTop - 50.0f)};
    SDL_RenderSetClipRect(r, &top);
    Theme::paper(r);
    SDL_RenderSetClipRect(r, nullptr);
    Theme::masthead(r);
    renderTabs();
    // Inside a series: its name in a caption box, and where you are.
    if (m_tab != kSettings && m_tab != kContinue && m_dir != shelfDir()) {
        const std::string name = Theme::upper(Library::titleOf(m_dir));
        const float w = std::min(kW - 220.0f, Theme::width(r, name, 30.0f, AppFont::Face::Display) + 40.0f);
        Theme::inkBox(r, {Theme::kMargin, kTabsTop + 64.0f, w, 46.0f}, Theme::Yellow, 3.0f, 4.0f);
        AppFont::draw(r, Theme::ellipsize(r, name, w - 30.0f, 30.0f, AppFont::Face::Display), Theme::kMargin + 15.0f,
                      kTabsTop + 72.0f, 30.0f, Theme::Ink, AppFont::Face::Display);
    }
    if (m_tab != kSettings && !m_items.empty()) {
        const std::string n = std::to_string(m_sel + 1) + " / " + std::to_string(m_items.size());
        AppFont::drawRight(r, n, kW - Theme::kMargin, kTabsTop + 74.0f, 28.0f, Theme::Muted, AppFont::Face::Display);
    }
    if (m_details) renderDetails();
    std::string hints;
    if (m_tab == kSettings) {
        if (m_setView == SetView::List)
            hints = m_setSel == kRowTurn || m_setSel == kRowDirection ? "LEFT/RIGHT Change   LB/RB Shelves   B Exit"
                                                                      : "A Open   LB/RB Shelves   B Exit";
        else if (m_setView == SetView::Updates && m_updater.state() == Updater::State::Available) hints = "A Install   B Back";
        else hints = "B Back";
    } else if (m_items.empty()) hints = "LB/RB Shelves   B Exit";
    else if (m_details && m_lookupOn) {
        const Lookup::State ls = m_lookup.state();
        hints = ls == Lookup::State::Found ? "A Keep   X Next match   B Discard"
              : ls == Lookup::State::Searching ? "B Stop" : "X Try again   B Back";
    } else if (m_details) hints = "A Read   X Look up   B Close";
    else hints = std::string("A Read   Y Details   LB/RB Shelves   ") + (m_dir != shelfDir() && m_tab != kContinue ? "B Back" : "B Exit");
    Theme::footerHints(r, hints);
}

void App::renderTabs() {
    SDL_Renderer* r = m_renderer;
    const float h = 48.0f, gap = 12.0f, size = 28.0f;
    const AppFont::Face D = AppFont::Face::Display;
    float widths[kTabCount], total = 0.0f;
    for (int i = 0; i < kTabCount; ++i) {
        widths[i] = Theme::width(r, Theme::upper(kTabNames[i]), size, D) + 34.0f;
        total += widths[i] + (i ? gap : 0.0f);
    }
    float x = (kW - total) * 0.5f;
    for (int i = 0; i < kTabCount; ++i) {
        const bool on = i == m_tab;
        const FRect rc{x, kTabsTop - (on ? 4.0f : 0.0f), widths[i], h};
        Theme::inkBox(r, rc, on ? Theme::Yellow : Theme::White, 3.0f, on ? 5.0f : 3.0f);
        AppFont::drawCentered(r, Theme::upper(kTabNames[i]), rc.x + rc.w * 0.5f, rc.y + 10.0f, size, on ? Theme::Ink : Theme::Muted, D);
        x += widths[i] + gap;
    }
}

void App::renderGrid() {
    SDL_Renderer* r = m_renderer;
    if (m_items.empty()) {
        if (m_tab == kContinue)
            Theme::emptyState(r, "Nothing to continue!", "Books you're partway through show up here.");
        else
            Theme::emptyState(r, m_tab == kManuals ? "No manuals yet!" : "No comics yet!",
                              std::string("Use Wi-Fi upload, or copy them to ") + shelfDir() + "/ on the stick.");
        return;
    }
    const int n = (int)m_items.size();
    const int firstRow = std::max(0, (int)(m_scrollShown / kRowStride) - 1);
    const int lastRow = (int)((m_scrollShown + (kGridBottom - kGridTop)) / kRowStride) + 1;
    // Covers just below the screen are asked for too, so scrolling finds them ready.
    for (int i = (lastRow + 1) * kCols; i < std::min(n, (lastRow + 2) * kCols); ++i) cover(m_items[(size_t)i].coverRel);

    SDL_Rect clip{0, (int)(kGridTop - 40.0f), kW, (int)(kGridBottom - kGridTop + 40.0f)};
    SDL_RenderSetClipRect(r, &clip);
    auto tile = [&](int i) {
        const Library::Entry& e = m_items[(size_t)i];
        const int row = i / kCols, col = i % kCols;
        const float x = kSide + col * (kCellW + kGap);
        const float y = kGridTop + row * kRowStride - m_scrollShown;
        const bool sel = i == m_sel;
        const FRect box{x, y, kCellW, kCoverH};
        if (sel) Theme::burst(r, box.x + box.w * 0.5f, box.y + box.h * 0.5f, 172.0f, m_clock * 6.0, Theme::Yellow, Theme::Red);
        if (e.series) {  // a stack of issues behind the cover
            Theme::inkBox(r, {box.x + 12.0f, box.y - 12.0f, box.w, box.h}, Theme::White, 3.0f, 0.0f);
            Theme::inkBox(r, {box.x + 6.0f, box.y - 6.0f, box.w, box.h}, Theme::White, 3.0f, 0.0f);
        }
        const float ink = sel ? 6.0f : 4.0f;
        Theme::inkBox(r, {box.x - ink, box.y - ink, box.w + 2 * ink, box.h + 2 * ink}, Theme::White, ink, sel ? 8.0f : 6.0f);
        Tex* t = cover(e.coverRel);
        if (t && t->tex) {
            // Fill the frame, cropping the long side a little (covers vary).
            const float s = std::max(box.w / t->w, box.h / t->h);
            const float sw = box.w / s, sh = box.h / s;
            SDL_Rect src{(int)((t->w - sw) * 0.5f), (int)((t->h - sh) * 0.5f), (int)sw, (int)sh};
            SDL_Rect dst{(int)std::lround(box.x), (int)std::lround(box.y), (int)std::lround(box.w), (int)std::lround(box.h)};
            SDL_RenderCopy(r, t->tex, &src, &dst);
        } else {
            AppFont::drawCentered(r, t && t->failed ? "CAN'T OPEN" : "...", box.x + box.w * 0.5f, box.y + box.h * 0.45f, 28.0f,
                                  Theme::Muted, AppFont::Face::Display);
        }
        // Progress, the title, and how far in.
        std::string detail;
        if (e.series) {
            detail = std::to_string(e.count) + (e.count == 1 ? " BOOK" : " BOOKS");
        } else {
            const Library::Progress p = m_progress.get(e.rel);
            if (p.pages > 0 && p.page > 0) {
                Theme::progress(r, {box.x + 8.0f, box.y + box.h - 22.0f, box.w - 16.0f, 14.0f}, (p.page + 1) / (float)p.pages);
                detail = p.page >= p.pages - 1 ? "FINISHED!" : "PAGE " + std::to_string(p.page + 1) + " OF " + std::to_string(p.pages);
            } else {
                detail = "NEW!";
            }
        }
        float ty = box.y + box.h + 16.0f;
        AppFont::draw(r, Theme::ellipsize(r, nameOf(e), kCellW, Theme::Type::Small), box.x, ty, Theme::Type::Small, Theme::Ink);
        ty += 32.0f;
        AppFont::draw(r, detail, box.x, ty, 24.0f, sel ? Theme::Red : Theme::Blue, AppFont::Face::Display);
    };
    // The selected tile last: its starburst goes over its neighbors.
    for (int i = firstRow * kCols; i < std::min(n, (lastRow + 1) * kCols); ++i)
        if (i != m_sel) tile(i);
    if (m_sel >= firstRow * kCols && m_sel < std::min(n, (lastRow + 1) * kCols)) tile(m_sel);
    SDL_RenderSetClipRect(r, nullptr);
}

void App::renderUpload() {
    SDL_Renderer* r = m_renderer;
    const float cx = kW * 0.5f, inner = kW - 2.0f * Theme::kMargin - 40.0f;
    const AppFont::Face D = AppFont::Face::Display;
    const TransferServer::Status st = m_transfer.status();
    const FRect card{Theme::kMargin, kGridTop - 10.0f, kW - 2.0f * Theme::kMargin, 420.0f};
    Theme::inkBox(r, card, Theme::White);
    float y = card.y + 30.0f;
    if (!st.listening) {
        AppFont::drawCentered(r, "COULDN'T START WI-FI UPLOAD", cx, y + 40.0f, Theme::Type::Heading, Theme::Red, D);
        AppFont::drawCentered(r, Theme::ellipsize(r, st.error, inner, Theme::Type::Small), cx, y + 110.0f, Theme::Type::Small, Theme::Ink);
    } else if (st.addresses.empty()) {
        AppFont::drawCentered(r, "NO NETWORK!", cx, y + 40.0f, Theme::Type::Heading, Theme::Red, D);
        AppFont::drawCentered(r, "Connect the cabinet to Wi-Fi or a cable, then try again.", cx, y + 110.0f, Theme::Type::Small, Theme::Ink);
    } else {
        AppFont::drawCentered(r, "ON A PHONE OR COMPUTER ON THE SAME NETWORK, OPEN", cx, y, 26.0f, Theme::Muted, D);
        y += 44.0f;
        for (size_t i = 0; i < st.addresses.size() && i < 2; ++i) {
            const std::string url = "http://" + st.addresses[i] + (st.port == 80 ? "" : ":" + std::to_string(st.port));
            const float us = Theme::fitSize(r, url, inner, Theme::Type::Heading, Theme::Type::Caption, D);
            AppFont::drawCentered(r, url, cx, y + (Theme::Type::Heading - us) * 0.5f, us, Theme::Blue, D);
            y += 58.0f;
        }
        y += 12.0f;
        AppFont::drawCentered(r, "AND ENTER THE PIN", cx, y, 26.0f, Theme::Muted, D);
        y += 40.0f;
        Theme::letterCentered(r, st.pin, cx, y, Theme::Type::Hero, Theme::Red, 4.0f);
    }
    // What is happening, in a caption.
    const FRect box{Theme::kMargin, card.y + card.h + 34.0f, card.w, 400.0f};
    Theme::inkBox(r, box, Theme::Yellow);
    float ty = box.y + 24.0f;
    const float tx = box.x + 28.0f, tw = box.w - 56.0f;
    if (!st.current.empty()) {
        AppFont::draw(r, "RECEIVING...", tx, ty, 28.0f, Theme::Red, D);
        ty += 40.0f;
        AppFont::draw(r, Theme::ellipsize(r, st.current, tw, Theme::Type::Body), tx, ty, Theme::Type::Body, Theme::Ink);
        ty += 48.0f;
        const float f = st.currentTotal ? (float)((double)st.currentDone / (double)st.currentTotal) : 0.0f;
        Theme::progress(r, {tx, ty, tw, 22.0f}, f);
        char pct[48];
        std::snprintf(pct, sizeof(pct), "%d%%  of %.1f MB", (int)(f * 100.0f), st.currentTotal / 1048576.0);
        AppFont::draw(r, pct, tx, ty + 30.0f, Theme::Type::Small, Theme::Ink);
        ty += 76.0f;
    } else {
        AppFont::draw(r, st.listening ? "WAITING FOR BOOKS..." : "NOT RUNNING", tx, ty, 32.0f, Theme::Ink, D);
        ty += 56.0f;
    }
    if (st.received > 0) {
        char done[64];
        std::snprintf(done, sizeof(done), "RECEIVED %d %s (%.1f MB)", st.received, st.received == 1 ? "FILE" : "FILES",
                      st.receivedBytes / 1048576.0);
        AppFont::draw(r, done, tx, ty, 28.0f, Theme::Red, D);
        ty += 44.0f;
    }
    for (const std::string& line : st.recent) {
        if (ty > box.y + box.h - 40.0f) break;
        AppFont::draw(r, Theme::ellipsize(r, line, tw, Theme::Type::Small), tx, ty, Theme::Type::Small,
                      line.find(" - failed") != std::string::npos ? Theme::Red : Theme::Ink);
        ty += 36.0f;
    }
}

// Y on a book: what it is. ComicInfo details if it has them, else its file.
void App::renderDetails() {
    SDL_Renderer* r = m_renderer;
    const AppFont::Face D = AppFont::Face::Display;
    const Library::Entry& e = m_items[(size_t)m_sel];
    Gfx::rect(r, {0, 0, (float)kW, (float)kH}, {24, 20, 18, 140});
    const FRect card{30.0f, 200.0f, kW - 60.0f, 940.0f};
    Theme::inkBox(r, card, Theme::White, 4.0f, 8.0f);
    // The cover, top left.
    const FRect cv{card.x + 26.0f, card.y + 26.0f, 230.0f, 345.0f};
    Theme::inkBox(r, cv, Theme::White, 4.0f, 6.0f);
    if (Tex* t = cover(e.coverRel)) {
        if (t->tex) {
            const float sc = std::min((cv.w - 8) / t->w, (cv.h - 8) / t->h);
            SDL_Rect dst{(int)(cv.x + 4 + (cv.w - 8 - t->w * sc) / 2), (int)(cv.y + 4 + (cv.h - 8 - t->h * sc) / 2), (int)(t->w * sc), (int)(t->h * sc)};
            SDL_RenderCopy(r, t->tex, nullptr, &dst);
        }
    }
    auto it = m_info.find(e.rel);
    const Lookup::State ls = m_lookup.state();
    const bool preview = m_lookupOn && ls == Lookup::State::Found;
    const Library::BookInfo info = preview ? m_lookup.result() : it == m_info.end() ? Library::BookInfo() : it->second;
    const float tx = cv.x + cv.w + 28.0f, tw = card.x + card.w - 26.0f - tx;
    float y = cv.y + 4.0f;
    // The name, lettered, two lines at most.
    const std::string name = Theme::upper(preview ? info.display() : nameOf(e));
    std::vector<std::string> nl;
    {
        std::string cur, word;
        auto flush = [&](const std::string& w) {
            const std::string t = cur.empty() ? w : cur + " " + w;
            if (!cur.empty() && Theme::width(r, t, 46.0f, D) > tw) { nl.push_back(cur); cur = w; } else cur = t;
        };
        for (char ch : name) { if (ch == ' ') { if (!word.empty()) flush(word); word.clear(); } else word += ch; }
        if (!word.empty()) flush(word);
        if (!cur.empty()) nl.push_back(cur);
    }
    for (size_t i = 0; i < nl.size() && i < 3; ++i) {
        Theme::letter(r, Theme::ellipsize(r, nl[i], tw, 46.0f, D), tx, y, 46.0f, Theme::Yellow, 3.0f);
        y += 52.0f;
    }
    y += 8.0f;
    auto line = [&](const std::string& label, const std::string& value) {
        if (value.empty() || y > cv.y + cv.h + 40.0f) return;
        AppFont::draw(r, label, tx, y, 22.0f, Theme::Red, D);
        y += 26.0f;
        AppFont::draw(r, Theme::ellipsize(r, value, tw, Theme::Type::Small), tx, y, Theme::Type::Small, Theme::Ink);
        y += 36.0f;
    };
    if (!info.series.empty()) line("STORY", info.title);
    static const char* months[] = {"", "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December"};
    const int mo = std::atoi(info.month.c_str());
    std::string when = (mo >= 1 && mo <= 12 ? std::string(months[mo]) + " " : "") + info.year;
    line("PUBLISHED", info.publisher.empty() ? when : when.empty() ? info.publisher : info.publisher + ", " + when);
    line("WRITER", info.writer);
    line("ART", info.artist);
    const Library::Progress p = m_progress.get(e.rel);
    const int pages = p.pages > 0 ? p.pages : info.pages;
    std::string where = pages > 0 ? std::to_string(pages) + " pages" : "";
    if (p.pages > 0 && p.page > 0) where += ", you're on page " + std::to_string(p.page + 1);
    line("PAGES", where);
    // The summary under it all, or the file when there's nothing else to say.
    y = std::max(y, cv.y + cv.h + 30.0f);
    const std::string body = !info.summary.empty() ? info.summary
                           : info.series.empty() && info.title.empty()
                               ? "No details in this book (no ComicInfo.xml). Press X to look it up in the Grand Comics Database. File: " + e.rel
                               : "";
    if (!body.empty()) {
        for (const std::string& l : noteLines(r, body, card.w - 52.0f, Theme::Type::Small)) {
            if (y > card.y + card.h - (m_lookupOn ? 170.0f : 70.0f)) break;
            AppFont::draw(r, l, card.x + 26.0f, y, Theme::Type::Small, Theme::Ink);
            y += 34.0f;
        }
    }
    // Where the details came from.
    if (info.source == "gcd")
        AppFont::drawCentered(r, "Details from the Grand Comics Database (comics.org), CC BY-SA 4.0", kW * 0.5f,
                              card.y + card.h - 40.0f, 18.0f, Theme::Muted);
    // The lookup: what it's doing, or the match it found to keep or skip.
    if (m_lookupOn) {
        const FRect box{card.x + 20.0f, card.y + card.h - 150.0f, card.w - 40.0f, 96.0f};
        Theme::inkBox(r, box, preview ? Theme::Yellow : Theme::White, 3.0f, 4.0f);
        std::string top, bottom;
        if (ls == Lookup::State::Searching) { top = "LOOKING UP..."; bottom = m_lookup.message(); }
        else if (preview) { top = "IS THIS IT?"; bottom = m_lookup.match(); }
        else if (ls == Lookup::State::NotFound) { top = "NOT FOUND"; bottom = m_lookup.message(); }
        else { top = "COULDN'T LOOK IT UP"; bottom = m_lookup.message(); }
        AppFont::draw(r, top, box.x + 20.0f, box.y + 12.0f, 32.0f, preview ? Theme::Ink : Theme::Red, D);
        AppFont::draw(r, Theme::ellipsize(r, bottom, box.w - 40.0f, Theme::Type::Small), box.x + 20.0f, box.y + 54.0f,
                      Theme::Type::Small, Theme::Ink);
    }
    if (info.rtl) Theme::burst(r, card.x + card.w - 60.0f, card.y + 20.0f, 60.0f, 10.0, Theme::Yellow, {0, 0, 0, 0}),
                  Theme::letterCentered(r, "R TO L", card.x + card.w - 60.0f, card.y + 6.0f, 24.0f, Theme::Red, 2.0f);
}

void App::renderSettings() {
    if (m_setView == SetView::Upload) { renderUpload(); return; }
    if (m_setView == SetView::Updates) { renderUpdates(); return; }
    if (m_setView == SetView::About) { renderAbout(); return; }
    SDL_Renderer* r = m_renderer;
    const AppFont::Face D = AppFont::Face::Display;
    const char* labels[kSetRows] = {"Wi-Fi upload", "Check for updates", "Page turn", "Reading direction",
                                    "Forget reading history", "Redraw covers", "About"};
    const std::string& known = m_known;
    const bool updateKnown = !known.empty() && Updater::isNewer(known, kVersion);
    for (int i = 0; i < kSetRows; ++i) {
        const bool on = i == m_setSel;
        const FRect rc{Theme::kMargin, kGridTop - 20.0f + i * 112.0f, kW - 2.0f * Theme::kMargin, 90.0f};
        Theme::inkBox(r, rc, on ? Theme::Yellow : Theme::White, on ? 5.0f : 3.0f, on ? 8.0f : 4.0f);
        AppFont::draw(r, Theme::upper(labels[i]), rc.x + 26.0f, rc.y + 22.0f, 40.0f, Theme::Ink, D);
        std::string value;
        if (i == kRowTurn) value = m_turnStyle == "slide" ? "SLIDE" : m_turnStyle == "none" ? "NONE" : "CURL";
        else if (i == kRowDirection) value = m_rtl ? "RIGHT TO LEFT" : "LEFT TO RIGHT";
        else if (i == kRowUpdates) value = updateKnown ? "V" + known + "!" : "V" + std::string(kVersion);
        if (!value.empty()) {
            const bool choice = i == kRowTurn || i == kRowDirection;
            const std::string v = choice && on ? "< " + value + " >" : value;
            AppFont::drawRight(r, v, rc.x + rc.w - 26.0f, rc.y + 26.0f, 34.0f, i == kRowUpdates && updateKnown ? Theme::Red : Theme::Blue, D);
        }
    }
    const char* notes[kSetRows] = {
        "Send books from a phone or computer.",
        "Get the newest version from GitHub.",
        "How a page turns: a curl, a slide, or straight to the next.",
        "Right to left swaps the flippers, for manga.",
        "Clears Continue and every book's place.",
        "Draws every cover again from its book.",
        "Version, license and the people behind it.",
    };
    AppFont::drawCentered(r, notes[m_setSel], kW * 0.5f, kGridTop - 20.0f + kSetRows * 112.0f + 6.0f, Theme::Type::Small, Theme::Muted);
}

// Markdown-ish release notes as plain lines: markers dropped, wrapped to width.
std::vector<std::string> noteLines(SDL_Renderer* r, const std::string& md, float width, float size) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= md.size()) {
        size_t nl = md.find('\n', at);
        std::string line = md.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? md.size() + 1 : nl + 1;
        std::string clean;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '*' || line[i] == '`' || line[i] == '#' || line[i] == '\r') continue;
            clean += line[i];
        }
        while (!clean.empty() && clean[0] == ' ') clean.erase(0, 1);
        if (clean.compare(0, 2, "- ") == 0) clean = "- " + clean.substr(2);  // (the fonts have ASCII only)
        if (clean.empty()) { if (!out.empty() && !out.back().empty()) out.push_back(""); continue; }
        std::string cur, word;
        auto flush = [&](const std::string& w) {
            const std::string t = cur.empty() ? w : cur + " " + w;
            if (!cur.empty() && Theme::width(r, t, size) > width) { out.push_back(cur); cur = "  " + w; }
            else cur = t;
        };
        for (char ch : clean) {
            if (ch == ' ') { if (!word.empty()) flush(word); word.clear(); }
            else word += ch;
        }
        if (!word.empty()) flush(word);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

void App::renderUpdates() {
    SDL_Renderer* r = m_renderer;
    const AppFont::Face D = AppFont::Face::Display;
    const FRect card{Theme::kMargin, kGridTop - 10.0f, kW - 2.0f * Theme::kMargin, 860.0f};
    Theme::inkBox(r, card, Theme::White);
    const float cx = kW * 0.5f, inner = card.w - 60.0f;
    float y = card.y + 34.0f;
    const Updater::State st = m_updater.state();
    AppFont::drawCentered(r, "THIS IS VERSION " + std::string(kVersion), cx, y, 30.0f, Theme::Muted, D);
    y += 60.0f;
    auto big = [&](const std::string& s, SDL_Color c) { Theme::letterCentered(r, Theme::upper(s), cx, y, Theme::Type::Heading, c, 3.0f); y += 76.0f; };
    auto small = [&](const std::string& s) {
        AppFont::drawCentered(r, Theme::ellipsize(r, s, inner, Theme::Type::Small), cx, y, Theme::Type::Small, Theme::Ink);
        y += 40.0f;
    };
    switch (st) {
    case Updater::State::Idle: case Updater::State::Checking: big("Checking...", Theme::Yellow); break;
    case Updater::State::UpToDate: big("Up to date!", Theme::Yellow); small("You have the newest version."); break;
    case Updater::State::NoRelease: big("No releases yet", Theme::Yellow); small("Press A to check again."); break;
    case Updater::State::Failed: big("Couldn't check", Theme::Red); small(m_updater.message()); small("Press A to try again."); break;
    case Updater::State::Downloading: {
        big("Downloading...", Theme::Yellow);
        Theme::progress(r, {card.x + 60.0f, y, card.w - 120.0f, 26.0f}, m_updater.progress() / 100.0f);
        y += 50.0f;
        small(std::to_string(m_updater.progress()) + "%  of version " + m_updater.latest());
        break;
    }
    case Updater::State::Installing: big("Installing...", Theme::Yellow); small("Don't turn the cabinet off."); break;
    case Updater::State::Done: big("Updated!", Theme::Yellow); small(m_updater.message()); small("Restarting..."); break;
    case Updater::State::Available: {
        big("Version " + m_updater.latest() + "!", Theme::Yellow);
        small("Press A to install it. Your books and places are kept.");
        y += 10.0f;
        // What's new, newest first, as far as it fits.
        float ty = y;
        for (const auto& n : m_updater.notes()) {
            if (ty > card.y + card.h - 70.0f) break;
            AppFont::draw(r, "VERSION " + n.first, card.x + 30.0f, ty, 30.0f, Theme::Red, D);
            ty += 42.0f;
            for (const std::string& line : noteLines(r, n.second, inner, 22.0f)) {
                if (ty > card.y + card.h - 40.0f) break;
                AppFont::draw(r, line, card.x + 30.0f, ty, 22.0f, Theme::Ink);
                ty += line.empty() ? 14.0f : 30.0f;
            }
            ty += 12.0f;
        }
        break;
    }
    }
}

void App::renderAbout() {
    SDL_Renderer* r = m_renderer;
    const AppFont::Face D = AppFont::Face::Display;
    const FRect card{Theme::kMargin, kGridTop - 10.0f, kW - 2.0f * Theme::kMargin, 860.0f};
    Theme::inkBox(r, card, Theme::White);
    const float cx = kW * 0.5f;
    float y = card.y + 30.0f;
    Theme::letterCentered(r, "COMIC READER " + std::string(kVersion), cx, y, Theme::Type::Heading, Theme::Yellow, 3.0f);
    y += 74.0f;
    AppFont::drawCentered(r, "By Bla1ze", cx, y, Theme::Type::Body, Theme::Ink);
    y += 54.0f;
    const char* lines[] = {
        "Free software: GNU AGPL 3.0. The source code is at",
        "github.com/Bla1ze/comic-reader",
        "",
        "It uses MuPDF (Artifex, AGPL 3.0), libarchive and libwebp,",
        "with FreeType, libjpeg (the Independent JPEG Group),",
        "Little CMS, zlib, jbig2dec and OpenJPEG.",
        "Lettering: Bangers and Comic Neue (SIL OFL).",
        "Looked-up details: the Grand Comics Database",
        "(comics.org), CC BY-SA 4.0.",
        "Controls and display code based on the AtGames",
        "External Applications SDK.",
        "",
        "Every license is in licenses/ on the USB stick.",
    };
    for (const char* l : lines) {
        if (!*l) { y += 20.0f; continue; }
        const bool link = std::string(l).find("github.com") != std::string::npos;
        AppFont::drawCentered(r, l, cx, y, link ? 30.0f : Theme::Type::Small, link ? Theme::Blue : Theme::Ink, link ? D : AppFont::Face::Body);
        y += link ? 44.0f : 36.0f;
    }
}

// One step of a page turn at t (0..1): `top` is folding over from its outer
// edge (the right one, or the left reading right to left) to its spine; the
// part already turned shows its back (paper with the print faintly through),
// shaded at the fold, casting a shadow on `under`.
void App::curl(const Tex& top, const Tex& under, float t, bool fromLeft) {
    SDL_Renderer* r = m_renderer;
    auto place = [&](const Tex& x) { return SDL_Rect{(m_canvasW - x.w) / 2, (m_canvasH - x.h) / 2, x.w, x.h}; };
    const SDL_Rect U = place(under), T = place(top);
    SDL_RenderCopy(r, under.tex, nullptr, &U);
    // Work as if the fold moves right to left; mirror x for right-to-left books.
    auto mx = [&](int x, int w) { return fromLeft ? 2 * T.x + T.w - x - w : x; };
    const int fold = T.x + (int)std::lround(T.w * (1.0f - t));       // where the sheet bends
    const int kept = fold - T.x;                                     // still flat
    const int turned = T.w - kept;                                   // lifted, now lying left of the fold
    const int sw = (int)(m_canvasW * 0.05f);                         // shadow width
    if (kept > 0) {
        SDL_Rect src{fromLeft ? T.w - kept : 0, 0, kept, top.h}, dst{mx(T.x, kept), T.y, kept, T.h};
        SDL_RenderCopy(r, top.tex, &src, &dst);
    }
    // The shadow the lifted sheet casts on the page it uncovers.
    const float soft = std::sin(t * 3.14159265f);
    Gfx::hGradient(r, {(float)mx(fold, sw), (float)U.y, (float)sw, (float)U.h},
                   fromLeft ? SDL_Color{0, 0, 0, 0} : SDL_Color{0, 0, 0, (Uint8)(150 * soft)},
                   fromLeft ? SDL_Color{0, 0, 0, (Uint8)(150 * soft)} : SDL_Color{0, 0, 0, 0});
    if (turned <= 0) return;
    // The back of the sheet: from the fold leftwards, as wide as what's turned.
    const int bx = fold - turned;
    SDL_Rect back{mx(bx, turned), T.y, turned, T.h};
    // ...and the shadow it drops on the flat part beside it.
    Gfx::hGradient(r, {(float)mx(bx - sw, sw), (float)T.y, (float)sw, (float)T.h},
                   fromLeft ? SDL_Color{0, 0, 0, (Uint8)(110 * soft)} : SDL_Color{0, 0, 0, 0},
                   fromLeft ? SDL_Color{0, 0, 0, 0} : SDL_Color{0, 0, 0, (Uint8)(110 * soft)});
    Gfx::rect(r, {(float)back.x, (float)back.y, (float)back.w, (float)back.h}, {246, 240, 226, 255});
    SDL_Rect src{fromLeft ? 0 : kept, 0, turned, top.h};
    SDL_SetTextureAlphaMod(top.tex, 46);
    SDL_RenderCopyEx(r, top.tex, &src, &back, 0.0, nullptr, SDL_FLIP_HORIZONTAL);
    SDL_SetTextureAlphaMod(top.tex, 255);
    // Shading: light across the sheet, darker into the fold, a crease line.
    Gfx::hGradient(r, {(float)back.x, (float)back.y, (float)back.w, (float)back.h},
                   fromLeft ? SDL_Color{0, 0, 0, 90} : SDL_Color{255, 255, 255, 30},
                   fromLeft ? SDL_Color{255, 255, 255, 30} : SDL_Color{0, 0, 0, 90});
    Gfx::rect(r, {(float)mx(fold - 2, 3), (float)T.y, 3.0f, (float)T.h}, {0, 0, 0, 120});
}

void App::renderReader() {
    SDL_Renderer* r = m_renderer;
    SDL_RenderSetScale(r, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(r, Theme::Night.r, Theme::Night.g, Theme::Night.b, 255);
    SDL_RenderClear(r);
    const float e = 1.0f - std::pow(1.0f - m_slideT, 3.0f);  // ease out
    auto draw = [&](int page, float dx) {
        auto it = m_pageTex.find(page);
        if (it == m_pageTex.end() || !it->second.tex) return false;
        const Tex& t = it->second;
        SDL_Rect dst{(m_canvasW - t.w) / 2 + (int)dx, (m_canvasH - t.h) / 2, t.w, t.h};
        SDL_RenderCopy(r, t.tex, nullptr, &dst);
        return true;
    };
    const bool sliding = m_slideT < 1.0f && m_slideFrom >= 0 && m_zoom <= 1.0f;
    bool shown = false;
    if (m_zoom > 1.0f) {
        // Zoomed: the fitted page enlarged (soft), the sharp detail over it.
        auto it = m_pageTex.find(m_page);
        if (it != m_pageTex.end() && it->second.tex) {
            const Tex& t = it->second;
            SDL_Rect dst{(int)std::lround(-m_vx), (int)std::lround(-m_vy), (int)std::lround(t.w * m_zoom),
                         (int)std::lround(t.h * m_zoom)};
            SDL_RenderCopy(r, t.tex, nullptr, &dst);
            shown = true;
            if (m_detail.tex && m_detailPage == m_page && m_detailZoom == m_zoom) {
                SDL_Rect d{(int)std::lround(m_detailX - m_vx), (int)std::lround(m_detailY - m_vy), m_detail.w, m_detail.h};
                SDL_RenderCopy(r, m_detail.tex, nullptr, &d);
            }
        }
    } else if (sliding && m_turnStyle == "curl" && m_pageTex.count(m_slideFrom) && m_pageTex.count(m_page) &&
               m_pageTex[m_slideFrom].tex && m_pageTex[m_page].tex) {
        // Forward, the old page lifts from its outer edge and folds over; back,
        // the same played in reverse with the pages swapped. Right to left
        // mirrors it.
        const bool forward = m_slideDir > 0;
        const Tex& top = m_pageTex[forward ? m_slideFrom : m_page];   // the sheet being turned
        const Tex& under = m_pageTex[forward ? m_page : m_slideFrom]; // what it uncovers
        const float t = forward ? e : 1.0f - e;
        curl(top, under, t, m_readRtl);
        shown = true;
    } else {
        // Slide: the new page pushes the old one out (from the left when reading right to left).
        const int pd = m_readRtl ? -m_slideDir : m_slideDir;
        if (sliding) draw(m_slideFrom, -pd * e * m_canvasW);
        shown = draw(m_page, sliding ? pd * (1.0f - e) * m_canvasW : 0.0f);
    }

    SDL_RenderSetScale(r, m_canvasScale, m_canvasScale);
    const int n = m_pages.pageCount();
    if (n == -2) {
        Theme::emptyState(r, "Can't open this book!", m_pages.openError());
        Theme::footerHints(r, "B Library");
        return;
    }
    auto it = m_pageTex.find(m_page);
    if (!shown && !sliding) {
        if (it != m_pageTex.end() && it->second.failed)
            Theme::emptyState(r, "Can't show page " + std::to_string(m_page + 1) + "!", it->second.error);
        else
            Theme::letterCentered(r, n > 0 ? "PAGE " + std::to_string(m_page + 1) + "..." : "OPENING...", kW * 0.5f, 600.0f,
                                  Theme::Type::Title, Theme::Yellow, 4.0f);
    }
    if (m_hud > 0.0f) {
        // The title in a caption at the top, the page at the bottom.
        const AppFont::Face D = AppFont::Face::Display;
        const std::string title = Theme::ellipsize(r, Theme::upper(nameOf(m_book)), kW - 140.0f, 34.0f, D);
        const float tw = Theme::width(r, title, 34.0f, D) + 50.0f;
        Theme::inkBox(r, {(kW - tw) * 0.5f, 28.0f, tw, 60.0f}, Theme::Yellow);
        AppFont::drawCentered(r, title, kW * 0.5f, 40.0f, 34.0f, Theme::Ink, D);
        if (m_pagesN > 0) {
            const FRect rc{(kW - 300.0f) * 0.5f, kH - 160.0f, 300.0f, 110.0f};
            Theme::inkBox(r, rc, Theme::White);
            AppFont::drawCentered(r, std::to_string(m_page + 1) + " / " + std::to_string(m_pagesN), kW * 0.5f, rc.y + 12.0f,
                                  Theme::Type::Heading, Theme::Ink, D);
            Theme::progress(r, {rc.x + 28.0f, rc.y + 70.0f, rc.w - 56.0f, 18.0f}, (m_page + 1) / (float)m_pagesN);
        }
    }
    if (m_zoom > 1.0f) {
        // How far in, and how to get back.
        const std::string z = std::to_string((int)m_zoom) + "X";
        Theme::burst(r, kW - 78.0f, 86.0f, 54.0f, 8.0, Theme::Yellow, {0, 0, 0, 0});
        Theme::letterCentered(r, z, kW - 78.0f, 64.0f, 40.0f, Theme::Red, 3.0f);
        if (m_hud > 0.0f) Theme::footerHints(r, "START Zoom   STICK Move   B Whole page");
    }
}

}  // namespace

int runApp(const std::string& appDir) {
    // Returns kRestart when an update was installed (main then starts the new ELF).
    Library::ensureFolders(appDir);
    Library::openLog(appDir);
    Library::installCrashLog();
    log("Comic Reader %s, app folder %s", kVersion, appDir.c_str());
    CpuPin::enabled() = true;  // before any worker thread starts
    App app(appDir);
    const int rc = app.run();
    return app.restart() ? kRestart : rc;
}
