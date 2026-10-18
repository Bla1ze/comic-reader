#pragma once

#include "Doc.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Covers: page 1 of a book drawn once at kCoverW x kCoverH and kept as a JPEG
// in data/covers/, so the library opens quickly the next time.
namespace Covers {

constexpr int kCoverW = 540, kCoverH = 810;

// The cover's pixels (RGBA), from the cache or drawn now (and cached). Slow on
// a miss: call off the UI thread, with that thread's own context.
bool load(cr_ctx* ctx, const std::string& appDir, const std::string& rel, std::vector<uint8_t>& out, int& w, int& h);

}  // namespace Covers

// Loads covers for the library grid on a worker thread. The newest requests
// are served first (they're the ones on screen); results are picked up with
// take() on the UI thread.
class CoverLoader {
public:
    struct Result {
        std::string rel;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
    };

    ~CoverLoader();
    void start(const std::string& appDir);
    void request(const std::string& rel);  // ignored if already asked for
    bool take(Result& out);
    void forget(const std::string& rel);   // may be asked for again

private:
    void worker();

    std::string m_appDir;
    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    std::map<std::string, bool> m_asked;
    std::deque<Result> m_done;
    bool m_stop = false;
};
