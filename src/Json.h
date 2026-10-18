#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A small JSON reader for the Grand Comics Database's API replies: objects,
// arrays, strings (with \u escapes as UTF-8), numbers, true / false / null.
// Anything malformed reads as null.
namespace Json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    const Value& operator[](const std::string& key) const {
        static const Value none;
        auto it = o.find(key);
        return type == Object && it != o.end() ? it->second : none;
    }
    const Value& operator[](size_t i) const {
        static const Value none;
        return type == Array && i < a.size() ? a[i] : none;
    }
    size_t size() const { return type == Array ? a.size() : 0; }
    // A string, or a number written as text; "" otherwise.
    std::string str() const {
        if (type == String) return s;
        if (type == Number) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", n);
            return buf;
        }
        return "";
    }
};

namespace detail {

inline void space(const std::string& t, size_t& i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
}

inline void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

inline bool string(const std::string& t, size_t& i, std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    while (i < t.size() && t[i] != '"') {
        char c = t[i++];
        if (c != '\\') { out += c; continue; }
        if (i >= t.size()) return false;
        char e = t[i++];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            if (i + 4 > t.size()) return false;
            unsigned cp = (unsigned)std::strtoul(t.substr(i, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                unsigned lo = (unsigned)std::strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
            }
            utf8(out, cp);
            break;
        }
        default: out += e; break;  // \" \\ \/
        }
    }
    if (i >= t.size()) return false;
    ++i;
    return true;
}

inline bool value(const std::string& t, size_t& i, Value& v, int depth) {
    if (depth > 64) return false;
    space(t, i);
    if (i >= t.size()) return false;
    const char c = t[i];
    if (c == '{') {
        v.type = Value::Object;
        ++i;
        space(t, i);
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        while (i < t.size()) {
            space(t, i);
            std::string key;
            if (!string(t, i, key)) return false;
            space(t, i);
            if (i >= t.size() || t[i] != ':') return false;
            ++i;
            Value item;
            if (!value(t, i, item, depth + 1)) return false;
            v.o[key] = std::move(item);
            space(t, i);
            if (i < t.size() && t[i] == ',') { ++i; continue; }
            if (i < t.size() && t[i] == '}') { ++i; return true; }
            return false;
        }
        return false;
    }
    if (c == '[') {
        v.type = Value::Array;
        ++i;
        space(t, i);
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        while (i < t.size()) {
            Value item;
            if (!value(t, i, item, depth + 1)) return false;
            v.a.push_back(std::move(item));
            space(t, i);
            if (i < t.size() && t[i] == ',') { ++i; continue; }
            if (i < t.size() && t[i] == ']') { ++i; return true; }
            return false;
        }
        return false;
    }
    if (c == '"') { v.type = Value::String; return string(t, i, v.s); }
    if (t.compare(i, 4, "true") == 0) { v.type = Value::Bool; v.b = true; i += 4; return true; }
    if (t.compare(i, 5, "false") == 0) { v.type = Value::Bool; i += 5; return true; }
    if (t.compare(i, 4, "null") == 0) { i += 4; return true; }
    char* end = nullptr;
    const double n = std::strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return false;
    v.type = Value::Number;
    v.n = n;
    i = (size_t)(end - t.c_str());
    return true;
}

}  // namespace detail

inline Value parse(const std::string& text) {
    Value v;
    size_t i = 0;
    if (!detail::value(text, i, v, 0)) return Value();
    return v;
}

}  // namespace Json
