#include "Lookup.h"

#include "Json.h"

#include <sys/stat.h>
#include <unistd.h>

#include <ctime>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

using Library::log;

namespace {

const char* kApi = "https://www.comics.org/api";

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

// For comparing series names: lower case, letters and digits only, no leading "the".
std::string key(std::string s) {
    s = lower(s);
    if (s.compare(0, 4, "the ") == 0) s = s.substr(4);
    std::string out;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) out += c;
    return out;
}

std::string noZeros(std::string n) {
    while (n.size() > 1 && n[0] == '0' && std::isdigit((unsigned char)n[1])) n.erase(0, 1);
    return n;
}

// A GCD credit, or "" when unknown ("?", "None", empty).
std::string credit(const Json::Value& v) {
    std::string s = trim(v.str());
    if (s.empty() || s[0] == '?' || s == "None") return "";
    if (s.size() > 120) s = s.substr(0, 117) + "...";
    return s;
}

}  // namespace

Lookup::~Lookup() { stop(); }

void Lookup::stop() {
    m_curl.cancel();
    if (m_thread.joinable()) m_thread.join();
    m_curl.reset();
}

std::string Lookup::rel() const { std::lock_guard<std::mutex> l(m_mu); return m_rel; }
std::string Lookup::message() const { std::lock_guard<std::mutex> l(m_mu); return m_message; }
Library::BookInfo Lookup::result() const { std::lock_guard<std::mutex> l(m_mu); return m_result; }
std::string Lookup::match() const { std::lock_guard<std::mutex> l(m_mu); return m_match; }

bool Lookup::parseName(const std::string& name, const std::string& folder, std::string& series, std::string& number,
                       std::string& year) {
    std::string s = name;
    for (char& c : s)
        if (c == '_') c = ' ';
    // A year in brackets: "(1947)".
    year.clear();
    for (size_t i = 0; i + 5 < s.size(); ++i)
        if (s[i] == '(' && std::isdigit((unsigned char)s[i + 1]) && std::isdigit((unsigned char)s[i + 4]) && s[i + 5] == ')') {
            const int y = std::atoi(s.substr(i + 1, 4).c_str());
            if (y >= 1890 && y <= 2100) year = s.substr(i + 1, 4);
        }
    // Drop bracketed parts: "(1947)", "[c2c]", "(of 4)".
    std::string clean;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (!depth) clean += c;
    }
    clean = trim(clean);
    // The issue number: the last run of digits, after "#", "No." or a space.
    size_t end = clean.size();
    while (end > 0 && !std::isdigit((unsigned char)clean[end - 1])) --end;
    size_t beg = end;
    while (beg > 0 && std::isdigit((unsigned char)clean[beg - 1])) --beg;
    if (beg == end) return false;
    number = noZeros(clean.substr(beg, end - beg));
    std::string rest = clean.substr(0, beg);
    // "Series #12", "Series No. 12", "Series v2 012" -> "Series".
    rest = trim(rest);
    while (!rest.empty() && (rest.back() == '#' || rest.back() == '-' || rest.back() == ',')) rest = trim(rest.substr(0, rest.size() - 1));
    const std::string lr = lower(rest);
    if (lr.size() >= 3 && lr.compare(lr.size() - 3, 3, "no.") == 0) rest = trim(rest.substr(0, rest.size() - 3));
    series = rest.empty() ? trim(folder) : rest;
    return !series.empty();
}

void Lookup::start(const std::string& appDir, const std::string& rel, const std::string& name, const std::string& folder) {
    stop();
    {
        std::lock_guard<std::mutex> l(m_mu);
        m_appDir = appDir;
        m_rel = rel;
        m_candidates.clear();
        m_index = 0;
        m_result = Library::BookInfo();
        m_match.clear();
        if (!parseName(name, folder, m_series, m_number, m_year)) {
            m_message = "No issue number in its name";
            m_state = State::NotFound;
            return;
        }
        m_message = m_series + " #" + m_number + (m_year.empty() ? "" : " (" + m_year + ")");
    }
    m_state = State::Searching;
    m_thread = std::thread([this] { run(0); });
}

void Lookup::next() {
    if (m_state != State::Found) return;
    stop();
    size_t from;
    {
        std::lock_guard<std::mutex> l(m_mu);
        from = m_index + 1;
    }
    m_state = State::Searching;
    m_thread = std::thread([this, from] { run(from); });
}

// One GCD request. Answers are kept 30 days in data/gcd/ (the API is
// rate-limited, and looking up a series' issues one by one asks the same
// series questions again). No retries: when comics.org says wait, we say so.
bool Lookup::fetch(const std::string& url, std::string& body, std::string& why) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) h = (h ^ c) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.json", (unsigned long long)h);
    const std::string dir = m_appDir + "/data/gcd", cache = dir + "/" + name;
    struct stat st;
    if (::stat(cache.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime < 30 * 24 * 3600) {
        body = Net::readFile(cache);
        if (!body.empty()) return true;
    }
    if (!m_prepared) { m_curl.prepare(m_appDir); m_prepared = true; }
    ::mkdir(dir.c_str(), 0755);
    const std::string tmp = dir + "/.fetch", code = dir + "/.code";
    const int rc = m_curl.run({"--retry", "0", "--max-time", "25", "-o", tmp, "-w", "%{http_code}", url}, code, false);
    body = rc == 0 ? Net::readFile(tmp) : "";
    const int http = std::atoi(Net::readFile(code).c_str());
    ::unlink(code.c_str());
    if (rc != 0) { ::unlink(tmp.c_str()); why = rc == -2 ? "Stopped" : Net::message(rc, "comics.org"); return false; }
    if (http == 429) {
        // {"detail":"Request was throttled. Expected available in 3188 seconds."}
        const size_t at = body.find("available in ");
        const int secs = at == std::string::npos ? 0 : std::atoi(body.c_str() + at + 13);
        const int mins = (secs + 59) / 60;
        why = mins > 0 ? "comics.org asks to wait " + std::to_string(mins) + (mins == 1 ? " minute" : " minutes") +
                             " (too many lookups for now)"
                       : "comics.org is busy: try again later";
        ::unlink(tmp.c_str());
        return false;
    }
    if (http != 200) { ::unlink(tmp.c_str()); why = "comics.org answered " + std::to_string(http); return false; }
    std::rename(tmp.c_str(), cache.c_str());
    return true;
}

void Lookup::run(size_t from) {
    std::string series, number, year, why, body;
    {
        std::lock_guard<std::mutex> l(m_mu);
        series = m_series;
        number = m_number;
        year = m_year;
    }
    auto fail = [&](State st, const std::string& msg) {
        std::lock_guard<std::mutex> l(m_mu);
        m_message = msg;
        m_state = st;
        log("lookup: %s", msg.c_str());
    };
    // The candidate series, once: same name first, then English, then the
    // nearest start year to a year in the file name (else the oldest).
    if (m_candidates.empty()) {
        if (!fetch(std::string(kApi) + "/series/name/" + Net::urlEncode(series) + "/?format=json", body, why)) {
            fail(State::Failed, why);
            return;
        }
        const Json::Value list = Json::parse(body)["results"];
        struct Cand { std::string url; int score; int year; };
        std::vector<Cand> cands;
        const std::string want = key(series);
        for (size_t i = 0; i < list.size(); ++i) {
            const Json::Value& r = list[i];
            const std::string k = key(r["name"].str());
            if (k.find(want) == std::string::npos) continue;
            Cand c{r["api_url"].str(), 0, std::atoi(r["year_began"].str().c_str())};
            c.score = (k == want ? 100 : 0) + (r["language"].str() == "en" ? 10 : 0);
            if (!year.empty() && c.year) c.score -= std::min(50, std::abs(std::atoi(year.c_str()) - c.year));
            if (!c.url.empty()) cands.push_back(c);
        }
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.score != b.score ? a.score > b.score : a.year < b.year;
        });
        std::lock_guard<std::mutex> l(m_mu);
        for (const Cand& c : cands) m_candidates.push_back(c.url);
        log("lookup: \"%s\" #%s: %zu series", series.c_str(), number.c_str(), m_candidates.size());
    }
    std::vector<std::string> cands;
    {
        std::lock_guard<std::mutex> l(m_mu);
        cands = m_candidates;
    }
    // The first (from `from` on, at most 6 tried) that has this issue number.
    for (size_t i = from; i < cands.size() && i < from + 6; ++i) {
        if (!fetch(cands[i], body, why)) { fail(State::Failed, why); return; }
        const Json::Value s = Json::parse(body);
        const Json::Value& desc = s["issue_descriptors"];
        const Json::Value& issues = s["active_issues"];
        std::string issueUrl;
        for (size_t k = 0; k < desc.size() && k < issues.size(); ++k)
            if (noZeros(trim(desc[k].str())) == number) { issueUrl = issues[k].str(); break; }
        if (issueUrl.empty()) continue;
        if (!fetch(issueUrl, body, why)) { fail(State::Failed, why); return; }
        const Json::Value is = Json::parse(body);
        Library::BookInfo info;
        info.known = true;
        info.source = "gcd";
        std::string sname = s["name"].str();
        info.series = sname;
        info.number = noZeros(is["number"].str());
        const std::string kd = is["key_date"].str();  // "1947-04-00"
        if (kd.size() >= 4) info.year = kd.substr(0, 4);
        if (kd.size() >= 7 && kd.substr(5, 2) != "00") info.month = std::to_string(std::atoi(kd.substr(5, 2).c_str()));
        info.publisher = trim(is["brand_emblem"].str());
        if (info.publisher.empty()) info.publisher = trim(is["indicia_publisher"].str());
        info.pages = (int)std::atof(is["page_count"].str().c_str());
        // The stories: the first comic story gives the title and credits; the
        // rest are listed as what's inside.
        const Json::Value& stories = is["story_set"];
        std::string inside, coverTitle;
        int listed = 0;
        for (size_t k = 0; k < stories.size(); ++k) {
            const Json::Value& st = stories[k];
            const std::string type = st["type"].str(), title = trim(st["title"].str()), feature = trim(st["feature"].str());
            if (type == "cover") { if (coverTitle.empty()) coverTitle = title; continue; }
            if (type != "comic story") continue;
            if (info.title.empty() && !title.empty()) info.title = title;
            if (info.writer.empty()) info.writer = credit(st["script"]);
            if (info.artist.empty()) info.artist = credit(st["pencils"]);
            const std::string what = !feature.empty() && !title.empty() ? feature + ": " + title : !feature.empty() ? feature : title;
            if (!what.empty() && listed < 10) { inside += (inside.empty() ? "" : "; ") + what; ++listed; }
        }
        if (info.title.empty()) info.title = coverTitle;
        if (!inside.empty()) info.summary = "Inside: " + inside + ".";
        std::string pubName = info.publisher;
        {
            std::lock_guard<std::mutex> l(m_mu);
            m_index = i;
            m_result = info;
            m_match = sname + (s["year_began"].str().empty() ? "" : " (" + s["year_began"].str() + ")") +
                      (pubName.empty() ? "" : ", " + pubName) + " - match " + std::to_string(i + 1);
        }
        log("lookup: found %s #%s in %s", info.series.c_str(), info.number.c_str(), cands[i].c_str());
        m_state = State::Found;
        return;
    }
    fail(State::NotFound, from == 0 ? "Not in the Grand Comics Database: " + series + " #" + number
                                    : "No other series has " + series + " #" + number);
}
