#include "Covers.h"

#include "CpuPin.h"
#include "Library.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

using Library::log;

namespace Covers {

bool load(cr_ctx* ctx, const std::string& appDir, const std::string& rel, std::vector<uint8_t>& out, int& w, int& h) {
    const std::string cache = Library::coverCachePath(appDir, rel);
    struct stat st;
    if (::stat(cache.c_str(), &st) == 0 && st.st_size > 0) {
        if (unsigned char* px = cr_load_image(ctx, cache.c_str(), &w, &h)) {
            out.assign(px, px + (size_t)w * h * 4);
            std::free(px);
            return true;
        }
        ::unlink(cache.c_str());  // unreadable: draw it again
    }
    auto t0 = std::chrono::steady_clock::now();
    char err[256] = {0};
    cr_book* book = cr_open(ctx, (appDir + "/" + rel).c_str(), err, sizeof(err));
    if (!book) {
        log("cover: %s: %s", rel.c_str(), err);
        return false;
    }
    unsigned char* px = cr_render(book, 0, kCoverW, kCoverH, &w, &h, err, sizeof(err));
    cr_close(book);
    if (!px) {
        log("cover: %s page 1: %s", rel.c_str(), err);
        return false;
    }
    out.assign(px, px + (size_t)w * h * 4);
    std::free(px);
    // Two threads can draw the same cover at once (the grid and the backglass):
    // each writes its own temporary file.
    const std::string tmp = cache + "." + std::to_string((unsigned long)std::hash<std::thread::id>()(std::this_thread::get_id())) + ".tmp";
    if (cr_save_jpeg(ctx, out.data(), w, h, tmp.c_str(), 85) == 0) std::rename(tmp.c_str(), cache.c_str());
    else ::unlink(tmp.c_str());
    long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    log("cover: %s drawn in %ld ms", rel.c_str(), ms);
    return true;
}

}  // namespace Covers

CoverLoader::~CoverLoader() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void CoverLoader::start(const std::string& appDir) {
    m_appDir = appDir;
    m_thread = std::thread(&CoverLoader::worker, this);
}

void CoverLoader::request(const std::string& rel) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_asked.count(rel)) return;
        m_asked[rel] = true;
        m_queue.push_back(rel);
    }
    m_cv.notify_one();
}

bool CoverLoader::take(Result& out) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_done.empty()) return false;
    out = std::move(m_done.front());
    m_done.pop_front();
    return true;
}

void CoverLoader::forget(const std::string& rel) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_asked.erase(rel);
}

void CoverLoader::worker() {
    // Covers stay off the fast cores, which the page renderer uses.
    if (CpuPin::enabled()) CpuPin::pinSelf(CpuPin::groups().slow);
    cr_ctx* ctx = cr_ctx_new();
    if (!ctx) {
        log("covers: no MuPDF context");
        return;
    }
    for (;;) {
        std::string rel;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) break;
            rel = m_queue.back();  // newest first: what is on screen now
            m_queue.pop_back();
        }
        Result r;
        r.rel = rel;
        r.ok = Covers::load(ctx, m_appDir, rel, r.rgba, r.w, r.h);
        std::lock_guard<std::mutex> lock(m_mu);
        m_done.push_back(std::move(r));
    }
    cr_ctx_drop(ctx);
}
