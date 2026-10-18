#include "App.h"

#include "AppConfig.h"
#include "AppFont.h"
#include "Covers.h"
#include "CpuPin.h"
#include "DisplayProfile.h"
#include "Gfx.h"
#include "Library.h"
#include "Pages.h"
#include "Panels.h"
#include "Theme.h"
#include "Transfer.h"
#include "Version.h"
#include "controls/Controls.h"

#include <SDL.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
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
using CE = AtGames::ControlEvent;

constexpr int kW = AppConfig::kLogicalWidth;
constexpr int kH = AppConfig::kLogicalHeight;

enum Tab { kContinue, kComics, kManuals, kUpload, kTabCount };
const char* const kTabNames[kTabCount] = {"Continue", "Comics", "Manuals", "Wi-Fi upload"};

// The library grid.
constexpr int kCols = 3;
constexpr float kSide = 36.0f, kGap = 24.0f;
constexpr float kCellW = (kW - 2 * kSide - (kCols - 1) * kGap) / kCols;  // 200
constexpr float kCoverH = kCellW * 1.5f;                                  // 300
constexpr float kCellH = kCoverH + 86.0f;
constexpr float kRowStride = kCellH + 22.0f;
constexpr float kTabsTop = 182.0f;
constexpr float kGridTop = 252.0f;
constexpr float kGridBottom = Theme::kFooterTop - 8.0f;

constexpr size_t kMaxCovers = 90;
constexpr float kHoldDelay = 0.45f, kHoldRepeat = 0.16f;
constexpr float kSlideTime = 0.24f, kHudTime = 2.2f;

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
    Tex* cover(const std::string& rel);
    void pumpCovers();

    // Reader
    void openBook(const Library::Entry& e);
    void closeBook();
    void turn(int dir);
    void pumpPages();
    void renderReader();
    void saveProgress(bool force);

    void updatePanels();
    void toast(const std::string& s) { m_toast = s; m_toastTime = 2.6f; }

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
    bool m_confirm = false;
    int m_confirmSel = 0;
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
    m_controls.open();
    m_panels.reset(new Panels());
    log("panels: %d up", m_panels->init(m_topo, m_appDir));
    return true;
}

int App::run() {
    if (!initVideo()) return 1;
    log("cpus: fast %s, slow %s", CpuPin::list(CpuPin::groups().fast).c_str(), CpuPin::list(CpuPin::groups().slow).c_str());
    m_progress.load(m_appDir);
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
            if (m_confirmSel == 1) m_quit = true;
            m_confirm = false;
        } else if (ce == CE::B || ce == CE::Back || ce == CE::Guide) m_confirm = false;
        return;
    }
    if (m_reading) {
        switch (ce) {
        case CE::RightShoulder: case CE::DpadRight: case CE::A: case CE::RightTrigger: turn(+1); break;
        case CE::LeftShoulder: case CE::DpadLeft: case CE::LeftTrigger: case CE::X: turn(-1); break;
        case CE::Start: case CE::DpadUp: case CE::DpadDown: m_hud = m_hud > 0.5f ? 0.0f : kHudTime; break;
        case CE::B: case CE::Back: case CE::Guide: closeBook(); break;
        default: break;
        }
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
    case CE::Guide: m_confirm = true; m_confirmSel = 0; break;
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
    int dir = 0;
    if (m_reading) dir = right && !left ? 1 : left && !right ? -1 : 0;
    else if (m_tab != kUpload && !m_confirm) dir = right ? 1 : left ? -1 : down ? kCols : up ? -kCols : 0;
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
        if (m_reading) turn(dir);
        else moveSel(dir);
    }
}

// ------------------------------------------------------------------ library

void App::setTab(int tab) {
    if (m_tab == kUpload && tab != kUpload) {
        const int got = m_transfer.status().received;
        m_transfer.stop();
        if (got > 0) toast("Added " + std::to_string(got) + (got == 1 ? " file" : " files"));
    }
    const bool changed = tab != m_tab;
    m_tab = tab;
    if (m_tab == kUpload) {
        m_transfer.start(m_appDir);
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
    if (m_tab == kUpload || m_items.empty()) return;
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
    m_confirm = true;
    m_confirmSel = 0;
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
    m_gen = m_pages.open(m_appDir + "/" + e.rel, m_canvasW, m_canvasH);
    log("open %s at page %d", e.rel.c_str(), m_page + 1);
}

void App::closeBook() {
    saveProgress(true);
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
    m_slideFrom = m_page;
    m_slideDir = dir;
    m_slideT = 0.0f;
    m_page = np;
    m_progressDirty = true;
}

void App::saveProgress(bool force) {
    if (!m_reading || m_pagesN <= 0) return;
    if (!m_progressDirty && !force) return;
    m_progress.set(m_book.rel, m_page, m_pagesN);
    m_progressDirty = false;
    m_progressAge = 0.0f;
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
    if (m_pages.take(r)) {
        Tex t;
        if (r.error.empty()) {
            t.tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, r.w, r.h);
            if (t.tex) {
                SDL_UpdateTexture(t.tex, nullptr, r.rgba.data(), r.w * 4);
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
    }
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
        if (m_slideT < 1.0f) m_slideT = std::min(1.0f, m_slideT + dt / kSlideTime);
        if (m_hud > 0.0f) m_hud -= dt;
        m_progressAge += dt;
        if (m_progressDirty && m_progressAge > 8.0f) saveProgress(false);
    } else if (m_tab != kUpload) {
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
    updatePanels();
}

void App::updatePanels() {
    if (!m_panels || !m_panels->active()) return;
    Panels::Item it;
    if (m_reading) {
        it.rel = m_book.rel;
        it.title = m_book.title;
        it.subtitle = m_bookSubtitle;
        it.page = m_page;
        it.pages = m_pagesN;
        it.reading = true;
    } else if (m_tab != kUpload && !m_items.empty()) {
        const Library::Entry& e = m_items[(size_t)m_sel];
        it.rel = e.coverRel;
        it.title = e.title;
        if (e.series) {
            it.subtitle = std::to_string(e.count) + (e.count == 1 ? " book" : " books");
            it.series = true;
        } else {
            const Library::Progress p = m_progress.get(e.rel);
            it.page = p.page;
            it.pages = p.pages;
            const size_t a = e.rel.find('/'), b = e.rel.find_last_of('/');
            it.subtitle = b != a ? Library::titleOf(e.rel.substr(a + 1, b - a - 1)) : kTabNames[m_tab == kManuals ? kManuals : kComics];
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
    if (m_confirm) Theme::confirmDialog(m_renderer, kW, kH, "Exit Comic Reader?", "Cancel", "Exit", m_confirmSel);
    if (m_toastTime > 0.0f && !m_toast.empty()) {
        const float a = std::min(1.0f, m_toastTime / 0.4f);
        const float tw = AppFont::measureWidth(m_renderer, m_toast, Theme::Type::Small) + 56.0f;
        const FRect rc{(kW - tw) * 0.5f, Theme::kFooterTop - 96.0f, tw, 56.0f};
        Gfx::roundRect(m_renderer, rc, 28.0f, {20, 26, 44, (Uint8)(235 * a)});
        AppFont::drawCentered(m_renderer, m_toast, kW * 0.5f, rc.y + 14.0f, Theme::Type::Small, Gfx::withAlpha(Theme::Text, (Uint8)(255 * a)));
    }
    SDL_RenderSetScale(m_renderer, 1.0f, 1.0f);
    SDL_SetRenderTarget(m_renderer, nullptr);
}

void App::renderLibrary() {
    SDL_Renderer* r = m_renderer;
    Theme::backgroundAnimated(r, kW, kH, m_clock);
    if (m_tab == kUpload) {
        renderUpload();
    } else {
        renderGrid();
    }
    // Header and tabs over anything scrolled up under them.
    Theme::vGradient(r, {0.0f, 0.0f, (float)kW, kGridTop - 6.0f}, Theme::BgTop, Gfx::withAlpha(Theme::BgTop, 250));
    std::string title = kTabNames[m_tab];
    if (m_tab != kContinue && m_tab != kUpload && m_dir != shelfDir()) title = Library::titleOf(m_dir);
    Theme::header(r, kW, "Comic Reader", title);
    renderTabs();
    std::string hints;
    if (m_tab == kUpload) hints = "LB/RB Shelves   B Exit";
    else if (m_items.empty()) hints = "LB/RB Shelves   B Exit";
    else hints = std::string("A Read   LB/RB Shelves   ") + (m_dir != shelfDir() && m_tab != kContinue ? "B Back" : "B Exit");
    Theme::footerHints(r, kW, hints, "");
}

void App::renderTabs() {
    SDL_Renderer* r = m_renderer;
    const float h = 40.0f, gap = 10.0f, size = Theme::Type::Caption;
    float widths[kTabCount], total = 0.0f;
    for (int i = 0; i < kTabCount; ++i) {
        widths[i] = AppFont::measureWidth(r, kTabNames[i], size) + 36.0f;
        total += widths[i] + (i ? gap : 0.0f);
    }
    float x = (kW - total) * 0.5f;
    for (int i = 0; i < kTabCount; ++i) {
        const FRect rc{x, kTabsTop, widths[i], h};
        const bool on = i == m_tab;
        if (on) {
            Gfx::softRect(r, rc, h * 0.5f, 14.0f, Theme::alpha(Theme::accent(), 70), true);
            Gfx::roundRect(r, rc, h * 0.5f, Theme::accent());
        } else {
            Gfx::roundRect(r, rc, h * 0.5f, {255, 255, 255, 16});
        }
        AppFont::drawCentered(r, kTabNames[i], rc.x + rc.w * 0.5f, rc.y + 9.0f, size, on ? Theme::onAccent(Theme::accent()) : Theme::TextDim);
        x += widths[i] + gap;
    }
}

void App::renderGrid() {
    SDL_Renderer* r = m_renderer;
    if (m_items.empty()) {
        if (m_tab == kContinue)
            Theme::emptyState(r, kW, "Nothing to continue", "Books you're partway through show up here.");
        else
            Theme::emptyState(r, kW, m_tab == kManuals ? "No manuals yet" : "No comics yet",
                              std::string("Use Wi-Fi upload, or copy them to ") + shelfDir() + "/ on the stick.");
        return;
    }
    const int n = (int)m_items.size();
    const int firstRow = std::max(0, (int)(m_scrollShown / kRowStride) - 1);
    const int lastRow = (int)((m_scrollShown + (kGridBottom - kGridTop)) / kRowStride) + 1;
    // Covers just below the screen are asked for too, so scrolling finds them ready.
    for (int i = (lastRow + 1) * kCols; i < std::min(n, (lastRow + 2) * kCols); ++i) cover(m_items[(size_t)i].coverRel);

    SDL_Rect clip{0, (int)(kGridTop - 20.0f), kW, (int)(kGridBottom - kGridTop + 20.0f)};
    SDL_RenderSetClipRect(r, &clip);
    for (int i = firstRow * kCols; i < std::min(n, (lastRow + 1) * kCols); ++i) {
        const Library::Entry& e = m_items[(size_t)i];
        const int row = i / kCols, col = i % kCols;
        const float x = kSide + col * (kCellW + kGap);
        const float y = kGridTop + row * kRowStride - m_scrollShown;
        const bool sel = i == m_sel;
        FRect box{x, y, kCellW, kCoverH};
        if (e.series) {  // a stack of books
            Gfx::roundRect(r, {box.x + 14.0f, box.y - 12.0f, box.w - 28.0f, box.h}, 10.0f, {255, 255, 255, 22});
            Gfx::roundRect(r, {box.x + 7.0f, box.y - 6.0f, box.w - 14.0f, box.h}, 10.0f, {255, 255, 255, 36});
        }
        if (sel) Gfx::softRect(r, box, 12.0f, 22.0f, Theme::alpha(Theme::accent(), 120), true);
        else Gfx::softRect(r, box, 12.0f, 16.0f, {0, 0, 0, 150}, false);
        Gfx::roundRect(r, box, 10.0f, {24, 30, 46, 255});
        Tex* t = cover(e.coverRel);
        if (t && t->tex) {
            // Fill the box, cropping the long side a little (covers vary).
            const float s = std::max(box.w / t->w, box.h / t->h);
            const float sw = box.w / s, sh = box.h / s;
            SDL_Rect src{(int)((t->w - sw) * 0.5f), (int)((t->h - sh) * 0.5f), (int)sw, (int)sh};
            SDL_Rect dst{(int)std::lround(box.x), (int)std::lround(box.y), (int)std::lround(box.w), (int)std::lround(box.h)};
            SDL_RenderCopy(r, t->tex, &src, &dst);
        } else {
            AppFont::drawCentered(r, t && t->failed ? "Can't open" : "...", box.x + box.w * 0.5f, box.y + box.h * 0.45f,
                                  Theme::Type::Small, Theme::Muted);
        }
        if (sel) {
            const float b = 4.0f;
            Gfx::rect(r, {box.x - b, box.y - b, box.w + 2 * b, b}, Theme::accent());
            Gfx::rect(r, {box.x - b, box.y + box.h, box.w + 2 * b, b}, Theme::accent());
            Gfx::rect(r, {box.x - b, box.y, b, box.h}, Theme::accent());
            Gfx::rect(r, {box.x + box.w, box.y, b, box.h}, Theme::accent());
        }
        // Progress under the cover.
        float ty = box.y + box.h + 12.0f;
        std::string detail;
        if (e.series) {
            detail = std::to_string(e.count) + (e.count == 1 ? " book" : " books");
        } else {
            const Library::Progress p = m_progress.get(e.rel);
            if (p.pages > 0 && p.page > 0) {
                const float f = (p.page + 1) / (float)p.pages;
                Gfx::rect(r, {box.x, box.y + box.h - 8.0f, box.w, 8.0f}, {0, 0, 0, 160});
                Gfx::rect(r, {box.x, box.y + box.h - 8.0f, box.w * f, 8.0f}, Theme::accent());
                detail = p.page >= p.pages - 1 ? "Finished" : "Page " + std::to_string(p.page + 1) + " of " + std::to_string(p.pages);
            } else {
                detail = "New";
            }
        }
        const SDL_Color ink = sel ? Theme::Text : Theme::TextDim;
        AppFont::draw(r, Theme::ellipsize(r, e.title, kCellW, Theme::Type::Small, AppFont::Face::Body), box.x, ty, Theme::Type::Small, ink);
        ty += 32.0f;
        AppFont::draw(r, detail, box.x, ty, Theme::Type::Caption, sel ? Theme::accent() : Theme::Muted);
    }
    SDL_RenderSetClipRect(r, nullptr);
    Theme::counter(r, kW, m_sel + 1, n);
}

void App::renderUpload() {
    SDL_Renderer* r = m_renderer;
    const float cx = kW * 0.5f, inner = kW - 2.0f * Theme::kMargin;
    const TransferServer::Status st = m_transfer.status();
    float y = kGridTop + 30.0f;
    if (!st.listening) {
        AppFont::drawCentered(r, "Couldn't start Wi-Fi upload", cx, y, Theme::Type::Heading, Theme::Text, AppFont::Face::Display);
        AppFont::drawCentered(r, Theme::ellipsize(r, st.error, inner, Theme::Type::Small, AppFont::Face::Body), cx, y + 64.0f,
                              Theme::Type::Small, Theme::Muted);
    } else if (st.addresses.empty()) {
        AppFont::drawCentered(r, "No network connection", cx, y, Theme::Type::Heading, Theme::Text, AppFont::Face::Display);
        AppFont::drawCentered(r, "Connect the cabinet to Wi-Fi or a cable, then try again.", cx, y + 64.0f, Theme::Type::Small,
                              Theme::Muted);
    } else {
        AppFont::drawCentered(r, "On a phone or computer on the same network, open", cx, y, Theme::Type::Small, Theme::Muted);
        y += 46.0f;
        for (size_t i = 0; i < st.addresses.size() && i < 2; ++i) {
            const std::string url = "http://" + st.addresses[i] + (st.port == 80 ? "" : ":" + std::to_string(st.port));
            const float us = Theme::fitSize(r, url, inner, Theme::Type::Heading, Theme::Type::Caption, AppFont::Face::Display);
            AppFont::drawCentered(r, url, cx, y + (Theme::Type::Heading - us) * 0.5f, us, Theme::accent(), AppFont::Face::Display);
            y += 58.0f;
        }
        y += 20.0f;
        AppFont::drawCentered(r, "and enter the PIN", cx, y, Theme::Type::Small, Theme::Muted);
        y += 40.0f;
        AppFont::drawCentered(r, st.pin, cx, y, Theme::Type::Hero, Theme::Text, AppFont::Face::Display);
    }
    y = 700.0f;
    FRect card{Theme::kMargin - 16.0f, y, kW - 2.0f * (Theme::kMargin - 16.0f), 440.0f};
    Theme::rowCard(r, card, false);
    float ty = y + 26.0f;
    const float tx = card.x + 28.0f, tw = card.w - 56.0f;
    if (!st.current.empty()) {
        AppFont::draw(r, "Receiving", tx, ty, Theme::Type::Caption, Theme::Muted);
        ty += 32.0f;
        AppFont::draw(r, Theme::ellipsize(r, st.current, tw, Theme::Type::Body, AppFont::Face::Body), tx, ty, Theme::Type::Body, Theme::Text);
        ty += 50.0f;
        const float f = st.currentTotal ? (float)((double)st.currentDone / (double)st.currentTotal) : 0.0f;
        Theme::pill(r, {tx, ty, tw, 12.0f}, 6.0f, {255, 255, 255, 30});
        if (f > 0.0f) Theme::pill(r, {tx, ty, std::max(12.0f, tw * f), 12.0f}, 6.0f, Theme::accent());
        char pct[48];
        std::snprintf(pct, sizeof(pct), "%d%%  of %.1f MB", (int)(f * 100.0f), st.currentTotal / 1048576.0);
        AppFont::draw(r, pct, tx, ty + 22.0f, Theme::Type::Caption, Theme::Muted);
        ty += 70.0f;
    } else {
        AppFont::draw(r, st.listening ? "Waiting for books" : "Not running", tx, ty, Theme::Type::Body, Theme::TextDim);
        ty += 60.0f;
    }
    if (st.received > 0) {
        char done[64];
        std::snprintf(done, sizeof(done), "Received %d %s (%.1f MB)", st.received, st.received == 1 ? "file" : "files",
                      st.receivedBytes / 1048576.0);
        AppFont::draw(r, done, tx, ty, Theme::Type::Small, Theme::accent());
        ty += 44.0f;
    }
    for (const std::string& line : st.recent) {
        if (ty > card.y + card.h - 40.0f) break;
        AppFont::draw(r, Theme::ellipsize(r, line, tw, Theme::Type::Small, AppFont::Face::Body), tx, ty, Theme::Type::Small,
                      line.find(" - failed") != std::string::npos ? Theme::Rose : Theme::TextDim);
        ty += 38.0f;
    }
}

void App::renderReader() {
    SDL_Renderer* r = m_renderer;
    SDL_RenderSetScale(r, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(r, 8, 9, 14, 255);
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
    const bool sliding = m_slideT < 1.0f && m_slideFrom >= 0;
    if (sliding) draw(m_slideFrom, -m_slideDir * e * m_canvasW);
    const bool shown = draw(m_page, sliding ? m_slideDir * (1.0f - e) * m_canvasW : 0.0f);

    SDL_RenderSetScale(r, m_canvasScale, m_canvasScale);
    const int n = m_pages.pageCount();
    if (n == -2) {
        Theme::emptyState(r, kW, "Can't open this book", Theme::ellipsize(r, m_pages.openError(), kW - 80.0f, Theme::Type::Small));
        Theme::footerHints(r, kW, "B Library", "");
        return;
    }
    auto it = m_pageTex.find(m_page);
    if (!shown && !sliding) {
        if (it != m_pageTex.end() && it->second.failed)
            Theme::emptyState(r, kW, "Can't show page " + std::to_string(m_page + 1),
                              Theme::ellipsize(r, it->second.error, kW - 80.0f, Theme::Type::Small));
        else
            Theme::emptyState(r, kW, n > 0 ? "Page " + std::to_string(m_page + 1) : "Opening...", "");
    }
    if (m_hud > 0.0f) {
        const float a = std::min(1.0f, m_hud / 0.4f);
        const Uint8 bg = (Uint8)(215 * a), fg = (Uint8)(255 * a);
        // The title at the top, the page at the bottom.
        const std::string title = Theme::ellipsize(r, m_book.title, kW - 120.0f, Theme::Type::Body);
        const float tw = AppFont::measureWidth(r, title, Theme::Type::Body) + 56.0f;
        Gfx::roundRect(r, {(kW - tw) * 0.5f, 30.0f, tw, 58.0f}, 29.0f, {12, 16, 28, bg});
        AppFont::drawCentered(r, title, kW * 0.5f, 42.0f, Theme::Type::Body, Gfx::withAlpha(Theme::Text, fg));
        if (m_pagesN > 0) {
            const std::string pg = std::to_string(m_page + 1) + " / " + std::to_string(m_pagesN);
            const float pw = 300.0f;
            const FRect rc{(kW - pw) * 0.5f, kH - 150.0f, pw, 100.0f};
            Gfx::roundRect(r, rc, 24.0f, {12, 16, 28, bg});
            AppFont::drawCentered(r, pg, kW * 0.5f, rc.y + 12.0f, Theme::Type::Heading, Gfx::withAlpha(Theme::Text, fg), AppFont::Face::Display);
            const float f = (m_page + 1) / (float)m_pagesN;
            Gfx::roundRect(r, {rc.x + 30.0f, rc.y + 70.0f, rc.w - 60.0f, 8.0f}, 4.0f, {255, 255, 255, (Uint8)(40 * a)});
            Gfx::roundRect(r, {rc.x + 30.0f, rc.y + 70.0f, (rc.w - 60.0f) * f, 8.0f}, 4.0f, Gfx::withAlpha(Theme::accent(), fg));
        }
    }
}

}  // namespace

int runApp(const std::string& appDir) {
    Library::ensureFolders(appDir);
    Library::openLog(appDir);
    Library::installCrashLog();
    log("Comic Reader %s, app folder %s", kVersion, appDir.c_str());
    CpuPin::enabled() = true;  // before any worker thread starts
    App app(appDir);
    return app.run();
}
