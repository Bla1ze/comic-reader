#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Draws the open book's pages at the playfield's own resolution on a worker
// thread pinned to the fast cores, nearest pages first: the one shown, then
// the next, the previous, and so on out to kAhead / kBehind. The UI thread
// says which page is shown and which pages it already holds; finished pages
// are picked up with take().
class PageRenderer {
public:
    static constexpr int kAhead = 3, kBehind = 2;

    struct Result {
        int generation = 0;  // the open() it belongs to
        int page = 0;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string error;  // set if the page couldn't be drawn
        long ms = 0;
    };

    ~PageRenderer();
    void start();
    // A new book (path on disk), drawn to fit maxW x maxH. Returns its generation.
    int open(const std::string& path, int maxW, int maxH);
    void close();
    // The page on screen and the pages already held.
    void want(int page, const std::set<int>& have);
    bool take(Result& out);

    int pageCount() const { return m_pages.load(); }  // -1 while opening, -2 if it can't be opened
    std::string openError() const;
    int generation() const { return m_gen.load(); }

private:
    void worker();

    std::thread m_thread;
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_stop = false;

    // Requested state (under m_mu).
    std::string m_path;
    int m_maxW = 0, m_maxH = 0;
    int m_page = 0;
    std::set<int> m_have;
    std::string m_error;

    std::atomic<int> m_gen{0};
    std::atomic<int> m_pages{-1};
    std::deque<Result> m_done;
};
