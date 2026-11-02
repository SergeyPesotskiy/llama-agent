#pragma once

#include <string>

// Result of utf8_sanitize(): the text is guaranteed to be valid UTF-8.
struct utf8_sanitize_result {
    std::string text;        // sanitized text (valid UTF-8)
    bool reencoded = false;  // true if at least one fragment was decoded from a legacy encoding
    std::string encoding;    // name of the winning encoding (e.g. "cp1251"), empty if none
};

// Keeps valid UTF-8 bytes as-is. Each maximal run of invalid bytes is scored
// against cp1251, koi8-r and cp1252 using Russian letter-frequency analysis;
// the best candidate wins when it looks plausible enough, otherwise the bytes
// are replaced with U+FFFD (replacement character).
utf8_sanitize_result utf8_sanitize(const std::string & s);

// Adjust a truncation position forward (at most 3 bytes) so it does not land on
// a UTF-8 continuation byte (0x80..0xBF), avoiding a split codepoint when the
// caller cuts the string at pos. Returns pos unchanged when it is already a
// lead byte / ASCII or outside the string.
size_t utf8_safe_cut(const std::string & s, size_t pos);
