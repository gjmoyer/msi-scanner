#include "msi_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace msi;

static uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

static bool loadBmp(const char* path, GrayImage& out) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    uint8_t header[54];
    if (std::fread(header, 1, 54, file) != 54 || header[0] != 'B' || header[1] != 'M') {
        std::fprintf(stderr, "not a BMP: %s\n", path);
        std::fclose(file);
        return false;
    }
    const uint32_t dataOffset = le32(header + 10);
    const int32_t width = static_cast<int32_t>(le32(header + 18));
    int32_t storedHeight = static_cast<int32_t>(le32(header + 22));
    const uint16_t bpp = le16(header + 28);
    const uint32_t compression = le32(header + 30);
    const bool topDown = storedHeight < 0;
    const int height = topDown ? -storedHeight : storedHeight;
    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32) || compression != 0) {
        std::fprintf(stderr, "unsupported BMP format (%d bpp, compression %u)\n", bpp, compression);
        std::fclose(file);
        return false;
    }
    const int bytesPerPixel = bpp / 8;
    const size_t rowSize = static_cast<size_t>((width * bytesPerPixel + 3) & ~3);
    std::vector<uint8_t> row(rowSize);
    out.width = width;
    out.height = height;
    out.pixels.assign(static_cast<size_t>(width) * height, 0);
    std::fseek(file, static_cast<long>(dataOffset), SEEK_SET);
    for (int y = 0; y < height; ++y) {
        if (std::fread(row.data(), 1, rowSize, file) != rowSize) {
            std::fprintf(stderr, "truncated BMP: %s\n", path);
            std::fclose(file);
            return false;
        }
        const int destY = topDown ? y : (height - 1 - y);
        uint8_t* dest = out.pixels.data() + static_cast<size_t>(destY) * width;
        for (int x = 0; x < width; ++x) {
            const uint8_t b = row[static_cast<size_t>(x) * bytesPerPixel];
            const uint8_t g = row[static_cast<size_t>(x) * bytesPerPixel + 1];
            const uint8_t r = row[static_cast<size_t>(x) * bytesPerPixel + 2];
            dest[x] = static_cast<uint8_t>((r * 77 + g * 151 + b * 28) >> 8);
        }
    }
    std::fclose(file);
    return true;
}

static bool parseMode(const char* name, ChecksumMode& mode) {
    if (std::strcmp(name, "none") == 0) mode = ChecksumMode::None;
    else if (std::strcmp(name, "mod10") == 0) mode = ChecksumMode::Mod10;
    else if (std::strcmp(name, "mod11") == 0) mode = ChecksumMode::Mod11;
    else if (std::strcmp(name, "mod1010") == 0) mode = ChecksumMode::Mod1010;
    else if (std::strcmp(name, "mod1110") == 0) mode = ChecksumMode::Mod1110;
    else if (std::strcmp(name, "auto") == 0) mode = ChecksumMode::Auto;
    else return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s image.bmp none|mod10|mod11|mod1010|mod1110|auto [minVotes] [rowStep] [minDigits] [maxDigits]\n", argv[0]);
        return 2;
    }
    if (std::getenv("MSI_DEBUG") != nullptr) {
        msi::setDebug(std::atoi(std::getenv("MSI_DEBUG")));
    }
    ChecksumMode mode = ChecksumMode::None;
    if (!parseMode(argv[2], mode)) {
        std::fprintf(stderr, "unknown checksum mode: %s\n", argv[2]);
        return 2;
    }
    GrayImage image;
    if (!loadBmp(argv[1], image)) {
        return 1;
    }

    Options options;
    if (argc > 3) {
        options.minVotes = std::atoi(argv[3]);
    }
    if (argc > 4) {
        options.rowStep = std::atoi(argv[4]);
    }
    if (argc > 5) {
        options.minDigits = std::atoi(argv[5]);
    }
    if (argc > 6) {
        options.maxDigits = std::atoi(argv[6]);
    }

    const Result result = decodeYPlane(image.pixels.data(), image.width, image.height,
                                       image.width, mode, options);
    const char* status = "none";
    if (result.checksum == ChecksumStatus::Valid) status = "valid";
    else if (result.checksum == ChecksumStatus::Invalid) status = "invalid";
    else if (result.checksum == ChecksumStatus::NotApplicable) status = "n/a";
    std::printf("{\"file\":\"%s\",\"found\":%s,\"text\":\"%s\",\"checksum\":\"%s\","
                "\"scheme\":\"%s\",\"votes\":%d,\"module\":%.2f,\"vertical\":%s,\"reversed\":%s}\n",
                argv[1], result.found ? "true" : "false", result.text.c_str(), status,
                result.scheme.c_str(), result.votes, result.moduleWidth,
                result.vertical ? "true" : "false", result.reversed ? "true" : "false");
    return result.found ? 0 : 1;
}
