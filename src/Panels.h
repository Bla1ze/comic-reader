#pragma once

#include "DisplayProfile.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The backglass and DMD, driven over KMS with SDL's own card0 fd (Retro
// Launcher's GamePanels technique, from Jukebox's PanelOutput), while the app
// owns the playfield:
//   backglass: the book's cover over a blurred, darkened copy of itself;
//   DMD:       the title, "Page 12 of 32" and a progress bar.
// Composed on a worker thread (latest request wins). The backglass is redrawn
// only when the cover changes, the DMD on every page turn.
class Panels {
public:
    struct Item {
        std::string rel;       // the book (or series) the cover comes from; "" = app title card
        std::string title;     // "Jumbo Comics 082"
        std::string subtitle;  // "Jumbo Comics" (series) or "Comics"
        int page = 0, pages = 0;  // pages 0: not started / unknown
        bool reading = false;  // in the reader (else browsing)
        bool series = false;   // a series: no reading progress
    };

    ~Panels();
    int init(const DisplayProfile::Topology& topology, const std::string& appDir);
    bool active() const { return !m_panels.empty(); }
    void show(const Item& item);

private:
    struct Panel {
        uint32_t connId = 0, crtcId = 0, fbId = 0, handle = 0, pitch = 0;
        int w = 0, h = 0;    // physical buffer
        int vw = 0, vh = 0;  // as seen (landscape)
        bool rot = false;    // portrait-native buffer: present rotated
        bool dmd = false;
        uint8_t* map = nullptr;
        std::size_t size = 0;
        std::vector<uint8_t> canvas;  // vw*vh RGBA
    };

    uint32_t pickCrtc(uint32_t currentEncoder, const std::vector<uint32_t>& encoders, uint32_t count, uint32_t mainConnector);
    void worker();
    void composeBackglass(Panel& p, const Item& item, const std::vector<uint8_t>& cover, int cw, int ch);
    void composeDmd(Panel& p, const Item& item);
    void present(Panel& p);

    std::string m_appDir;
    int m_fd = -1;  // SDL's card0 fd; never closed here
    std::vector<Panel> m_panels;
    std::string m_lastKey, m_lastCoverRel = "\x01";
    std::string m_shownBg = "\x01", m_shownDmd = "\x01";
    std::vector<uint8_t> m_dmdGround;  // the DMD's background, drawn once

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    Item m_job;
    bool m_hasJob = false, m_stop = false;
};
