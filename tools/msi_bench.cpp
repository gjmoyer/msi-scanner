#include "msi_decoder.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace msi;

int main() {
    const int width = 1280;
    const int height = 720;
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height, 210);

    const int modulePx = 4;
    const int quiet = 12;
    const std::string modules =
        "110"
        "100100110100"
        "100110100100"
        "100110110100"
        "110100100100"
        "100100100100"
        "100100100110"
        "100110100100"
        "1001";
    const int lengthPx = (static_cast<int>(modules.size()) + 2 * quiet) * modulePx;
    const int x0 = (width - lengthPx) / 2;
    const int y0 = height / 2 - 60;
    for (size_t i = 0; i < modules.size(); ++i) {
        if (modules[i] != '1') {
            continue;
        }
        for (int p = 0; p < modulePx; ++p) {
            const int x = x0 + (quiet + static_cast<int>(i)) * modulePx + p;
            for (int y = y0; y < y0 + 120; ++y) {
                frame[static_cast<size_t>(y) * width + x] = 20;
            }
        }
    }

    Options options;
    const int iterations = 300;

    auto run = [&](const char* name, const uint8_t* data) {
        int found = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            const Result r = decodeYPlane(data, width, height, width, ChecksumMode::Mod10, options);
            found += r.found ? 1 : 0;
        }
        const auto end = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(end - start).count() / iterations;
        std::printf("%-28s %8.1f us/frame  (%.2f ms)  found %d/%d\n", name, us, us / 1000.0,
                    found, iterations);
    };

    run("with barcode", frame.data());

    std::vector<uint8_t> empty(frame.size(), 210);
    run("no barcode (full scan)", empty.data());

    return 0;
}
