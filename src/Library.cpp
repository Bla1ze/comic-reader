#include "Library.h"

#include "Doc.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>

namespace Library {

namespace {

FILE* g_log = nullptr;
std::mutex g_logMu;

bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (struct dirent* e = ::readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        ::closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return cr_natcmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

// A folder is a book when its pictures are right inside it.
bool isPictureFolder(const std::string& dir) {
    if (DIR* d = ::opendir(dir.c_str())) {
        bool found = false;
        while (struct dirent* e = ::readdir(d))
            if (e->d_name[0] != '.' && cr_is_image_name(e->d_name)) { found = true; break; }
        ::closedir(d);
        return found;
    }
    return false;
}

const char* kReadme =
    "Put comics here: PDF, CBZ, CBR or CBT files, single pictures, or a folder of\n"
    "pictures per book (its pages, in name order).\n"
    "\n"
    "A folder of books becomes a series, one level deep:\n"
    "  comics/Jumbo Comics/Jumbo Comics 082.cbr\n"
    "\n"
    "Or send them from a phone or computer: Wi-Fi upload in Comic Reader.\n";

}  // namespace

void openLog(const std::string& appDir) {
    const std::string path = appDir + "/data/comic-reader.log";
    struct stat st;
    const bool big = ::stat(path.c_str(), &st) == 0 && st.st_size > 512 * 1024;
    g_log = std::fopen(path.c_str(), big ? "w" : "a");
    log("==== comic-reader, pid %d", (int)::getpid());
}

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMu);
    std::fprintf(stdout, "[comic-reader] %s\n", buf);
    std::fflush(stdout);
    if (g_log) {
        std::fprintf(g_log, "%s\n", buf);
        std::fflush(g_log);
    }
}

void logInputDevices() {
    std::ifstream in("/proc/bus/input/devices");
    std::string line, name;
    while (std::getline(in, line)) {
        if (line.compare(0, 9, "N: Name=\"") == 0) name = line.substr(9, line.size() - 10);
        else if (line.compare(0, 12, "H: Handlers=") == 0) log("input: %s [%s]", name.c_str(), line.substr(12).c_str());
    }
}

void ensureFolders(const std::string& appDir) {
    for (const char* d : {"comics", "manuals", "data", "data/covers"}) ::mkdir((appDir + "/" + d).c_str(), 0755);
    const std::string readme = appDir + "/comics/README.txt";
    struct stat st;
    if (::stat(readme.c_str(), &st) != 0)
        if (FILE* f = std::fopen(readme.c_str(), "w")) {
            std::fputs(kReadme, f);
            std::fclose(f);
        }
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool isBookFile(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = lower(name.substr(dot + 1));
    return ext == "pdf" || ext == "cbz" || ext == "cbr" || ext == "cbt" || cr_is_image_name(name.c_str());
}

std::string titleOf(const std::string& name) {
    std::string s = name;
    const size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && isBookFile(s)) s = s.substr(0, dot);
    std::string out;
    for (char c : s) {
        if (c == '_') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? name : out;
}

std::vector<Entry> list(const std::string& appDir, const std::string& dirRel) {
    std::vector<Entry> series, books;
    const std::string dir = appDir + "/" + dirRel;
    for (const std::string& n : names(dir)) {
        const std::string full = dir + "/" + n, rel = dirRel + "/" + n;
        if (isDir(full)) {
            if (isPictureFolder(full)) {
                Entry e;
                e.rel = e.coverRel = rel;
                e.title = titleOf(n);
                books.push_back(e);
                continue;
            }
            // A series: its books, one level down.
            Entry e;
            e.rel = rel;
            e.title = titleOf(n);
            e.series = true;
            for (const std::string& b : names(full)) {
                const std::string bf = full + "/" + b;
                if (isDir(bf) ? isPictureFolder(bf) : isBookFile(b)) {
                    if (e.coverRel.empty()) e.coverRel = rel + "/" + b;
                    ++e.count;
                }
            }
            if (e.count > 0) series.push_back(e);
        } else if (isBookFile(n)) {
            Entry e;
            e.rel = e.coverRel = rel;
            e.title = titleOf(n);
            books.push_back(e);
        }
    }
    series.insert(series.end(), books.begin(), books.end());
    return series;
}

// ------------------------------------------------------------------ progress

void ProgressStore::load(const std::string& appDir) {
    m_path = appDir + "/data/progress.txt";
    m_map.clear();
    std::ifstream in(m_path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
        Progress p;
        p.page = std::atoi(line.substr(a + 1, b - a - 1).c_str());
        p.pages = std::atoi(line.substr(b + 1, c - b - 1).c_str());
        p.time = std::atoll(line.substr(c + 1).c_str());
        m_map[line.substr(0, a)] = p;
    }
}

Progress ProgressStore::get(const std::string& rel) const {
    auto it = m_map.find(rel);
    return it == m_map.end() ? Progress() : it->second;
}

void ProgressStore::set(const std::string& rel, int page, int pages) {
    Progress& p = m_map[rel];
    p.page = page;
    p.pages = pages;
    p.time = (long long)std::time(nullptr);
    save();
}

std::vector<std::string> ProgressStore::continueReading(size_t max) const {
    std::vector<std::pair<long long, std::string>> v;
    for (const auto& kv : m_map)
        if (kv.second.pages > 0 && kv.second.page > 0 && kv.second.page < kv.second.pages - 1)
            v.push_back({kv.second.time, kv.first});
    std::sort(v.begin(), v.end(), [](const std::pair<long long, std::string>& a, const std::pair<long long, std::string>& b) {
        return a.first > b.first;
    });
    std::vector<std::string> out;
    for (size_t i = 0; i < v.size() && i < max; ++i) out.push_back(v[i].second);
    return out;
}

void ProgressStore::save() const {
    if (m_path.empty()) return;
    const std::string tmp = m_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    for (const auto& kv : m_map) std::fprintf(f, "%s\t%d\t%d\t%lld\n", kv.first.c_str(), kv.second.page, kv.second.pages, kv.second.time);
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    std::rename(tmp.c_str(), m_path.c_str());
}

std::string coverCachePath(const std::string& appDir, const std::string& rel) {
    struct stat st;
    std::string key = rel + "|720";  // the cover size: a change draws them again
    if (::stat((appDir + "/" + rel).c_str(), &st) == 0)
        key += "|" + std::to_string((long long)st.st_size) + "|" + std::to_string((long long)st.st_mtime);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.jpg", (unsigned long long)h);
    return appDir + "/data/covers/" + name;
}

}  // namespace Library
