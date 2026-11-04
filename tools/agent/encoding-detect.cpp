#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "encoding-detect.h"

#include <cstdint>
#include <cstddef>

namespace enc {

namespace {

// Auto-generated CP866_MAP / CP1251_MAP (uint32_t[128], bytes 0x80..0xFF).
#include "enc-tables.inc"

inline void append_cp32(uint32_t cp, std::string & out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string table_to_utf8(const uint32_t * map, const std::string & raw) {
    std::string out;
    out.reserve(raw.size() + raw.size() / 2);
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            append_cp32(map[c - 0x80], out);
        }
    }
    return out;
}

void normalize_crlf(std::string & t) {
    size_t w = 0;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n') {
            i++; // skip CR, keep LF
        }
        t[w++] = t[i];
    }
    t.resize(w);
}

} // namespace

bool is_valid_utf8(const std::string & s) {
    const unsigned char * p = reinterpret_cast<const unsigned char *>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        unsigned char b = p[i];
        if (b < 0x80) { i++; continue; }
        size_t len;
        unsigned char lo2 = 0x80, hi2 = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            len = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            len = 3;
            if (b == 0xE0) lo2 = 0xA0; // no overlong
            if (b == 0xED) hi2 = 0x9F; // no surrogates
        } else if (b >= 0xF0 && b <= 0xF4) {
            len = 4;
            if (b == 0xF0) lo2 = 0x90; // no overlong
            if (b == 0xF4) hi2 = 0x8F; // no > U+10FFFF
        } else {
            return false; // 0x80-0xBF lone continuation, 0xC0/0xC1, 0xF5+
        }
        if (i + len > n) return false;
        for (size_t j = 1; j < len; j++) {
            unsigned char c = p[i + j];
            unsigned char lo = (j == 1) ? lo2 : 0x80;
            unsigned char hi = (j == 1) ? hi2 : 0xBF;
            if (c < lo || c > hi) return false;
        }
        i += len;
    }
    return true;
}

bool looks_like_utf8(const std::string & s) {
    bool has_high = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (static_cast<unsigned char>(s[i]) >= 0x80) { has_high = true; break; }
    }
    return has_high && is_valid_utf8(s);
}

std::string cp866_to_utf8(const std::string & raw) {
    return table_to_utf8(CP866_MAP, raw);
}

std::string cp1251_to_utf8(const std::string & raw) {
    return table_to_utf8(CP1251_MAP, raw);
}

std::string utf16_to_utf8(const std::string & raw, bool big_endian) {
    std::string out;
    const size_t n = raw.size() & ~static_cast<size_t>(1);
    out.reserve(raw.size());
    auto unit = [&](size_t i) -> uint32_t {
        unsigned char a = static_cast<unsigned char>(raw[i]);
        unsigned char b = static_cast<unsigned char>(raw[i + 1]);
        return big_endian ? (static_cast<uint32_t>(a) << 8 | b)
                          : (static_cast<uint32_t>(b) << 8 | a);
    };
    for (size_t i = 0; i < n; ) {
        uint32_t u = unit(i);
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 2 <= n) {
                uint32_t lo = unit(i);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    append_cp32(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), out);
                    i += 2;
                    continue;
                }
            }
            append_cp32(0xFFFD, out); // dangling high surrogate
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            append_cp32(0xFFFD, out); // lone low surrogate
        } else {
            append_cp32(u, out);
        }
    }
    if (raw.size() & 1) append_cp32(0xFFFD, out); // odd trailing byte
    return out;
}

#ifdef _WIN32
static std::string api_cp_to_utf8(
        UINT cp, const std::string & raw,
        std::string (*fallback)(const std::string &)) {
    if (raw.empty()) return raw;
    int wlen = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (wlen <= 0) return fallback(raw);
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    if (MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), &w[0], wlen) != wlen) {
        return fallback(raw);
    }
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return fallback(raw);
    std::string u(static_cast<size_t>(ulen), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, &u[0], ulen, nullptr, nullptr) != ulen) {
        return fallback(raw);
    }
    return u;
}

std::string oem_to_utf8(const std::string & raw) {
    return api_cp_to_utf8(CP_OEMCP, raw, &cp866_to_utf8);
}

std::string ansi_to_utf8(const std::string & raw) {
    return api_cp_to_utf8(CP_ACP, raw, &cp1251_to_utf8);
}
#endif

std::string decode_console_output(const std::string & raw) {
    if (raw.empty() || is_valid_utf8(raw)) return raw; // ASCII / UTF-8 / chcp 65001
#ifdef _WIN32
    std::string conv = oem_to_utf8(raw);
#else
    std::string conv = cp866_to_utf8(raw);
#endif
    return is_valid_utf8(conv) ? conv : raw;
}

text_decode_result decode_text_bytes(const std::string & raw) {
    text_decode_result r;
    r.converted = false;
    r.encoding = "utf-8";
    if (raw.empty()) return r;

    const unsigned char * p = reinterpret_cast<const unsigned char *>(raw.data());
    const size_t n = raw.size();
    const bool bom8    = n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF;
    const bool bom16le = n >= 2 && p[0] == 0xFF && p[1] == 0xFE;
    const bool bom16be = n >= 2 && p[0] == 0xFE && p[1] == 0xFF;
    const bool bom32le = n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00;
    const bool bom32be = n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF;

    if (bom32le || bom32be) {
        r.text = raw; // UTF-32: pass through untouched, keep BOM
        r.encoding = "utf-32";
        return r;
    }
    if (!bom16le && !bom16be) {
        const size_t lim = n < 8000 ? n : 8000;
        for (size_t i = 0; i < lim; i++) {
            if (p[i] == 0) {
                r.text = raw;
                r.encoding = "binary";
                return r;
            }
        }
    }

    if (bom8) {
        r.text = raw.substr(3);
        r.encoding = "utf-8-bom";
        r.converted = true;
    } else if (bom16le) {
        r.text = utf16_to_utf8(raw.substr(2), false);
        r.encoding = "utf-16le";
        r.converted = true;
    } else if (bom16be) {
        r.text = utf16_to_utf8(raw.substr(2), true);
        r.encoding = "utf-16be";
        r.converted = true;
    } else if (is_valid_utf8(raw)) {
        r.text = raw;
    } else {
        r.text = cp1251_to_utf8(raw);
        r.encoding = "cp1251";
        r.converted = true;
    }

    const size_t before = r.text.size();
    normalize_crlf(r.text);
    if (r.text.size() != before) r.converted = true;
    return r;
}

} // namespace enc
