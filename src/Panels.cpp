#include "Panels.h"

#include "Covers.h"
#include "CpuPin.h"
#include "DrmKms.h"
#include "Library.h"
#include "PanelFont.h"

#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The drawing helpers are Retro Launcher's GamePanels ones, Neon palette only.

namespace {

using Library::log;

constexpr uint8_t kInk[3] = {245, 246, 250};
constexpr uint8_t kGold[3] = {245, 214, 95};
constexpr uint8_t kTeal[3] = {43, 199, 184};
constexpr uint8_t kViolet[3] = {124, 92, 232};
constexpr uint8_t kGradTop[3] = {16, 30, 46};
constexpr uint8_t kGradBot[3] = {30, 16, 50};

int findCard0Fd() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int best = -1;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[64], target[256];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        ssize_t n = ::readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (std::strstr(target, "/dev/dri/card")) { best = std::atoi(e->d_name); break; }
    }
    ::closedir(d);
    return best;
}

void gradient(std::vector<uint8_t>& c, int W, int H) {
    for (int y = 0; y < H; ++y) {
        float t = H > 1 ? (float)y / (H - 1) : 0.0f;
        uint8_t r = (uint8_t)(kGradTop[0] + (kGradBot[0] - kGradTop[0]) * t),
                g = (uint8_t)(kGradTop[1] + (kGradBot[1] - kGradTop[1]) * t),
                b = (uint8_t)(kGradTop[2] + (kGradBot[2] - kGradTop[2]) * t);
        uint8_t* row = &c[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x) { row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255; }
    }
}

void glow(std::vector<uint8_t>& c, int W, int H, float cx, float cy, float radius, const uint8_t col[3], float strength) {
    int x0 = std::max(0, (int)(cx - radius)), x1 = std::min(W, (int)(cx + radius));
    int y0 = std::max(0, (int)(cy - radius)), y1 = std::min(H, (int)(cy + radius));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
            if (d >= 1.0f) continue;
            float a = (1.0f - d) * (1.0f - d) * strength;
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::min(255.0f, p[k] + col[k] * a);
        }
}

// Text with its left edge at x (align 0), centered on x (1), or ending at x (2).
void text(std::vector<uint8_t>& c, int W, int H, const std::string& s, float x, float top, float px, const uint8_t col[3],
          PanelFont::Face face, float maxW, int align = 1) {
    if (s.empty()) return;
    float w = PanelFont::measure(s, px, face);
    if (w > maxW && w > 0) { px *= maxW / w; w = maxW; }
    float left = align == 0 ? x : align == 1 ? x - w * 0.5f : x - w;
    PanelFont::draw(c.data(), W, H, s, left, top, px, col[0], col[1], col[2], face);
}

void bar(std::vector<uint8_t>& c, int W, int H, int x, int y, int w, int h, float f) {
    for (int yy = y; yy < y + h && yy < H; ++yy)
        for (int xx = x; xx < x + w && xx < W; ++xx) {
            if (xx < 0 || yy < 0) continue;
            uint8_t* p = &c[((size_t)yy * W + xx) * 4];
            const bool on = xx < x + (int)(w * f);
            const float t = w > 1 ? (float)(xx - x) / (w - 1) : 0.0f;
            for (int k = 0; k < 3; ++k) {
                uint8_t v = on ? (uint8_t)(kTeal[k] + (kViolet[k] - kTeal[k]) * t) : (uint8_t)(p[k] * 0.6f + 255 * 0.12f);
                p[k] = v;
            }
        }
}

// Blurred, darkened backdrop: average to a tiny grid, bilinear back up.
void backdrop(std::vector<uint8_t>& c, int W, int H, const std::vector<uint8_t>& img, int iw, int ih) {
    constexpr int SB = 18;
    float small[SB * SB * 3];
    for (int sy = 0; sy < SB; ++sy)
        for (int sx = 0; sx < SB; ++sx) {
            int x0 = sx * iw / SB, x1 = std::max(x0 + 1, (sx + 1) * iw / SB);
            int y0 = sy * ih / SB, y1 = std::max(y0 + 1, (sy + 1) * ih / SB);
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    const uint8_t* s = &img[((size_t)y * iw + x) * 4];
                    r += s[0]; g += s[1]; b += s[2]; ++n;
                }
            float* d = &small[(sy * SB + sx) * 3];
            d[0] = r / n; d[1] = g / n; d[2] = b / n;
        }
    const float dim = 0.32f;
    for (int y = 0; y < H; ++y) {
        float fv = (y + 0.5f) / H * SB - 0.5f;
        int v0 = (int)std::floor(fv);
        float vt = fv - v0;
        int a0 = std::min(std::max(v0, 0), SB - 1), a1 = std::min(std::max(v0 + 1, 0), SB - 1);
        for (int x = 0; x < W; ++x) {
            float fu = (x + 0.5f) / W * SB - 0.5f;
            int u0 = (int)std::floor(fu);
            float ut = fu - u0;
            int b0 = std::min(std::max(u0, 0), SB - 1), b1 = std::min(std::max(u0 + 1, 0), SB - 1);
            uint8_t* o = &c[((size_t)y * W + x) * 4];
            for (int k = 0; k < 3; ++k) {
                float t = small[(a0 * SB + b0) * 3 + k] * (1 - ut) + small[(a0 * SB + b1) * 3 + k] * ut;
                float bo = small[(a1 * SB + b0) * 3 + k] * (1 - ut) + small[(a1 * SB + b1) * 3 + k] * ut;
                o[k] = (uint8_t)((t * (1 - vt) + bo * vt) * dim);
            }
            o[3] = 255;
        }
    }
}

// Bilinear, aspect-fit blit into a box (aligned left when left is set), with a
// soft shadow. Returns the drawn rectangle's right edge.
int blitFit(std::vector<uint8_t>& c, int W, int H, const std::vector<uint8_t>& img, int iw, int ih, int bx, int by, int bw,
            int bh, bool left) {
    float s = std::min((float)bw / iw, (float)bh / ih);
    int dw = std::max(1, (int)(iw * s)), dh = std::max(1, (int)(ih * s));
    int ox = left ? bx : bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
    const int sh = std::max(8, H / 60);
    for (int y = oy - sh; y < oy + dh + sh * 2; ++y)
        for (int x = ox - sh; x < ox + dw + sh * 2; ++x) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float dx = std::max(0, std::max(ox - x, x - (ox + dw - 1) - sh));
            float dy = std::max(0, std::max(oy - y, y - (oy + dh - 1) - sh));
            float d = std::sqrt(dx * dx + dy * dy) / (sh * 1.5f);
            if (d >= 1.0f) continue;
            uint8_t* p = &c[((size_t)y * W + x) * 4];
            float k = 1.0f - 0.55f * (1.0f - d);
            p[0] = (uint8_t)(p[0] * k); p[1] = (uint8_t)(p[1] * k); p[2] = (uint8_t)(p[2] * k);
        }
    for (int y = 0; y < dh; ++y) {
        int cy = oy + y;
        if (cy < 0 || cy >= H) continue;
        float fv = (y + 0.5f) / dh * ih - 0.5f;
        int v0 = std::min(std::max((int)std::floor(fv), 0), ih - 1), v1 = std::min(v0 + 1, ih - 1);
        float vt = std::max(0.0f, fv - v0);
        for (int x = 0; x < dw; ++x) {
            int cx = ox + x;
            if (cx < 0 || cx >= W) continue;
            float fu = (x + 0.5f) / dw * iw - 0.5f;
            int u0 = std::min(std::max((int)std::floor(fu), 0), iw - 1), u1 = std::min(u0 + 1, iw - 1);
            float ut = std::max(0.0f, fu - u0);
            const uint8_t *s00 = &img[((size_t)v0 * iw + u0) * 4], *s10 = &img[((size_t)v0 * iw + u1) * 4],
                          *s01 = &img[((size_t)v1 * iw + u0) * 4], *s11 = &img[((size_t)v1 * iw + u1) * 4];
            uint8_t* q = &c[((size_t)cy * W + cx) * 4];
            for (int k = 0; k < 3; ++k)
                q[k] = (uint8_t)((s00[k] * (1 - ut) + s10[k] * ut) * (1 - vt) + (s01[k] * (1 - ut) + s11[k] * ut) * vt);
        }
    }
    return ox + dw;
}

std::string pageLine(int page, int pages) {
    if (pages <= 0) return "";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Page %d of %d", page + 1, pages);
    return buf;
}

}  // namespace

Panels::~Panels() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    for (Panel& p : m_panels)
        if (p.map) ::munmap(p.map, p.size);
}

uint32_t Panels::pickCrtc(uint32_t currentEncoder, const std::vector<uint32_t>& encoders, uint32_t count, uint32_t mainConnector) {
    if (currentEncoder) {
        drm_mode_get_encoder en;
        std::memset(&en, 0, sizeof(en));
        en.encoder_id = currentEncoder;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &en) == 0 && en.crtc_id) return en.crtc_id;
    }
    drm_mode_card_res res;
    std::memset(&res, 0, sizeof(res));
    if (::ioctl(m_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_crtcs == 0) return 0;
    std::vector<uint32_t> crtcs(res.count_crtcs);
    res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs.data());
    res.count_connectors = res.count_encoders = res.count_fbs = 0;
    res.connector_id_ptr = res.encoder_id_ptr = res.fb_id_ptr = 0;
    if (::ioctl(m_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) return 0;
    std::vector<uint32_t> taken;
    for (const Panel& p : m_panels) taken.push_back(p.crtcId);
    if (mainConnector) {
        drm_mode_get_connector mc;
        std::memset(&mc, 0, sizeof(mc));
        mc.connector_id = mainConnector;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &mc) == 0 && mc.encoder_id) {
            drm_mode_get_encoder me;
            std::memset(&me, 0, sizeof(me));
            me.encoder_id = mc.encoder_id;
            if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &me) == 0 && me.crtc_id) taken.push_back(me.crtc_id);
        }
    }
    for (uint32_t e = 0; e < count && e < encoders.size(); ++e) {
        drm_mode_get_encoder en;
        std::memset(&en, 0, sizeof(en));
        en.encoder_id = encoders[e];
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETENCODER, &en) != 0) continue;
        for (uint32_t i = 0; i < crtcs.size() && i < 32; ++i) {
            if (!(en.possible_crtcs & (1u << i))) continue;
            if (std::find(taken.begin(), taken.end(), crtcs[i]) != taken.end()) continue;
            return crtcs[i];
        }
    }
    return 0;
}

int Panels::init(const DisplayProfile::Topology& topo, const std::string& appDir) {
    m_appDir = appDir;
    m_fd = findCard0Fd();
    if (m_fd < 0) { log("panels: no card0 fd"); return 0; }
    for (const DisplayProfile::Screen* scr : {&topo.backglass, &topo.dmd}) {
        if (!scr->available || scr->connectorId == 0 || scr->connectorId == topo.main.connectorId) continue;
        drm_mode_get_connector cn;
        std::memset(&cn, 0, sizeof(cn));
        cn.connector_id = scr->connectorId;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn) != 0 || cn.count_modes == 0) {
            log("panels: connector %u unavailable", scr->connectorId);
            continue;
        }
        std::vector<drm_mode_modeinfo> modes(cn.count_modes);
        std::vector<uint32_t> encoders(cn.count_encoders ? cn.count_encoders : 1);
        cn.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
        cn.encoders_ptr = reinterpret_cast<uint64_t>(encoders.data());
        cn.count_props = 0;
        cn.props_ptr = cn.prop_values_ptr = 0;
        ::ioctl(m_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cn);
        uint32_t crtc = pickCrtc(cn.encoder_id, encoders, cn.count_encoders, topo.main.connectorId);
        if (!crtc) { log("panels: no display controller for connector %u", scr->connectorId); continue; }
        const drm_mode_modeinfo mode = modes[0];

        drm_mode_create_dumb cd;
        std::memset(&cd, 0, sizeof(cd));
        cd.width = mode.hdisplay; cd.height = mode.vdisplay; cd.bpp = 32;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) { log("panels: CREATE_DUMB %s", std::strerror(errno)); continue; }
        drm_mode_fb_cmd fbc;
        std::memset(&fbc, 0, sizeof(fbc));
        fbc.width = cd.width; fbc.height = cd.height; fbc.pitch = cd.pitch; fbc.bpp = 32; fbc.depth = 24; fbc.handle = cd.handle;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_ADDFB, &fbc) != 0) { log("panels: ADDFB %s", std::strerror(errno)); continue; }
        drm_mode_map_dumb md;
        std::memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) continue;
        uint8_t* map = (uint8_t*)::mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, md.offset);
        if (map == MAP_FAILED) continue;

        Panel p;
        p.connId = scr->connectorId; p.crtcId = crtc; p.fbId = fbc.fb_id; p.handle = cd.handle;
        p.pitch = cd.pitch; p.w = mode.hdisplay; p.h = mode.vdisplay; p.map = map; p.size = cd.size;
        p.dmd = scr == &topo.dmd;
        p.rot = p.h > p.w;  // portrait-native buffer (the DMD): compose landscape, present turned
        p.vw = p.rot ? p.h : p.w;
        p.vh = p.rot ? p.w : p.h;
        p.canvas.assign((size_t)p.vw * p.vh * 4, 0);

        drm_mode_crtc sc;
        std::memset(&sc, 0, sizeof(sc));
        sc.crtc_id = p.crtcId; sc.fb_id = p.fbId;
        sc.set_connectors_ptr = reinterpret_cast<uint64_t>(&p.connId);
        sc.count_connectors = 1; sc.mode = mode; sc.mode_valid = 1;
        if (::ioctl(m_fd, DRM_IOCTL_MODE_SETCRTC, &sc) != 0) {
            log("panels: SETCRTC %u: %s", p.connId, std::strerror(errno));
            ::munmap(map, cd.size);
            continue;
        }
        log("panels: %s up on connector %u (%dx%d%s)", p.dmd ? "DMD" : "backglass", p.connId, p.w, p.h, p.rot ? ", rotated" : "");
        m_panels.push_back(std::move(p));
    }
    if (!m_panels.empty()) m_thread = std::thread(&Panels::worker, this);
    return (int)m_panels.size();
}

void Panels::show(const Item& item) {
    if (m_panels.empty()) return;
    const std::string key = item.rel + "|" + item.title + "|" + item.subtitle + "|" + std::to_string(item.page) + "|" +
                            std::to_string(item.pages) + (item.reading ? "|r" : "|b");
    if (key == m_lastKey) return;
    m_lastKey = key;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job = item;
        m_hasJob = true;
    }
    m_cv.notify_one();
}

void Panels::worker() {
    if (CpuPin::enabled()) CpuPin::pinSelf(CpuPin::groups().slow);
    cr_ctx* ctx = cr_ctx_new();
    std::vector<uint8_t> cover;
    int cw = 0, ch = 0;
    for (;;) {
        Item job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || m_hasJob; });
            if (m_stop) break;
            job = m_job;
            m_hasJob = false;
        }
        auto t0 = std::chrono::steady_clock::now();
        if (job.rel != m_lastCoverRel) {
            m_lastCoverRel = job.rel;
            cover.clear();
            cw = ch = 0;
            if (ctx && !job.rel.empty() && !Covers::load(ctx, m_appDir, job.rel, cover, cw, ch)) cover.clear();
        }
        // The backglass shows the book and how far in; the DMD the page.
        // The backglass changes only with the cover; the DMD with the page too.
        const std::string bgKey = job.rel.empty() ? "|" + job.title : job.rel;
        const std::string dmdKey = job.rel + "|" + job.title + "|" + job.subtitle + "|" + pageLine(job.page, job.pages) +
                                   (job.reading ? "|r" : "|b");
        for (Panel& p : m_panels) {
            if (p.dmd) {
                if (dmdKey == m_shownDmd) continue;
                composeDmd(p, job);
            } else {
                if (bgKey == m_shownBg) continue;
                composeBackglass(p, job, cover, cw, ch);
            }
            present(p);
        }
        m_shownBg = bgKey;
        m_shownDmd = dmdKey;
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 40) log("panels: %s in %ld ms", job.title.c_str(), ms);
    }
    if (ctx) cr_ctx_drop(ctx);
}

void Panels::composeBackglass(Panel& p, const Item& item, const std::vector<uint8_t>& cover, int cw, int ch) {
    const int W = p.vw, H = p.vh;
    std::vector<uint8_t>& c = p.canvas;
    if (cover.empty()) {
        gradient(c, W, H);
        glow(c, W, H, W * 0.18f, H * 0.2f, H * 0.9f, kTeal, 0.22f);
        glow(c, W, H, W * 0.85f, H * 0.85f, H * 1.0f, kViolet, 0.25f);
        text(c, W, H, item.title.empty() ? "COMIC READER" : item.subtitle, W * 0.5f, H * 0.22f, H * 0.06f, kTeal,
             PanelFont::Face::Display, W * 0.8f);
        text(c, W, H, item.title.empty() ? "Pick a book on the playfield" : item.title, W * 0.5f, H * 0.38f, H * 0.14f, kInk,
             PanelFont::Face::Display, W * 0.88f);
        text(c, W, H, pageLine(item.page, item.pages), W * 0.5f, H * 0.62f, H * 0.055f, kGold, PanelFont::Face::Body, W * 0.8f);
        return;
    }
    // Just the cover, centered over a blurred copy of itself: the words are on the DMD.
    backdrop(c, W, H, cover, cw, ch);
    const int pad = (int)(H * 0.06f);
    blitFit(c, W, H, cover, cw, ch, pad, pad, W - 2 * pad, H - 2 * pad, false);
}

void Panels::composeDmd(Panel& p, const Item& item) {
    const int W = p.vw, H = p.vh;
    std::vector<uint8_t>& c = p.canvas;
    // The lit background is the slow part (365 ms on the cabinet's small
    // cores): drawn once, then copied for every page.
    if (m_dmdGround.size() != c.size()) {
        m_dmdGround.assign(c.size(), 0);
        gradient(m_dmdGround, W, H);
        glow(m_dmdGround, W, H, W * 0.1f, H * 0.5f, H * 1.4f, kTeal, 0.14f);
        glow(m_dmdGround, W, H, W * 0.92f, H * 0.5f, H * 1.4f, kViolet, 0.16f);
    }
    c = m_dmdGround;
    if (item.title.empty()) {
        text(c, W, H, "COMIC READER", W * 0.5f, H * 0.3f, H * 0.36f, kInk, PanelFont::Face::Display, W * 0.9f);
        return;
    }
    if (item.reading && item.pages > 0) {
        text(c, W, H, item.title, W * 0.5f, H * 0.1f, H * 0.2f, kInk, PanelFont::Face::Display, W * 0.92f);
        char big[32];
        std::snprintf(big, sizeof(big), "%d / %d", item.page + 1, item.pages);
        text(c, W, H, big, W * 0.5f, H * 0.36f, H * 0.34f, kGold, PanelFont::Face::Display, W * 0.9f);
        bar(c, W, H, (int)(W * 0.08f), (int)(H * 0.82f), (int)(W * 0.84f), std::max(6, H / 30), (item.page + 1) / (float)item.pages);
        return;
    }
    text(c, W, H, item.subtitle, W * 0.5f, H * 0.14f, H * 0.16f, kTeal, PanelFont::Face::Display, W * 0.9f);
    text(c, W, H, item.title, W * 0.5f, H * 0.38f, H * 0.3f, kInk, PanelFont::Face::Display, W * 0.92f);
    text(c, W, H, pageLine(item.page, item.pages), W * 0.5f, H * 0.76f, H * 0.12f, kGold, PanelFont::Face::Body, W * 0.9f);
}

void Panels::present(Panel& p) {
    const uint8_t* c = p.canvas.data();
    for (int vy = 0; vy < p.vh; ++vy) {
        const uint8_t* srow = c + (size_t)vy * p.vw * 4;
        if (!p.rot) {
            uint32_t* dst = reinterpret_cast<uint32_t*>(p.map + (size_t)vy * p.pitch);
            for (int vx = 0; vx < p.vw; ++vx) {
                const uint8_t* s = srow + vx * 4;
                dst[vx] = (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
            }
        } else {
            for (int vx = 0; vx < p.vw; ++vx) {  // 90° CCW for the portrait-native DMD
                const uint8_t* s = srow + vx * 4;
                int bx = p.w - 1 - vy, by = vx;
                *reinterpret_cast<uint32_t*>(p.map + (size_t)by * p.pitch + bx * 4) =
                    (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
            }
        }
    }
}
