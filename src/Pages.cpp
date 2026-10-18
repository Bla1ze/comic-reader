#include "Pages.h"

#include "CpuPin.h"
#include "Doc.h"
#include "Library.h"

#include <chrono>
#include <cstdlib>

using Library::log;

PageRenderer::~PageRenderer() {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void PageRenderer::start() {
    m_thread = std::thread(&PageRenderer::worker, this);
}

int PageRenderer::open(const std::string& path, int maxW, int maxH) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_path = path;
    m_maxW = maxW;
    m_maxH = maxH;
    m_page = 0;
    m_have.clear();
    m_error.clear();
    m_done.clear();
    m_pages = -1;
    const int g = ++m_gen;
    m_cv.notify_one();
    return g;
}

void PageRenderer::close() {
    std::lock_guard<std::mutex> lock(m_mu);
    m_path.clear();
    m_done.clear();
    m_pages = -1;
    ++m_gen;
    m_cv.notify_one();
}

void PageRenderer::want(int page, const std::set<int>& have) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_page = page;
    m_have = have;
    m_cv.notify_one();
}

bool PageRenderer::take(Result& out) {
    std::lock_guard<std::mutex> lock(m_mu);
    while (!m_done.empty()) {
        Result r = std::move(m_done.front());
        m_done.pop_front();
        if (r.generation != m_gen.load()) continue;  // from a book since closed
        out = std::move(r);
        return true;
    }
    return false;
}

std::string PageRenderer::openError() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_error;
}

void PageRenderer::worker() {
    // Page drawing is the one job that has to be quick: the fast cores.
    if (CpuPin::enabled()) CpuPin::pinSelf(CpuPin::groups().fast);
    cr_ctx* ctx = cr_ctx_new();
    if (!ctx) {
        log("pages: no MuPDF context");
        return;
    }
    cr_book* book = nullptr;
    int bookGen = 0, pages = 0;
    std::string bookPath;
    std::set<int> sent;  // drawn for this generation and not yet known to be held
    for (;;) {
        std::string path;
        int gen, page, maxW, maxH;
        std::set<int> have;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [&] {
                if (m_stop) return true;
                if (m_path.empty()) return book != nullptr;  // close the book
                if (m_gen.load() != bookGen) return true;    // open another
                if (pages <= 0) return false;
                for (int d = 0; d <= kAhead; ++d)
                    for (int s : {1, -1}) {
                        const int p = m_page + d * s;
                        if ((s < 0 && d > kBehind) || p < 0 || p >= pages) continue;
                        if (!m_have.count(p) && !sent.count(p)) return true;
                    }
                return false;
            });
            if (m_stop) break;
            path = m_path;
            gen = m_gen.load();
            page = m_page;
            have = m_have;
            maxW = m_maxW;
            maxH = m_maxH;
        }
        if (gen != bookGen || path != bookPath) {
            if (book) cr_close(book);
            book = nullptr;
            pages = 0;
            sent.clear();
            bookGen = gen;
            bookPath = path;
            if (path.empty()) continue;
            auto t0 = std::chrono::steady_clock::now();
            char err[256] = {0};
            book = cr_open(ctx, path.c_str(), err, sizeof(err));
            pages = book ? cr_page_count(book) : -1;
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lock(m_mu);
            if (gen != m_gen.load()) continue;  // replaced while opening
            if (!book || pages <= 0) {
                m_error = book ? "no pages" : err;
                m_pages = -2;
                log("pages: can't open %s: %s", path.c_str(), m_error.c_str());
                if (book) cr_close(book);
                book = nullptr;
                pages = 0;
            } else {
                m_pages = pages;
                log("pages: opened %s, %d pages, %ld ms", path.c_str(), pages, ms);
            }
            continue;
        }
        if (!book) continue;
        // Pages the UI now holds needn't be remembered as sent.
        for (auto it = sent.begin(); it != sent.end();) {
            if (have.count(*it) || *it < page - kBehind || *it > page + kAhead) it = sent.erase(it);
            else ++it;
        }
        int next = -1;
        for (int d = 0; d <= kAhead && next < 0; ++d)
            for (int s : {1, -1}) {
                const int p = page + d * s;
                if ((s < 0 && d > kBehind) || p < 0 || p >= pages) continue;
                if (!have.count(p) && !sent.count(p)) { next = p; break; }
            }
        if (next < 0) continue;
        Result r;
        r.generation = gen;
        r.page = next;
        auto t0 = std::chrono::steady_clock::now();
        char err[256] = {0};
        unsigned char* px = cr_render(book, next, maxW, maxH, &r.w, &r.h, err, sizeof(err));
        r.ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (px) {
            r.rgba.assign(px, px + (size_t)r.w * r.h * 4);
            std::free(px);
        } else {
            r.error = err[0] ? err : "can't draw this page";
            log("pages: page %d: %s", next + 1, r.error.c_str());
        }
        sent.insert(next);
        std::lock_guard<std::mutex> lock(m_mu);
        if (gen == m_gen.load()) m_done.push_back(std::move(r));
    }
    if (book) cr_close(book);
    cr_ctx_drop(ctx);
}
