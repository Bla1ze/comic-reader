#include "Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace Theme {

namespace {

constexpr int kLogicalW = 720, kLogicalH = 1280;

SDL_Texture* g_paper = nullptr;
SDL_Texture* g_star = nullptr;

float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Newsprint: a warm page, darker toward the edges, with halftone dots that
// grow toward the top-left and bottom-right corners (like a printed tint).
SDL_Texture* bakePaper(SDL_Renderer* r, float scale) {
    const float s = std::max(1.0f, std::min(2.0f, scale));  // up to 2x: plenty for soft dots
    const int W = (int)(kLogicalW * s), H = (int)(kLogicalH * s);
    std::vector<uint32_t> px((size_t)W * H);
    const float cell = 13.0f * s;
    for (int y = 0; y < H; ++y) {
        const float v = (float)y / H;
        for (int x = 0; x < W; ++x) {
            const float u = (float)x / W;
            // Page tone: a little darker at the edges.
            const float edge = std::max(std::fabs(u - 0.5f), std::fabs(v - 0.5f)) * 2.0f;
            const float shade = 1.0f - 0.07f * edge * edge;
            // Dot size: strong in two corners, a light tint everywhere.
            const float d1 = clamp01(1.0f - std::sqrt(u * u + v * v * 0.6f) * 1.5f);
            const float d2 = clamp01(1.0f - std::sqrt((1 - u) * (1 - u) + (1 - v) * (1 - v) * 0.6f) * 1.5f);
            const float density = 0.18f + 0.82f * std::max(d1, d2);
            // Dots on a 45-degree grid, like a print screen.
            const float rx = (x + y) * 0.70710678f, ry = (x - y) * 0.70710678f;
            const float fx = std::fmod(rx, cell) - cell * 0.5f, fy = std::fmod(ry + 100000.0f, cell) - cell * 0.5f;
            const float dist = std::sqrt(fx * fx + fy * fy);
            const float radius = cell * 0.48f * std::sqrt(density);
            const float a = clamp01(radius - dist + 0.5f);
            float cr = Paper.r * shade, cg = Paper.g * shade, cb = Paper.b * shade;
            cr += (PaperDot.r - Paper.r) * a;
            cg += (PaperDot.g - Paper.g) * a;
            cb += (PaperDot.b - Paper.b) * a;
            px[(size_t)y * W + x] = (0xFFu << 24) | ((uint32_t)cb << 16) | ((uint32_t)cg << 8) | (uint32_t)cr;
        }
    }
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (t) SDL_UpdateTexture(t, nullptr, px.data(), W * 4);
    return t;
}

// A white starburst on transparency (tinted per draw), antialiased by 3x3
// supersampling.
SDL_Texture* bakeStar(SDL_Renderer* r) {
    const int S = 512, points = 18;
    const float cx = S * 0.5f, cy = S * 0.5f, outer = S * 0.49f, inner = S * 0.34f;
    std::vector<float> vx, vy;
    for (int i = 0; i < points * 2; ++i) {
        // Uneven spikes look hand-drawn.
        const float jitter = (i % 2 == 0) ? (i % 4 == 0 ? 1.0f : 0.9f) : (i % 6 == 1 ? 0.95f : 1.0f);
        const float rad = (i % 2 == 0 ? outer : inner) * jitter;
        const float ang = (float)i / (points * 2) * 6.2831853f;
        vx.push_back(cx + rad * std::cos(ang));
        vy.push_back(cy + rad * std::sin(ang));
    }
    auto inside = [&](float x, float y) {
        bool in = false;
        for (size_t i = 0, j = vx.size() - 1; i < vx.size(); j = i++)
            if (((vy[i] > y) != (vy[j] > y)) && (x < (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]) + vx[i])) in = !in;
        return in;
    };
    std::vector<uint32_t> px((size_t)S * S);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            int hit = 0;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) hit += inside(x + (sx + 0.5f) / 3.0f, y + (sy + 0.5f) / 3.0f);
            const uint32_t a = (uint32_t)(hit * 255 / 9);
            px[(size_t)y * S + x] = (a << 24) | 0x00FFFFFFu;
        }
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, S, S);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px.data(), S * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}

void star(SDL_Renderer* r, float cx, float cy, float radius, double degrees, SDL_Color c) {
    if (!g_star || c.a == 0) return;
    SDL_SetTextureColorMod(g_star, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(g_star, c.a);
    SDL_Rect dst{(int)std::lround(cx - radius), (int)std::lround(cy - radius), (int)std::lround(radius * 2),
                 (int)std::lround(radius * 2)};
    SDL_RenderCopyEx(r, g_star, nullptr, &dst, degrees, nullptr, SDL_FLIP_NONE);
}

}  // namespace

void init(SDL_Renderer* r, float canvasScale) {
    g_paper = bakePaper(r, canvasScale);
    g_star = bakeStar(r);
}

void shutdown() {
    if (g_paper) SDL_DestroyTexture(g_paper);
    if (g_star) SDL_DestroyTexture(g_star);
    g_paper = g_star = nullptr;
}

void paper(SDL_Renderer* r) {
    if (g_paper) {
        SDL_Rect dst{0, 0, kLogicalW, kLogicalH};
        SDL_RenderCopy(r, g_paper, nullptr, &dst);
    } else {
        Gfx::rect(r, {0, 0, (float)kLogicalW, (float)kLogicalH}, Paper);
    }
}

void inkBox(SDL_Renderer* r, const FRect& rc, SDL_Color fill, float ink, float shadow) {
    if (shadow > 0.0f) Gfx::rect(r, {rc.x + shadow, rc.y + shadow, rc.w, rc.h}, Ink);
    Gfx::rect(r, rc, Ink);
    Gfx::rect(r, {rc.x + ink, rc.y + ink, rc.w - 2 * ink, rc.h - 2 * ink}, fill);
}

void burst(SDL_Renderer* r, float cx, float cy, float radius, double degrees, SDL_Color fill, SDL_Color inner) {
    star(r, cx + kShadow, cy + kShadow, radius * 1.05f, degrees, Ink);  // the shadow
    star(r, cx, cy, radius * 1.05f, degrees, Ink);                      // the outline
    star(r, cx, cy, radius, degrees, fill);
    if (inner.a) star(r, cx, cy, radius * 0.72f, degrees + 7.0, inner);
}

void bubble(SDL_Renderer* r, const FRect& rc, float tailX) {
    const float rad = std::min(36.0f, rc.h * 0.4f);
    const FRect tail{tailX - 26.0f, rc.y + rc.h - 18.0f, 52.0f, 60.0f};
    // Outline and shadow first, then the white on top.
    Gfx::roundRect(r, {rc.x - kInk + kShadow, rc.y - kInk + kShadow, rc.w + 2 * kInk, rc.h + 2 * kInk}, rad + kInk, Ink);
    Gfx::roundRect(r, {rc.x - kInk, rc.y - kInk, rc.w + 2 * kInk, rc.h + 2 * kInk}, rad + kInk, Ink);
    Gfx::triangle(r, {tail.x - kInk, tail.y, tail.w + 2 * kInk, tail.h + 2 * kInk}, 100.0, Ink);
    Gfx::roundRect(r, rc, rad, White);
    Gfx::triangle(r, tail, 100.0, White);
}

void progress(SDL_Renderer* r, const FRect& rc, float f, SDL_Color fill) {
    inkBox(r, rc, White, 3.0f, 0.0f);
    const float w = (rc.w - 6.0f) * clamp01(f);
    if (w > 0.0f) Gfx::rect(r, {rc.x + 3.0f, rc.y + 3.0f, w, rc.h - 6.0f}, fill);
}

float width(SDL_Renderer* r, const std::string& s, float size, AppFont::Face face) {
    return AppFont::measureWidth(r, s, size, face);
}

void letter(SDL_Renderer* r, const std::string& s, float x, float y, float size, SDL_Color fill, float outline) {
    const AppFont::Face D = AppFont::Face::Display;
    const float o = outline;
    AppFont::draw(r, s, x + o * 1.8f, y + o * 1.8f, size, Ink, D);  // shadow
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy) AppFont::draw(r, s, x + dx * o, y + dy * o, size, Ink, D);
    AppFont::draw(r, s, x, y, size, fill, D);
}

void letterCentered(SDL_Renderer* r, const std::string& s, float cx, float y, float size, SDL_Color fill, float outline) {
    letter(r, s, cx - width(r, s, size, AppFont::Face::Display) * 0.5f, y, size, fill, outline);
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string ellipsize(SDL_Renderer* r, std::string s, float maxW, float size, AppFont::Face face) {
    if (width(r, s, size, face) <= maxW) return s;
    while (!s.empty() && width(r, s + "...", size, face) > maxW) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();  // whole UTF-8 characters
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + "...";
}

float fitSize(SDL_Renderer* r, const std::string& s, float maxW, float size, float minSize, AppFont::Face face) {
    while (size > minSize && width(r, s, size, face) > maxW) size -= 2.0f;
    return std::max(size, minSize);
}

void masthead(SDL_Renderer* r) {
    const FRect band{kMargin - 14.0f, 18.0f, kLogicalW - 2.0f * (kMargin - 14.0f), 112.0f};
    inkBox(r, band, Red);
    // A few big print dots across the band.
    for (int i = 0; i < 9; ++i) {
        const float x = band.x + band.w - 40.0f - i * 26.0f, y = band.y + 26.0f + (i % 3) * 28.0f;
        Gfx::disc(r, x, y, 7.0f - i * 0.5f, {255, 255, 255, 40});
    }
    letterCentered(r, "COMIC READER!", kLogicalW * 0.5f, band.y + 18.0f, Type::Title, Yellow, 4.0f);
}

void footerHints(SDL_Renderer* r, const std::string& line) {
    const FRect box{20.0f, kFooterTop + 14.0f, kLogicalW - 40.0f, 82.0f};
    inkBox(r, box, Yellow);
    struct Item { std::string key, label; };
    std::vector<Item> items;
    for (size_t i = 0; i < line.size();) {
        size_t end = line.find("   ", i);
        if (end == std::string::npos) end = line.size();
        std::string seg = line.substr(i, end - i);
        i = end;
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t f = seg.find_first_not_of(' ');
        if (f == std::string::npos) continue;
        seg = seg.substr(f);
        const size_t sp = seg.find(' ');
        items.push_back({seg.substr(0, sp), sp == std::string::npos ? "" : upper(seg.substr(sp + 1))});
    }
    const AppFont::Face D = AppFont::Face::Display;
    float size = 30.0f, gap = 30.0f;
    auto chipW = [&](const std::string& k) { return std::max(46.0f, width(r, k, 26.0f, D) + 24.0f); };
    auto total = [&]() {
        float t = 0.0f;
        for (const Item& it : items) t += chipW(it.key) + (it.label.empty() ? 0.0f : 10.0f + width(r, it.label, size, D));
        return t + gap * (items.size() > 0 ? items.size() - 1 : 0);
    };
    while (total() > box.w - 40.0f && size > 20.0f) { size -= 2.0f; gap = std::max(16.0f, gap - 3.0f); }
    float x = box.x + (box.w - total()) * 0.5f;
    const float cy = box.y + box.h * 0.5f;
    for (const Item& it : items) {
        const float w = chipW(it.key);
        Gfx::roundRect(r, {x - 3.0f, cy - 26.0f, w + 6.0f, 52.0f}, 26.0f, Ink);
        Gfx::roundRect(r, {x, cy - 23.0f, w, 46.0f}, 23.0f, it.key == "A" ? Red : White);
        AppFont::drawCentered(r, it.key, x + w * 0.5f, cy - 16.0f, 26.0f, it.key == "A" ? White : Ink, D);
        x += w;
        if (!it.label.empty()) {
            x += 10.0f;
            AppFont::draw(r, it.label, x, cy - size * 0.55f, size, Ink, D);
            x += width(r, it.label, size, D);
        }
        x += gap;
    }
}

void confirmDialog(SDL_Renderer* r, const std::string& question, const std::string& cancelLabel,
                   const std::string& confirmLabel, int selected) {
    Gfx::rect(r, {0, 0, (float)kLogicalW, (float)kLogicalH}, {24, 20, 18, 150});
    const FRect b{70.0f, 360.0f, kLogicalW - 140.0f, 430.0f};
    burst(r, b.x + b.w - 20.0f, b.y + 10.0f, 70.0f, 12.0, Yellow, {0, 0, 0, 0});
    bubble(r, b, b.x + b.w * 0.3f);
    AppFont::drawCentered(r, upper(question), kLogicalW * 0.5f, b.y + 48.0f, Type::Heading, Ink, AppFont::Face::Display);
    const float bw = b.w - 120.0f, bx = kLogicalW * 0.5f - bw * 0.5f;
    const std::string labels[2] = {cancelLabel, confirmLabel};
    for (int i = 0; i < 2; ++i) {
        const FRect btn{bx, b.y + 150.0f + i * 112.0f, bw, 86.0f};
        const bool on = i == selected;
        inkBox(r, btn, on ? (i == 1 ? Red : Yellow) : White);
        AppFont::drawCentered(r, upper(labels[i]), btn.x + btn.w * 0.5f, btn.y + 20.0f, Type::Heading,
                              on && i == 1 ? White : Ink, AppFont::Face::Display);
    }
}

void emptyState(SDL_Renderer* r, const std::string& big, const std::string& hint) {
    burst(r, kLogicalW * 0.5f, 560.0f, 210.0f, 6.0, Yellow, {0, 0, 0, 0});
    letterCentered(r, upper(big), kLogicalW * 0.5f, 520.0f, Type::Heading, Red, 3.0f);
    if (!hint.empty()) {
        const float w = std::min(kLogicalW - 80.0f, width(r, hint, Type::Small) + 60.0f);
        const FRect cap{(kLogicalW - w) * 0.5f, 820.0f, w, 56.0f};
        inkBox(r, cap, White);
        AppFont::drawCentered(r, ellipsize(r, hint, w - 30.0f, Type::Small), kLogicalW * 0.5f, cap.y + 14.0f, Type::Small, Ink);
    }
}

void toast(SDL_Renderer* r, const std::string& text, float alpha) {
    if (alpha <= 0.0f || text.empty()) return;
    const float w = std::min(kLogicalW - 80.0f, width(r, upper(text), 30.0f, AppFont::Face::Display) + 70.0f);
    const FRect b{(kLogicalW - w) * 0.5f, kFooterTop - 150.0f, w, 70.0f};
    bubble(r, b, b.x + w * 0.75f);
    AppFont::drawCentered(r, upper(text), kLogicalW * 0.5f, b.y + 18.0f, 30.0f, Ink, AppFont::Face::Display);
}

}  // namespace Theme
