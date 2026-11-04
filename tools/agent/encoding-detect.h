#ifndef ENCODING_DETECT_H
#define ENCODING_DETECT_H

#include <string>

// Encoding detection / conversion helpers for Windows console output and text
// file reading (mojibake fix, see docs). All functions are cross-platform;
// WinAPI-backed variants exist only on Windows.
namespace enc {

// Strict UTF-8 validation (no overlongs, no surrogates, no > U+10FFFF).
// Empty string and pure ASCII are valid UTF-8.
bool is_valid_utf8(const std::string & s);

// Contains bytes >= 0x80 AND is valid UTF-8.
bool looks_like_utf8(const std::string & s);

std::string cp866_to_utf8(const std::string & raw);
std::string cp1251_to_utf8(const std::string & raw);

// Decode UTF-16LE/BE byte stream to UTF-8 (handles surrogate pairs;
// dangling surrogates / odd trailing byte become U+FFFD).
std::string utf16_to_utf8(const std::string & raw, bool big_endian);

#ifdef _WIN32
// System OEM codepage (CP866 on ru-RU) -> UTF-8, table-based fallback.
std::string oem_to_utf8(const std::string & raw);
// System ANSI codepage (CP1251 on ru-RU) -> UTF-8, table-based fallback.
std::string ansi_to_utf8(const std::string & raw);
#endif

// Decode child-process console output captured from a pipe:
// valid UTF-8 (incl. pure ASCII / chcp 65001) passes through unchanged,
// otherwise converted from the OEM codepage. Result is always valid UTF-8
// (raw returned as-is if conversion produced invalid UTF-8).
std::string decode_console_output(const std::string & raw);

struct text_decode_result {
    std::string text;     // decoded UTF-8 text
    const char *encoding; // utf-8 | utf-8-bom | utf-16le | utf-16be | cp1251 | utf-32 | binary
    bool converted;       // true if bytes were transformed
};

// Decode raw bytes of a text file: BOM sniffing (UTF-8/UTF-16), strict UTF-8
// passthrough, CP1251 fallback; NUL bytes => binary; NUL-free UTF-16 without
// BOM is NOT detected (documented limitation). Normalizes CRLF -> LF in all
// text branches.
text_decode_result decode_text_bytes(const std::string & raw);

} // namespace enc

#endif // ENCODING_DETECT_H
