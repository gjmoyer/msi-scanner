#include "msi_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace msi;

static int g_failures = 0;

static void check(bool condition, const std::string& name) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name.c_str());
    if (!condition) {
        g_failures++;
    }
}

static const char* kDigitModules[10] = {
    "100100100100", "100100100110", "100100110100", "100100110110", "100110100100",
    "100110100110", "100110110100", "100110110110", "110100100100", "110100100110",
};

static std::string encodeModules(const std::string& digits) {
    std::string s = "110";
    for (char c : digits) {
        s += kDigitModules[c - '0'];
    }
    s += "1001";
    return s;
}

static std::string encodeModulesWideSpace(const std::string& digits) {
    std::string s = "1100";
    for (char c : digits) {
        const int d = c - '0';
        for (int bit = 3; bit >= 0; --bit) {
            s += ((d >> bit) & 1) ? "1100" : "1000";
        }
    }
    s += "10001";
    return s;
}

static std::string withChecksum(const std::string& data, ChecksumMode mode) {
    std::string s = data;
    switch (mode) {
        case ChecksumMode::Mod10:
            s += static_cast<char>('0' + mod10CheckDigit(data));
            break;
        case ChecksumMode::Mod11:
            s += static_cast<char>('0' + mod11CheckDigit(data));
            break;
        case ChecksumMode::Mod1010: {
            const int c1 = mod10CheckDigit(data);
            s += static_cast<char>('0' + c1);
            s += static_cast<char>('0' + mod10CheckDigit(s));
            break;
        }
        case ChecksumMode::Mod1110: {
            const int c1 = mod11CheckDigit(data);
            s += static_cast<char>('0' + c1);
            s += static_cast<char>('0' + mod10CheckDigit(s));
            break;
        }
        default:
            break;
    }
    return s;
}

struct Image {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;

    uint8_t& at(int along, int across, bool vertical) {
        return vertical ? pixels[static_cast<size_t>(along) * stride + across]
                        : pixels[static_cast<size_t>(across) * stride + along];
    }
};

struct RenderSpec {
    int modulePx = 3;
    int barHeight = 64;
    int quietModules = 10;
    bool vertical = false;
    bool flipX = false;
    int noise = 0;
    int blurPasses = 0;
    uint8_t dark = 0;
    uint8_t light = 255;
};

static uint32_t g_rng = 0x12345678u;

static uint32_t xorshift() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static Image render(const std::string& modules, const RenderSpec& spec) {
    Image img;
    const int lengthPx = (static_cast<int>(modules.size()) + 2 * spec.quietModules) * spec.modulePx;
    const int acrossPx = spec.barHeight;
    if (!spec.vertical) {
        img.width = lengthPx;
        img.height = acrossPx;
    } else {
        img.width = acrossPx;
        img.height = lengthPx;
    }
    img.stride = img.width;
    img.pixels.assign(static_cast<size_t>(img.width) * img.height, spec.light);

    for (size_t i = 0; i < modules.size(); ++i) {
        if (modules[i] != '1') {
            continue;
        }
        const int start = (spec.quietModules + static_cast<int>(i)) * spec.modulePx;
        for (int p = 0; p < spec.modulePx; ++p) {
            int along = start + p;
            if (spec.flipX) {
                along = lengthPx - 1 - along;
            }
            for (int across = 0; across < acrossPx; ++across) {
                img.at(along, across, spec.vertical) = spec.dark;
            }
        }
    }

    for (int pass = 0; pass < spec.blurPasses; ++pass) {
        std::vector<uint8_t> copy = img.pixels;
        Image src = img;
        src.pixels.swap(copy);
        for (int along = 1; along < lengthPx - 1; ++along) {
            for (int across = 0; across < acrossPx; ++across) {
                const int value =
                    (src.at(along - 1, across, spec.vertical) +
                     2 * src.at(along, across, spec.vertical) +
                     src.at(along + 1, across, spec.vertical)) / 4;
                img.at(along, across, spec.vertical) = static_cast<uint8_t>(value);
            }
        }
    }

    if (spec.noise > 0) {
        for (uint8_t& p : img.pixels) {
            const int delta = static_cast<int>(xorshift() % (2 * spec.noise + 1)) - spec.noise;
            int value = p + delta;
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            p = static_cast<uint8_t>(value);
        }
    }

    return img;
}

static Result decode(const Image& img, ChecksumMode mode, Options options) {
    return decodeYPlane(img.pixels.data(), img.width, img.height, img.stride, mode, options);
}

static void testWikipediaFixture() {
    check(mod10CheckDigit("1234567") == 4, "mod10 check digit for 1234567 is 4");
    check(mod11CheckDigit("1234567") == 4, "mod11 (IBM) check digit for 1234567 is 4");
    check(mod10CheckDigit("7992739871") == 3, "mod10 check digit for 7992739871 is 3 (Luhn)");
    check(mod10CheckDigit("8052") == 3, "mod10 check digit for 8052 is 3 (literature)");
    check(mod11CheckDigit("1111111") == 4, "mod11 check digit for 1111111 is 4 (literature)");

    const std::string expected =
        "110"
        "100100100110"
        "100100110100"
        "100100110110"
        "100110100100"
        "100110100110"
        "100110110100"
        "100110110110"
        "100110100100"
        "1001";
    check(encodeModules("12345674") == expected, "module map matches the Wikipedia example");
}

static void testRoundTrips() {
    const std::string data = "1234567";
    const ChecksumMode modes[] = {ChecksumMode::Mod10, ChecksumMode::Mod11,
                                  ChecksumMode::Mod1010, ChecksumMode::Mod1110};
    const char* names[] = {"mod10", "mod11", "mod1010", "mod1110"};
    for (int i = 0; i < 4; ++i) {
        const std::string full = withChecksum(data, modes[i]);
        RenderSpec spec;
        Image img = render(encodeModules(full), spec);
        Options options;
        const Result r = decode(img, modes[i], options);
        check(r.found && r.text == full && r.checksum == ChecksumStatus::Valid,
              std::string("round trip ") + names[i]);
    }
}

static void testAllDigits() {
    const std::string full = withChecksum("0123456789", ChecksumMode::Mod10);
    RenderSpec spec;
    spec.modulePx = 2;
    spec.barHeight = 48;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Mod10, options);
    check(r.found && r.text == full && r.checksum == ChecksumStatus::Valid, "all digits mod10");
}

static void testVertical() {
    const std::string full = withChecksum("1357902", ChecksumMode::Mod10);
    RenderSpec spec;
    spec.vertical = true;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Mod10, options);
    check(r.found && r.text == full && r.vertical && r.checksum == ChecksumStatus::Valid,
          "vertical barcode");
}

static void testRotated180() {
    const std::string full = withChecksum("24680", ChecksumMode::Mod10);
    RenderSpec spec;
    spec.flipX = true;
    Image img = render(encodeModules(full), spec);
    Options options;
    Result r = decode(img, ChecksumMode::Mod10, options);
    check(r.found && r.text == full && r.reversed && r.checksum == ChecksumStatus::Valid,
          "180 degree rotated barcode");
}

static void testNoiseAndBlur() {
    const std::string full = withChecksum("9876543210", ChecksumMode::Mod1010);
    RenderSpec spec;
    spec.modulePx = 4;
    spec.barHeight = 80;
    spec.noise = 25;
    spec.blurPasses = 1;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Mod1010, options);
    check(r.found && r.text == full && r.checksum == ChecksumStatus::Valid,
          "noise + blur mod1010");
}

static void testWrongChecksum() {
    const std::string full = withChecksum("123456789", ChecksumMode::Mod10);
    check(mod10CheckDigit("123456789") == 7 && mod11CheckDigit("123456789") == 2,
          "123456789 check digits differ between mod10 and mod11");
    RenderSpec spec;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Mod11, options);
    check(r.found && r.text == full && r.checksum == ChecksumStatus::Invalid,
          "wrong checksum mode reported invalid");
}

static void testNoChecksumMode() {
    const std::string full = withChecksum("424242", ChecksumMode::Mod10);
    RenderSpec spec;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::None, options);
    check(r.found && r.text == full && r.checksum == ChecksumStatus::None,
          "checksum mode none returns raw digits");
}

static void testAutoMode() {
    struct AutoCase {
        std::string full;
        ChecksumMode built;
        const char* scheme;
    };
    const AutoCase cases[] = {
        {withChecksum("123456789", ChecksumMode::Mod10), ChecksumMode::Mod10, "mod10"},
        {withChecksum("1111111", ChecksumMode::Mod11), ChecksumMode::Mod11, "mod11"},
        {withChecksum("123456789", ChecksumMode::Mod1010), ChecksumMode::Mod1010, "mod1010"},
        {withChecksum("1111111", ChecksumMode::Mod1110), ChecksumMode::Mod1110, "mod1110"},
        {"11111114", ChecksumMode::Mod11, "mod11"},
        {"111111142", ChecksumMode::Mod1110, "mod1110"},
    };
    for (const AutoCase& c : cases) {
        RenderSpec spec;
        Image img = render(encodeModules(c.full), spec);
        Options options;
        const Result r = decode(img, ChecksumMode::Auto, options);
        check(r.found && r.text == c.full && r.checksum == ChecksumStatus::Valid &&
                  r.scheme == c.scheme,
              std::string("auto detect ") + c.scheme + " for " + c.full);
    }
}

static void testUniformRejected() {
    const std::string full = "0000";
    RenderSpec spec;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Auto, options);
    check(!r.found, "uniform all-zero string rejected");
}

static void testLowContrast() {
    const std::string full = withChecksum("1234567", ChecksumMode::Mod10);
    RenderSpec spec;
    spec.dark = 112;
    spec.light = 134;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Mod10, options);
    check(r.found && r.text == full && r.checksum == ChecksumStatus::Valid,
          "low contrast decoded (template correlation)");
}

static void testTinyModules() {
    const std::string full = withChecksum("1234567", ChecksumMode::Mod10);
    RenderSpec spec;
    spec.modulePx = 1;
    spec.barHeight = 32;
    Image img = render(encodeModules(full), spec);
    Options options;
    const Result r = decode(img, ChecksumMode::Mod10, options);
    check(r.found && r.text == full && r.checksum == ChecksumStatus::Valid,
          "1px modules decode (subpixel + upscale)");
}

static void testBrokenBarRepair() {
    const std::string full = withChecksum("1234567", ChecksumMode::Mod10);
    const std::string modules = encodeModules(full);
    RenderSpec spec;
    Image img = render(modules, spec);
    const size_t splitIdx = modules.find("11", 3);
    check(splitIdx != std::string::npos, "split target found in module string");
    if (splitIdx != std::string::npos) {
        const int x = (spec.quietModules + static_cast<int>(splitIdx)) * spec.modulePx + 2;
        for (int row = 0; row < img.height; ++row) {
            img.pixels[static_cast<size_t>(row) * img.stride + x] = spec.light;
        }
        Options options;
        const Result r = decode(img, ChecksumMode::Mod10, options);
        check(r.found && r.text == full && r.checksum == ChecksumStatus::Valid,
              "bar split by 1px spec repaired");
    }
}

static void testWideSpaceVariant() {
    const std::string fullMod10 = withChecksum("0904783", ChecksumMode::Mod10);
    RenderSpec spec;
    Image img = render(encodeModulesWideSpace(fullMod10), spec);
    Options options;
    const Result r1 = decode(img, ChecksumMode::Auto, options);
    check(r1.found && r1.text == fullMod10 && r1.checksum == ChecksumStatus::Valid,
          "wide-space variant mod10 (09047838)");

    const std::string fullMod1010 = withChecksum("0904783", ChecksumMode::Mod1010);
    Image img2 = render(encodeModulesWideSpace(fullMod1010), spec);
    const Result r2 = decode(img2, ChecksumMode::Auto, options);
    check(r2.found && r2.text == fullMod1010 && r2.checksum == ChecksumStatus::Valid &&
              r2.scheme == "mod1010",
          "wide-space variant mod1010 (090478389)");

    RenderSpec blurry;
    blurry.modulePx = 2;
    blurry.noise = 18;
    blurry.blurPasses = 1;
    Image img3 = render(encodeModulesWideSpace(fullMod1010), blurry);
    const Result r3 = decode(img3, ChecksumMode::Auto, options);
    check(r3.found && r3.text == fullMod1010 && r3.checksum == ChecksumStatus::Valid,
          "wide-space variant under noise + blur");
}

static void testNegatives() {
    Image img;
    img.width = 640;
    img.height = 128;
    img.stride = img.width;
    img.pixels.resize(static_cast<size_t>(img.width) * img.height);
    for (uint8_t& p : img.pixels) {
        p = static_cast<uint8_t>(xorshift() & 0xff);
    }
    Options options;
    const Result noise = decode(img, ChecksumMode::Mod10, options);
    check(!noise.found, "random noise rejected");

    Image blank = img;
    std::fill(blank.pixels.begin(), blank.pixels.end(), 255);
    const Result white = decode(blank, ChecksumMode::Mod10, options);
    check(!white.found, "blank image rejected");
}

int main() {
    testWikipediaFixture();
    testRoundTrips();
    testAllDigits();
    testVertical();
    testRotated180();
    testNoiseAndBlur();
    testWrongChecksum();
    testNoChecksumMode();
    testAutoMode();
    testUniformRejected();
    testLowContrast();
    testTinyModules();
    testBrokenBarRepair();
    testWideSpaceVariant();
    testNegatives();

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
