// Off-cabinet test of everything but the screens: the library scan, covers,
// the page renderer and the Wi-Fi upload server. Build with tools/build-harness.sh
// and run on any aarch64 Linux:  harness <app folder>
#include "../src/Covers.h"
#include "../src/Doc.h"
#include "../src/Library.h"
#include "../src/Pages.h"
#include "../src/Transfer.h"
#include "../src/Update.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <cstdlib>
#include <string>
#include <thread>

using Library::log;

static long msSince(std::chrono::steady_clock::time_point t0) {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: harness <app folder> [serve-seconds]\n"); return 2; }
    const std::string app = argv[1];
    Library::ensureFolders(app);
    Library::openLog(app);
    int fails = 0;

    // harness <app> update: check (COMIC_READER_RELEASES_FILE / _PRETEND_VERSION
    // steer it) and install, as Settings > Check for updates does.
    if (argc >= 3 && std::string(argv[2]) == "update") {
        Updater u;
        u.check(app, false);
        auto waitWhile = [&](std::initializer_list<Updater::State> busy) {
            for (int i = 0; i < 6000; ++i) {
                bool b = false;
                for (Updater::State s : busy) b |= u.state() == s;
                if (!b) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        waitWhile({Updater::State::Idle, Updater::State::Checking});
        std::printf("check: state %d, latest %s, %s\n", (int)u.state(), u.latest().c_str(), u.message().c_str());
        if (u.state() != Updater::State::Available) return 1;
        u.install();
        waitWhile({Updater::State::Available, Updater::State::Downloading, Updater::State::Installing});
        std::printf("install: state %d (%s), files updated %d\n", (int)u.state(), u.message().c_str(), u.filesUpdated());
        return u.state() == Updater::State::Done ? 0 : 1;
    }

    // 1. The library.
    for (const char* shelf : {"comics", "manuals"}) {
        auto items = Library::list(app, shelf);
        std::printf("== %s: %zu entries\n", shelf, items.size());
        for (const auto& e : items) {
            std::printf("  %-8s %-40s cover=%s count=%d\n", e.series ? "series" : "book", e.title.c_str(), e.coverRel.c_str(), e.count);
            if (e.series)
                for (const auto& b : Library::list(app, e.rel)) std::printf("    book %-38s %s\n", b.title.c_str(), b.rel.c_str());
        }
    }

    // 2. Covers, drawn then cached.
    cr_ctx* ctx = cr_ctx_new();
    std::vector<std::string> books;
    for (const char* shelf : {"comics", "manuals"})
        for (const auto& e : Library::list(app, shelf)) {
            if (!e.series) { books.push_back(e.rel); continue; }
            for (const auto& b : Library::list(app, e.rel)) books.push_back(b.rel);
        }
    for (int pass = 0; pass < 2; ++pass)
        for (const std::string& rel : books) {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            auto t0 = std::chrono::steady_clock::now();
            bool ok = Covers::load(ctx, app, rel, px, w, h);
            std::printf("cover %s %-45s %s %dx%d %ld ms\n", pass ? "cached" : "drawn ", rel.c_str(), ok ? "ok  " : "FAIL", w, h, msSince(t0));
            if (!ok) ++fails;
        }

    // 3. Pages: open each book, ask for page 1, then flip forward.
    PageRenderer pages;
    pages.start();
    for (const std::string& rel : books) {
        auto t0 = std::chrono::steady_clock::now();
        const int gen = pages.open(app + "/" + rel, 2160, 3840);
        while (pages.pageCount() == -1 && msSince(t0) < 20000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const int n = pages.pageCount();
        std::printf("pages %-45s gen %d: %d pages (open %ld ms)\n", rel.c_str(), gen, n, msSince(t0));
        if (n <= 0) { ++fails; continue; }
        std::set<int> have;
        int got = 0;
        for (int page = 0; page < std::min(n, 4); ++page) {
            pages.want(page, have);
            auto t1 = std::chrono::steady_clock::now();
            PageRenderer::Result r;
            // Wait for this page (others arrive too: they're the read-ahead).
            bool found = have.count(page) > 0;
            while (!found && msSince(t1) < 20000) {
                if (pages.take(r)) {
                    have.insert(r.page);
                    ++got;
                    if (!r.error.empty()) { std::printf("    page %d ERROR %s\n", r.page + 1, r.error.c_str()); ++fails; }
                    else std::printf("    page %d %dx%d drawn in %ld ms\n", r.page + 1, r.w, r.h, r.ms);
                    found = r.page == page;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (!found) { std::printf("    page %d TIMEOUT\n", page + 1); ++fails; }
        }
    }
    pages.close();

    // 4. ComicInfo (none in these files, but it must not fail).
    for (const std::string& rel : books) {
        char* xml = cr_comicinfo(ctx, (app + "/" + rel).c_str());
        if (xml) { std::printf("comicinfo %s: %zu bytes\n", rel.c_str(), std::strlen(xml)); std::free(xml); }
    }
    cr_ctx_drop(ctx);

    // 5. The upload server: run for the given seconds (the shell side does the requests).
    if (argc >= 3) {
        TransferServer t;
        if (!t.start(app)) { std::printf("transfer: did not start\n"); return 1; }
        TransferServer::Status st = t.status();
        std::printf("transfer: port %d pin %s\n", st.port, st.pin.c_str());
        FILE* f = std::fopen((app + "/data/test-pin.txt").c_str(), "w");
        if (f) { std::fprintf(f, "%d %s\n", st.port, st.pin.c_str()); std::fclose(f); }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
        st = t.status();
        std::printf("transfer: received %d files, %llu bytes\n", st.received, (unsigned long long)st.receivedBytes);
        for (const auto& line : st.recent) std::printf("  %s\n", line.c_str());
        t.stop();
    }
    std::printf("fails: %d\n", fails);
    return fails ? 1 : 0;
}
