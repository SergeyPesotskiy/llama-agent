// Unit tests for enc:: encoding-detect (CP866/CP1251/UTF-16/BOM -> UTF-8).
// Cross-platform; WinAPI path is exercised under Wine/Windows only.
#include "encoding-detect.h"

#include <cstdio>
#include <cstring>
#include <string>

// Byte fixtures, auto-generated with python (cp866/cp1251/utf-16 codecs).
#include "test-enc-bytes.inc"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                  \
    } while (0)

int main() {
    using namespace std::string_literals;

    // t1: CP866 -> UTF-8 (table)
    CHECK(enc::cp866_to_utf8(kCp866Privet) == "Привет"s);

    // t2: valid UTF-8 passes decode_console_output unchanged
    std::string u8 = "Настройка интерфейса \"Ethernet 4\""s;
    CHECK(enc::decode_console_output(u8) == u8);
    CHECK(enc::looks_like_utf8(u8));

    // t3: CP866 'Диски: C:\' is invalid UTF-8 and must convert to expected text
    std::string diski = kCp866Diski;
    CHECK(!enc::is_valid_utf8(diski));
    CHECK(enc::decode_console_output(diski) == "Диски: C:\\"s);
    CHECK(enc::decode_console_output(kCp866Privet) == "Привет"s);

    // t4: pure ASCII unchanged everywhere, valid UTF-8
    std::string asc = "dir C:\\ /b 2>&1";
    CHECK(enc::is_valid_utf8(asc));
    CHECK(enc::decode_console_output(asc) == asc);
    CHECK(enc::cp866_to_utf8(asc) == asc);
    CHECK(enc::cp1251_to_utf8(asc) == asc);
    auto d0 = enc::decode_text_bytes(asc);
    CHECK(!d0.converted && d0.encoding == "utf-8"s && d0.text == asc);

    // t5: BOM handling
    std::string u8bom = "\xEF\xBB\xBF"s + "Привет"s;
    auto d1 = enc::decode_text_bytes(u8bom);
    CHECK(d1.converted && d1.encoding == "utf-8-bom"s);
    CHECK(d1.text == "Привет"s);
    auto d2 = enc::decode_text_bytes(std::string(kUtf16LEHello, sizeof(kUtf16LEHello) - 1));
    CHECK(d2.converted && d2.encoding == "utf-16le"s);
    CHECK(d2.text == "Привет мир"s);
    auto d3 = enc::decode_text_bytes(std::string(kUtf16BEHello, sizeof(kUtf16BEHello) - 1));
    CHECK(d3.converted && d3.encoding == "utf-16be"s);
    CHECK(d3.text == "Привет мир"s);
    std::string u32 = "\xFF\xFE\x00\x00"s + "x\0\0\0"s;
    auto d3b = enc::decode_text_bytes(u32);
    CHECK(d3b.encoding == "utf-32"s && d3b.text == u32 && !d3b.converted);

    // t6: CP1251 fallback for non-UTF-8 text (double-encoding bug source)
    auto d4 = enc::decode_text_bytes(kCp1251Zapros);
    CHECK(d4.converted && d4.encoding == "cp1251"s);
    CHECK(d4.text == "Запрошенное имя верно"s);

    // t7: CRLF -> LF normalization (mixed ASCII + CP1251 body)
    std::string c7 = "a\r\n"s + kCp1251B; // kCp1251B == cp1251("б\r\n")
    auto d5 = enc::decode_text_bytes(c7);
    CHECK(d5.text == "a\nб\n"s);
    CHECK(d5.converted);
    auto d5b = enc::decode_text_bytes("line\r\n");
    CHECK(d5b.text == "line\n"s && d5b.converted && d5b.encoding == "utf-8"s);

    // t8: round-trips through both codepages
    CHECK(enc::cp866_to_utf8(kCp866Round) == "Широкая электрическая юбка"s);
    CHECK(enc::cp1251_to_utf8(kCp1251Round) == "Широкая электрическая юбка"s);

    // t9: looks_like_utf8 must NOT be fooled by CP866 Cyrillic
    CHECK(!enc::looks_like_utf8(kCp866DiskiShort));
    CHECK(!enc::looks_like_utf8(std::string(kCp866Privet)));
    CHECK(enc::looks_like_utf8("Привет"s));
    CHECK(!enc::looks_like_utf8("plain ascii"));

    // binary sniff: NUL without UTF-16 BOM
    auto d6 = enc::decode_text_bytes(std::string("a\0b", 3));
    CHECK(d6.encoding == "binary"s && !d6.converted);
    // empty
    auto d7 = enc::decode_text_bytes("");
    CHECK(d7.text.empty() && !d7.converted && d7.encoding == "utf-8"s);

#ifdef _WIN32
    // WinAPI OEM path (CP866 under ru-RU; Wine inherits host locale, table
    // fallback keeps this deterministic if OEMCP != 866 — both must match).
    CHECK(enc::oem_to_utf8(kCp866Privet) == "Привет"s);
#endif

    if (g_failures == 0) {
        printf("test-encoding: OK (%d checks)\n", g_checks);
        return 0;
    }
    printf("test-encoding: FAILED (%d/%d)\n", g_failures, g_checks);
    return 1;
}
