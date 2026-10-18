#pragma once

#include <map>
#include <string>
#include <vector>

// The app's folder on the USB stick:
//   comics/     books, loose or in series folders (one level)
//   manuals/    rule sheets and manuals, the same way
//   data/       settings, reading progress, the log, the cover cache
// A book is a PDF, CBZ, CBR or CBT file, a single picture, or a folder of
// pictures (the pages, in name order).
namespace Library {

void openLog(const std::string& appDir);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logInputDevices();
// A crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) writes the signal and a
// backtrace to the log before the app goes down.
void installCrashLog();

void ensureFolders(const std::string& appDir);

struct Entry {
    std::string rel;       // from the app folder: "comics/Jumbo Comics/Jumbo_Comics_082.cbr"
    std::string title;     // "Jumbo Comics 082"
    bool series = false;   // a folder of books
    int count = 0;         // series: books inside
    std::string coverRel;  // series: its first book; a book: itself
};

// What's in a shelf folder ("comics", "manuals" or a series folder under one):
// series first, then books, each in natural name order.
std::vector<Entry> list(const std::string& appDir, const std::string& dirRel);

bool isBookFile(const std::string& name);  // by extension
std::string titleOf(const std::string& name);
std::string lower(std::string s);

// Reading progress, data/progress.txt: "rel<TAB>page<TAB>pages<TAB>time".
struct Progress {
    int page = 0;   // 0-based, the page last shown
    int pages = 0;  // 0 if never opened
    long long time = 0;
};
class ProgressStore {
public:
    void load(const std::string& appDir);
    Progress get(const std::string& rel) const;
    void set(const std::string& rel, int page, int pages);
    // Started and not finished, most recent first.
    std::vector<std::string> continueReading(size_t max) const;

private:
    void save() const;
    std::string m_path;
    std::map<std::string, Progress> m_map;
};

// data/covers/<key>.jpg: the cover drawn once, keyed by the book's path, size
// and date so a replaced file gets a new one.
std::string coverCachePath(const std::string& appDir, const std::string& rel);

}  // namespace Library
