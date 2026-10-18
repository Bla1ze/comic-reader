#pragma once

#include "Library.h"
#include "Net.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// "Look up details" on the details card: for a book with no ComicInfo.xml, the
// series and issue number read from its name ("Jumbo Comics 082") are looked
// up in the Grand Comics Database (comics.org, free API, data CC BY-SA 4.0).
// Several series often share a name, so the result is shown to be kept or
// skipped for the next match; a kept one goes in the app's own cache, never
// into the book.
class Lookup {
public:
    enum class State { Idle, Searching, Found, NotFound, Failed };

    ~Lookup();
    // `name`: the book's title as shown ("Jumbo Comics 082"); `folder`: its
    // series folder's name, used when the name is only a number.
    void start(const std::string& appDir, const std::string& rel, const std::string& name, const std::string& folder);
    void next();  // the next series that has this issue
    void stop();

    State state() const { return m_state.load(); }
    std::string rel() const;
    std::string message() const;   // why NotFound / Failed, or what was searched
    Library::BookInfo result() const;
    std::string match() const;     // "Jumbo Comics (1938 series), Fiction House - match 1"

    // "Jumbo Comics 082 (1947)" -> series "Jumbo Comics", number "82", year "1947".
    static bool parseName(const std::string& name, const std::string& folder, std::string& series, std::string& number,
                          std::string& year);

private:
    void run(size_t from);
    bool fetch(const std::string& url, std::string& body, std::string& why);

    std::string m_appDir, m_rel, m_series, m_number, m_year;
    std::vector<std::string> m_candidates;  // series API URLs, best first
    size_t m_index = 0;
    bool m_prepared = false;
    Net::Curl m_curl;
    std::thread m_thread;
    std::atomic<State> m_state{State::Idle};
    mutable std::mutex m_mu;
    std::string m_message, m_match;
    Library::BookInfo m_result;
};
