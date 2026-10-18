#include "Library.h"

#include "Doc.h"

#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>

namespace Library {

bool isBookFile(const std::string& name);

namespace {

FILE* g_log = nullptr;
std::mutex g_logMu;

bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

bool isDirPath(const std::string& p) { return isDir(p); }

namespace {

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

// A folder is a book when it holds pictures and nothing else: no books of
// other kinds and no folders (then it's a series, its pictures single books).
bool isPictureFolder(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return false;
    bool pictures = false, other = false;
    while (struct dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string n = e->d_name;
        if (cr_is_image_name(n.c_str()) && !isDir(dir + "/" + n)) pictures = true;
        else if (isDir(dir + "/" + n) || isBookFile(n)) { other = true; break; }
    }
    ::closedir(d);
    return pictures && !other;
}

const char* kReadme =
    "Put comics here: PDF, CBZ, CBR or CBT files, single pictures (JPG, PNG, WebP\n"
    "and others), or a folder of pictures per book (its pages, in name order).\n"
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

namespace {
void onCrash(int sig) {
    // Only async-signal-safe calls: write() and backtrace_symbols_fd().
    if (g_log) {
        const int fd = fileno(g_log);
        char msg[64];
        const int n = std::snprintf(msg, sizeof(msg), "crash: signal %d, backtrace:\n", sig);
        if (n > 0 && ::write(fd, msg, (size_t)n) < 0) {}
        void* frames[48];
        backtrace_symbols_fd(frames, backtrace(frames, 48), fd);
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
}  // namespace

void installCrashLog() {
    void* warm[2];
    backtrace(warm, 2);  // loads libgcc now, not inside the handler
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) ::signal(sig, onCrash);
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

namespace {

// The text of <tag>...</tag> (first one), entities decoded, trimmed.
std::string xmlField(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return "";
    const size_t b = xml.find(close, a + open.size());
    if (b == std::string::npos) return "";
    std::string v = xml.substr(a + open.size(), b - a - open.size()), out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '&') {
            static const char* ents[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}};
            bool done = false;
            for (auto& e : ents)
                if (v.compare(i, std::strlen(e[0]), e[0]) == 0) { out += e[1]; i += std::strlen(e[0]) - 1; done = true; break; }
            if (!done) out += v[i];
        } else if (v[i] == '\r' || v[i] == '\n' || v[i] == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += v[i];
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out[0] == ' ') out.erase(0, 1);
    return out;
}

}  // namespace

std::string BookInfo::display() const {
    if (!series.empty()) return number.empty() ? series : series + " #" + number;
    return title;
}

BookInfo parseComicInfo(const std::string& xml) {
    BookInfo i;
    i.known = true;
    i.series = xmlField(xml, "Series");
    i.number = xmlField(xml, "Number");
    i.title = xmlField(xml, "Title");
    i.year = xmlField(xml, "Year");
    i.month = xmlField(xml, "Month");
    i.publisher = xmlField(xml, "Publisher");
    i.writer = xmlField(xml, "Writer");
    i.artist = xmlField(xml, "Penciller");
    if (i.artist.empty()) i.artist = xmlField(xml, "CoverArtist");
    i.summary = xmlField(xml, "Summary");
    i.pages = std::atoi(xmlField(xml, "PageCount").c_str());
    i.rtl = xmlField(xml, "Manga") == "YesAndRightToLeft";
    // Leading zeros on numbers read as "#082"; drop them.
    while (i.number.size() > 1 && i.number[0] == '0' && std::isdigit((unsigned char)i.number[1])) i.number.erase(0, 1);
    return i;
}

std::string encodeInfo(const BookInfo& i) {
    auto one = [](std::string v) { for (char& c : v) if (c == '\n') c = ' '; return v; };
    return one(i.series) + "\n" + one(i.number) + "\n" + one(i.title) + "\n" + one(i.year) + "\n" + one(i.month) + "\n" +
           one(i.publisher) + "\n" + one(i.writer) + "\n" + one(i.artist) + "\n" + one(i.summary) + "\n" +
           std::to_string(i.pages) + "\n" + (i.rtl ? "rtl" : "ltr") + "\n" + one(i.source) + "\n";
}

BookInfo decodeInfo(const std::string& s) {
    BookInfo i;
    std::vector<std::string> f;
    size_t at = 0;
    while (at < s.size()) {
        size_t nl = s.find('\n', at);
        f.push_back(s.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    if (f.size() < 11) return i;
    i.series = f[0]; i.number = f[1]; i.title = f[2]; i.year = f[3]; i.month = f[4];
    i.publisher = f[5]; i.writer = f[6]; i.artist = f[7]; i.summary = f[8];
    i.pages = std::atoi(f[9].c_str());
    i.rtl = f[10] == "rtl";
    if (f.size() > 11) i.source = f[11];
    i.known = true;
    return i;
}

void ProgressStore::clear() {
    m_map.clear();
    save();
}

void Settings::load(const std::string& appDir) {
    m_path = appDir + "/data/settings.cfg";
    m_map.clear();
    std::ifstream in(m_path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            return v;
        };
        m_map[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

std::string Settings::get(const std::string& key, const std::string& fallback) const {
    auto it = m_map.find(key);
    return it == m_map.end() ? fallback : it->second;
}

void Settings::set(const std::string& key, const std::string& value) {
    m_map[key] = value;
    if (m_path.empty()) return;
    const std::string tmp = m_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    for (const auto& kv : m_map) std::fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    std::rename(tmp.c_str(), m_path.c_str());
}

int clearCovers(const std::string& appDir) {
    const std::string dir = appDir + "/data/covers";
    int n = 0;
    for (const std::string& name : names(dir))
        if (::unlink((dir + "/" + name).c_str()) == 0) ++n;
    return n;
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
