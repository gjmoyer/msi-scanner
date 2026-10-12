#pragma once

#include <cstdint>
#include <string>

namespace msi {

enum class ChecksumMode : int {
    None = 0,
    Mod10 = 1,
    Mod11 = 2,
    Mod1010 = 3,
    Mod1110 = 4,
    Auto = 5,
};

enum class ChecksumStatus : int {
    None = 0,
    Valid = 1,
    Invalid = 2,
    NotApplicable = 3,
};

struct Options {
    int rowStep = 4;
    int maxRows = 48;
    bool scanVertical = true;
    int maxColumns = 48;
    int minDigits = 4;
    int maxDigits = 12;
    int minVotes = 2;
    bool upscaleRetry = true;
    bool contrastRetry = true;
    bool beamRetry = true;
    bool keepLongestReads = false;
};

struct Result {
    bool found = false;
    std::string text;
    ChecksumStatus checksum = ChecksumStatus::None;
    std::string scheme;
    float moduleWidth = 0.0f;
    int votes = 0;
    bool vertical = false;
    bool reversed = false;
};

void setDebug(int level);

Result decodeYPlane(const uint8_t* y, int width, int height, int rowStride,
                    ChecksumMode mode, const Options& options);

int mod10CheckDigit(const std::string& data);
int mod11CheckDigit(const std::string& data);
ChecksumStatus validateChecksum(const std::string& text, ChecksumMode mode);

}
