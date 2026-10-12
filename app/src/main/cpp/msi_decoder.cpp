#include "msi_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace msi {
namespace {

int g_debug = 0;

struct Run {
    float len;
    bool black;
};

constexpr float kWidthTolerance = 0.45f;
constexpr float kQuietFactor = 3.8f;
constexpr float kMinModuleWidth = 0.9f;
constexpr int kMinContrast = 24;
constexpr int kMaxCandidateTries = 64;
constexpr int kMaxRuns = 4096;
constexpr int kMaxRepairs = 2;
constexpr int kMaxBands = 8;
constexpr int kMaxBandRows = 16;
constexpr int kUpscaleMaxLongEdge = 800;
constexpr int kBandMinHeight = 10;
constexpr int kBandMinScore = 6;
constexpr int kBandAbsoluteRowScore = 25;
constexpr float kBandThresholdFactor = 0.35f;
constexpr float kStretchMaxRange = 208.0f;
constexpr int kMaxBarCenters = 160;
constexpr float kTemplateWidths[2] = {0.5f, 1.0f};
constexpr float kTemplateSigmaMul[3] = {0.35f, 0.6f, 0.95f};
constexpr float kTemplateParsimony = 0.03f;
constexpr float kTemplateMinScore = 0.0f;
constexpr int kTemplateBudgetTotal = 24;

bool approx(float value, float target, float tolerance = kWidthTolerance) {
    return std::fabs(value - target) <= tolerance * target;
}

bool isUniform(const std::string& text) {
    if (text.size() < 4) {
        return false;
    }
    for (size_t i = 1; i < text.size(); ++i) {
        if (text[i] != text[0]) {
            return false;
        }
    }
    return true;
}

class LineScanner {
public:
    void extract(const uint8_t* pixels, int count, int step) {
        runs_.clear();
        if (count < 16) {
            return;
        }
        uint8_t mn = 255;
        uint8_t mx = 0;
        for (int i = 0; i < count; ++i) {
            const uint8_t v = pixels[static_cast<size_t>(i) * step];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (mx - mn < kMinContrast) {
            return;
        }
        const float threshold = (mn + mx) / 2.0f;
        bool black = pixels[0] <= threshold;
        float prevEdge = 0.0f;
        uint8_t prev = pixels[0];
        for (int i = 1; i < count; ++i) {
            const uint8_t cur = pixels[static_cast<size_t>(i) * step];
            const bool b = cur <= threshold;
            if (b != black) {
                float t = 0.5f;
                if (prev != cur) {
                    t = (threshold - prev) / static_cast<float>(cur - prev);
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                }
                const float edge = static_cast<float>(i - 1) + t;
                runs_.push_back({edge - prevEdge, black});
                if (static_cast<int>(runs_.size()) >= kMaxRuns) {
                    runs_.clear();
                    return;
                }
                prevEdge = edge;
                black = b;
            }
            prev = cur;
        }
        runs_.push_back({static_cast<float>(count) - prevEdge, black});
    }

    bool empty() const { return runs_.empty(); }

    std::vector<Run>& runs() { return runs_; }

    void reverse() {
        std::reverse(runs_.begin(), runs_.end());
    }

    bool decodeForward(const Options& opts, std::string& text, float& moduleWidth) {
        const int n = static_cast<int>(runs_.size());
        int tries = 0;
        for (int i = 0; i + 2 < n; ++i) {
            if (!runs_[i].black) {
                continue;
            }
            const float b0 = runs_[i].len;
            const float s0 = runs_[i + 1].len;
            if (s0 <= 0.0f || b0 < 0.8f * s0 || b0 > 3.2f * s0) {
                continue;
            }
            const float m = b0 / 2.0f;
            if (m < kMinModuleWidth) {
                continue;
            }
            if (s0 < 0.6f * m || s0 > 2.6f * m) {
                continue;
            }
            const bool quietBefore = (i == 0) ||
                (!runs_[i - 1].black && runs_[i - 1].len >= kQuietFactor * m);
            if (!quietBefore) {
                continue;
            }
            if (++tries > kMaxCandidateTries) {
                break;
            }
            std::string digits;
            if (decodeFromForward(i + 2, m, opts, digits)) {
                text = std::move(digits);
                moduleWidth = m;
                return true;
            }
            if (g_debug >= 2) {
                std::fprintf(stderr, "[msi] cand i=%d b0=%.1f s0=%.1f m=%.2f rejected\n",
                             i, b0, s0, m);
            }
        }
        return false;
    }

private:
    bool decodeFromForward(int startIndex, float m, const Options& opts, std::string& out) {
        std::vector<Run> seg(runs_.begin() + startIndex, runs_.end());
        if (g_debug >= 2) {
            std::fprintf(stderr, "[msi] seg@%d m=%.2f:", startIndex, m);
            for (int q = 0; q < 10 && q < static_cast<int>(seg.size()); ++q) {
                std::fprintf(stderr, " %.0f%c", seg[q].len, seg[q].black ? 'B' : 'W');
            }
            std::fprintf(stderr, "\n");
        }
        int k = 0;
        int repairs = 0;
        int value = 0;
        int bitIndex = 0;
        std::string digits;
        while (true) {
            const int n = static_cast<int>(seg.size());
            if (bitIndex == 0 && k + 2 < n && seg[k].black && !seg[k + 1].black &&
                seg[k + 2].black) {
                const float b1 = seg[k].len;
                const float s = seg[k + 1].len;
                const float b2 = seg[k + 2].len;
                if (approx(b1, m, 0.6f) && s >= 1.4f * m && s <= 3.6f * m &&
                    approx(b2, m, 0.6f)) {
                    const bool quiet = (k + 3 >= n) ||
                        (!seg[k + 3].black && seg[k + 3].len >= kQuietFactor * m);
                    if (quiet) {
                        if (static_cast<int>(digits.size()) >= opts.minDigits) {
                            out = std::move(digits);
                            return true;
                        }
                        return false;
                    }
                }
            }
            if (static_cast<int>(digits.size()) >= opts.maxDigits) {
                return false;
            }
            if (bitIndex == 0) {
                value = 0;
            }
            if (k + 1 >= n) {
                return false;
            }
            const Run& bar = seg[k];
            const Run& space = seg[k + 1];
            const float sum = bar.len + space.len;
            const int bit = bar.len > 1.5f * m ? 1 : 0;
            const bool valid = bar.black && !space.black &&
                bar.len <= 2.8f * m &&
                space.len >= 0.5f * m && space.len <= 3.4f * m &&
                sum >= 2.6f * m && sum <= 4.4f * m;
            if (!valid && repairs < kMaxRepairs && bar.black && !space.black &&
                k + 2 < n && seg[k + 2].black && space.len < 0.6f * m) {
                const float merged = seg[k].len + seg[k + 1].len + seg[k + 2].len;
                seg.erase(seg.begin() + k + 1, seg.begin() + k + 3);
                seg[k].len = merged;
                ++repairs;
                continue;
            }
            if (!valid) {
                if (g_debug >= 2) {
                    std::fprintf(stderr,
                                 "[msi] fail@digits=%s bit=%d bar=%.1f space=%.1f m=%.2f k=%d\n",
                                 digits.c_str(), bitIndex, bar.len, space.len, m, k);
                }
                return false;
            }
            value = (value << 1) | bit;
            const float divisor = sum > 3.5f * m ? 4.0f : 3.0f;
            m = 0.75f * m + 0.25f * (sum / divisor);
            k += 2;
            ++bitIndex;
            if (bitIndex == 4) {
                if (value > 9) {
                    return false;
                }
                digits.push_back(static_cast<char>('0' + value));
                bitIndex = 0;
            }
        }
    }

    std::vector<Run> runs_;
};

struct Band {
    int y0;
    int y1;
    int score;
    int peak;
};

int findBands(const uint8_t* y, int width, int height, int rowStride,
              Band* out, int cap) {
    std::vector<int> rowScore(height, 0);
    int peak = 0;
    for (int r = 0; r < height; ++r) {
        const uint8_t* row = y + static_cast<size_t>(r) * rowStride;
        uint8_t mn = 255;
        uint8_t mx = 0;
        for (int x = 0; x < width; ++x) {
            mn = std::min(mn, row[x]);
            mx = std::max(mx, row[x]);
        }
        int s = 0;
        if (mx - mn >= kMinContrast) {
            const int threshold = (mn + mx) / 2;
            bool prev = row[0] <= threshold;
            for (int x = 1; x < width; ++x) {
                const bool cur = row[x] <= threshold;
                if (cur != prev) {
                    ++s;
                    prev = cur;
                }
            }
        }
        rowScore[r] = s;
        peak = std::max(peak, s);
    }
    if (peak < kBandMinScore) {
        return 0;
    }
    const int threshold = std::max(2, static_cast<int>(peak * kBandThresholdFactor));
    int count = 0;
    int start = -1;
    int score = 0;
    int bandPeak = 0;
    for (int r = 0; r <= height; ++r) {
        if (r < height && (rowScore[r] >= threshold || rowScore[r] >= kBandAbsoluteRowScore)) {
            if (start < 0) {
                start = r;
                score = 0;
                bandPeak = 0;
            }
            score += rowScore[r];
            bandPeak = std::max(bandPeak, rowScore[r]);
        } else if (start >= 0) {
            if (r - start >= kBandMinHeight && count < cap) {
                out[count].y0 = start;
                out[count].y1 = r;
                out[count].score = score;
                out[count].peak = bandPeak;
                ++count;
            }
            start = -1;
        }
    }
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (out[j].score > out[i].score) {
                std::swap(out[i], out[j]);
            }
        }
    }
    return count;
}

struct ChecksumInfo {
    ChecksumStatus status = ChecksumStatus::None;
    std::string scheme;
    int strength = 0;
};

bool mod10Valid(const std::string& text) {
    if (text.size() < 2) return false;
    return text.back() - '0' == mod10CheckDigit(text.substr(0, text.size() - 1));
}

bool mod11Valid(const std::string& text) {
    if (text.size() < 2) return false;
    const int check = mod11CheckDigit(text.substr(0, text.size() - 1));
    return check >= 0 && text.back() - '0' == check;
}

bool mod1010Valid(const std::string& text) {
    if (text.size() < 3) return false;
    const std::string data = text.substr(0, text.size() - 2);
    const int c1 = mod10CheckDigit(data);
    const int c2 = mod10CheckDigit(data + static_cast<char>('0' + c1));
    return text[text.size() - 2] - '0' == c1 && text.back() - '0' == c2;
}

bool mod1110Valid(const std::string& text) {
    if (text.size() < 3) return false;
    const std::string data = text.substr(0, text.size() - 2);
    const int c1 = mod11CheckDigit(data);
    if (c1 < 0) return false;
    const int c2 = mod10CheckDigit(data + static_cast<char>('0' + c1));
    return text[text.size() - 2] - '0' == c1 && text.back() - '0' == c2;
}

ChecksumInfo analyzeChecksum(const std::string& text, ChecksumMode mode) {
    ChecksumInfo info;
    if (mode == ChecksumMode::None) {
        return info;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            info.status = ChecksumStatus::NotApplicable;
            return info;
        }
    }
    switch (mode) {
        case ChecksumMode::Mod10:
            if (text.size() < 2) {
                info.status = ChecksumStatus::NotApplicable;
                return info;
            }
            info.scheme = "mod10";
            info.strength = mod10Valid(text) ? 1 : 0;
            info.status = info.strength ? ChecksumStatus::Valid : ChecksumStatus::Invalid;
            return info;
        case ChecksumMode::Mod11:
            if (text.size() < 2) {
                info.status = ChecksumStatus::NotApplicable;
                return info;
            }
            info.scheme = "mod11";
            info.strength = mod11Valid(text) ? 1 : 0;
            info.status = info.strength ? ChecksumStatus::Valid : ChecksumStatus::Invalid;
            return info;
        case ChecksumMode::Mod1010:
            if (text.size() < 3) {
                info.status = ChecksumStatus::NotApplicable;
                return info;
            }
            info.scheme = "mod1010";
            info.strength = mod1010Valid(text) ? 2 : 0;
            info.status = info.strength ? ChecksumStatus::Valid : ChecksumStatus::Invalid;
            return info;
        case ChecksumMode::Mod1110:
            if (text.size() < 3) {
                info.status = ChecksumStatus::NotApplicable;
                return info;
            }
            info.scheme = "mod1110";
            info.strength = mod1110Valid(text) ? 2 : 0;
            info.status = info.strength ? ChecksumStatus::Valid : ChecksumStatus::Invalid;
            return info;
        case ChecksumMode::Auto:
            if (mod1110Valid(text)) {
                info.scheme = "mod1110";
                info.strength = 2;
            } else if (mod1010Valid(text)) {
                info.scheme = "mod1010";
                info.strength = 2;
            } else if (mod11Valid(text)) {
                info.scheme = "mod11";
                info.strength = 1;
            } else if (mod10Valid(text)) {
                info.scheme = "mod10";
                info.strength = 1;
            }
            info.status = info.strength ? ChecksumStatus::Valid : ChecksumStatus::Invalid;
            return info;
        default:
            break;
    }
    return info;
}

struct BarCenter {
    float center;
    float width;
};

int findBarCenters(const float* p, int n, BarCenter* bars, int cap) {
    float mean = 0.0f;
    for (int i = 0; i < n; ++i) {
        mean += p[i];
    }
    mean /= static_cast<float>(n);
    float variance = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float d = p[i] - mean;
        variance += d * d;
    }
    const float stddev = std::sqrt(variance / static_cast<float>(n));
    float depth = 0.4f * stddev;
    if (depth < 3.0f) depth = 3.0f;
    if (depth > 8.0f) depth = 8.0f;
    int count = 0;
    for (int i = 1; i + 1 < n && count < cap; ++i) {
        if (p[i] < p[i - 1] && p[i] <= p[i + 1] && p[i] < mean - depth) {
            if (count > 0 && i - bars[count - 1].center < 3.0f) {
                if (p[i] < p[static_cast<int>(bars[count - 1].center)]) {
                    bars[count - 1].center = static_cast<float>(i);
                }
                continue;
            }
            bars[count].center = static_cast<float>(i);
            ++count;
        }
    }
    for (int b = 0; b < count; ++b) {
        const int c = static_cast<int>(bars[b].center);
        int le = c;
        int re = c;
        float best = -1.0f;
        for (int i = std::max(1, c - 14); i < c; ++i) {
            const float gd = std::fabs(p[i + 1] - p[i - 1]) * 0.5f;
            if (gd > best) {
                best = gd;
                le = i;
            }
        }
        best = -1.0f;
        for (int i = c; i < std::min(n - 1, c + 14); ++i) {
            const float gd = std::fabs(p[i + 1] - p[i - 1]) * 0.5f;
            if (gd > best) {
                best = gd;
                re = i;
            }
        }
        bars[b].width = static_cast<float>(re - le);
    }
    return count;
}

int filterBarCenters(BarCenter* bars, int count, float& medianOut) {
    if (count < 8) {
        return count;
    }
    float spacings[kMaxBarCenters];
    for (int i = 0; i + 1 < count; ++i) {
        spacings[i] = bars[i + 1].center - bars[i].center;
    }
    std::sort(spacings, spacings + count - 1);
    const float median = spacings[(count - 1) / 2];
    medianOut = median;
    if (median < 1.5f || median > 48.0f) {
        return 0;
    }
    const float mergeDistance = 0.5f * median;
    BarCenter filtered[kMaxBarCenters];
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        if (kept > 0 && bars[i].center - filtered[kept - 1].center < mergeDistance) {
            if (bars[i].width > filtered[kept - 1].width) {
                filtered[kept - 1] = bars[i];
            }
            continue;
        }
        filtered[kept++] = bars[i];
    }
    for (int i = 0; i < kept; ++i) {
        bars[i] = filtered[i];
    }
    return kept;
}

void buildVariantPatterns(char buffer[10][17], const char* pointers[10]) {
    for (int d = 0; d < 10; ++d) {
        int p = 0;
        for (int bit = 3; bit >= 0; --bit) {
            const char* piece = ((d >> bit) & 1) ? "1100" : "1000";
            for (int i = 0; i < 4; ++i) {
                buffer[d][p++] = piece[i];
            }
        }
        buffer[d][16] = '\0';
        pointers[d] = buffer[d];
    }
}

bool buildDigitTemplates(const char* const* patterns, float module, float sigma,
                         float tmpl[10][96], int& tmplLen) {
    const int patternLen = static_cast<int>(std::strlen(patterns[0]));
    int len = static_cast<int>(patternLen * module + 0.5f);
    if (len > 96) {
        len = 96;
    }
    if (len < 8) {
        return false;
    }
    tmplLen = len;
    int kernelRadius = static_cast<int>(sigma * 3.0f);
    if (kernelRadius < 1) kernelRadius = 1;
    if (kernelRadius > 6) kernelRadius = 6;
    for (int d = 0; d < 10; ++d) {
        float square[96];
        for (int i = 0; i < len; ++i) {
            int moduleIndex = static_cast<int>(i / module);
            if (moduleIndex > patternLen - 1) moduleIndex = patternLen - 1;
            square[i] = patterns[d][moduleIndex] == '1' ? -1.0f : 1.0f;
        }
        for (int i = 0; i < len; ++i) {
            float sum = 0.0f;
            float weightSum = 0.0f;
            for (int k = -kernelRadius; k <= kernelRadius; ++k) {
                int j = i + k;
                if (j < 0) j = 0;
                if (j >= len) j = len - 1;
                const float w = std::exp(-0.5f * k * k / (sigma * sigma));
                sum += square[j] * w;
                weightSum += w;
            }
            tmpl[d][i] = sum / weightSum;
        }
        float mean = 0.0f;
        for (int i = 0; i < len; ++i) {
            mean += tmpl[d][i];
        }
        mean /= len;
        float norm = 0.0f;
        for (int i = 0; i < len; ++i) {
            tmpl[d][i] -= mean;
            norm += tmpl[d][i] * tmpl[d][i];
        }
        norm = std::sqrt(norm);
        if (norm > 1e-6f) {
            for (int i = 0; i < len; ++i) {
                tmpl[d][i] /= norm;
            }
        }
    }
    return true;
}

bool decodeProfileByTemplates(const float* p, int n, const Options& opts, ChecksumMode mode,
                              std::string& digitsOut, float& moduleOut, float& scoreOut) {
    BarCenter bars[kMaxBarCenters];
    int count = findBarCenters(p, n, bars, kMaxBarCenters);
    if (count < 1 + 4 * opts.minDigits + 2) {
        return false;
    }
    float median = 0.0f;
    count = filterBarCenters(bars, count, median);
    if (count < 1 + 4 * opts.minDigits + 2) {
        return false;
    }

    static const char* kStandardPatterns[10] = {
        "100100100100", "100100100110", "100100110100", "100100110110",
        "100110100100", "100110100110", "100110110100", "100110110110",
        "110100100100", "110100100110",
    };
    char variantBuffer[10][17];
    const char* variantPointers[10];
    buildVariantPatterns(variantBuffer, variantPointers);
    struct Family {
        const char* const* patterns;
        float module;
    };
    const Family families[2] = {
        {kStandardPatterns, median / 3.0f},
        {variantPointers, median / 4.0f},
    };

    struct Cand {
        std::string digits;
        float score;
        int strength;
        int fit;
        float module;
    };
    std::vector<Cand> cands;
    float tmpl[10][96];

    for (const Family& family : families) {
    const float module = family.module;
    if (module < 0.5f || module > 12.0f) {
        continue;
    }
    for (int sgi = 0; sgi < 3; ++sgi) {
        int tmplLen = 0;
        if (!buildDigitTemplates(family.patterns, module,
                                 kTemplateSigmaMul[sgi] * module, tmpl, tmplLen)) {
            continue;
        }
        const int starts = std::min(5, count);
        for (int sIdx = 0; sIdx < starts; ++sIdx) {
            if (bars[sIdx].width < 1.2f * module) {
                continue;
            }
            for (int nd = opts.minDigits; nd <= opts.maxDigits; ++nd) {
                const int need = 1 + 4 * nd + 2;
                if (sIdx + need > count + 6) {
                    continue;
                }
                if (sIdx + need < count - 6) {
                    continue;
                }
                std::string digits;
                float totalScore = 0.0f;
                bool ok = true;
                for (int d = 0; d < nd; ++d) {
                    const int barIdx = sIdx + 1 + d * 4;
                    if (barIdx >= count) {
                        ok = false;
                        break;
                    }
                    const float bc = bars[barIdx].center;
                    int bestD = -1;
                    float bestC = -2.0f;
                    for (int w = 0; w < 2; ++w) {
                        const float dStart = bc - kTemplateWidths[w] * module;
                        for (int so = -1; so <= 1; ++so) {
                            const float ds = dStart + so * 0.5f * module;
                            const int i0 = static_cast<int>(ds + 0.5f);
                            if (i0 < 0 || i0 + tmplLen > n) {
                                continue;
                            }
                            float seg[96];
                            float mean = 0.0f;
                            for (int i = 0; i < tmplLen; ++i) {
                                seg[i] = p[i0 + i];
                                mean += seg[i];
                            }
                            mean /= tmplLen;
                            float norm = 0.0f;
                            for (int i = 0; i < tmplLen; ++i) {
                                seg[i] -= mean;
                                norm += seg[i] * seg[i];
                            }
                            norm = std::sqrt(norm);
                            if (norm < 1e-6f) {
                                continue;
                            }
                            for (int i = 0; i < tmplLen; ++i) {
                                seg[i] /= norm;
                            }
                            for (int dig = 0; dig < 10; ++dig) {
                                float c = 0.0f;
                                for (int i = 0; i < tmplLen; ++i) {
                                    c += seg[i] * tmpl[dig][i];
                                }
                                if (c > bestC) {
                                    bestC = c;
                                    bestD = dig;
                                }
                            }
                        }
                    }
                    if (bestD < 0) {
                        ok = false;
                        break;
                    }
                    digits.push_back(static_cast<char>('0' + bestD));
                    totalScore += bestC;
                }
                if (!ok) {
                    continue;
                }
                const ChecksumInfo info = analyzeChecksum(digits, mode);
                if (info.status != ChecksumStatus::Valid) {
                    continue;
                }
                const float rawScore = totalScore / static_cast<float>(nd);
                if (rawScore < kTemplateMinScore) {
                    continue;
                }
                const float score = rawScore + sIdx * 0.001f - kTemplateParsimony * sgi;
                int fit = sIdx + need - count;
                if (fit < 0) fit = -fit;
                cands.push_back({digits, score, info.strength, fit, module});
            }
        }
    }
    }
    if (cands.empty()) {
        return false;
    }

    std::vector<char> drop(cands.size(), 0);
    for (size_t i = 0; i < cands.size(); ++i) {
        for (size_t j = 0; j < cands.size(); ++j) {
            if (i == j || cands[j].digits.size() <= cands[i].digits.size()) {
                continue;
            }
            if (cands[j].digits.compare(0, cands[i].digits.size(), cands[i].digits) != 0) {
                continue;
            }
            if (cands[j].strength > cands[i].strength && cands[j].fit <= cands[i].fit) {
                drop[i] = 1;
            } else if (cands[j].strength <= cands[i].strength || cands[j].fit > cands[i].fit) {
                drop[j] = 1;
            }
        }
    }
    int best = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (drop[i]) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const Cand& a = cands[i];
        const Cand& b = cands[best];
        if (a.fit < b.fit ||
            (a.fit == b.fit &&
             (a.strength > b.strength ||
              (a.strength == b.strength && a.score > b.score)))) {
            best = static_cast<int>(i);
        }
    }
    if (best < 0) {
        return false;
    }
    digitsOut = cands[best].digits;
    moduleOut = cands[best].module;
    scoreOut = cands[best].score;
    return true;
}

void extractProfile(const uint8_t* pixels, int count, int step, std::vector<float>& out) {
    out.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float v = pixels[static_cast<size_t>(i) * step];
        if (i > 0 && i + 1 < count) {
            out[i] = (pixels[static_cast<size_t>(i - 1) * step] +
                      2.0f * v +
                      pixels[static_cast<size_t>(i + 1) * step]) * 0.25f;
        } else {
            out[i] = v;
        }
    }
}

struct Vote {
    std::string text;
    float moduleSum = 0.0f;
    int count = 0;
    bool vertical = false;
    bool reversed = false;
};


Result selectWinner(std::vector<Vote> pool, ChecksumMode mode, const Options& options) {
    Result result;
    if (pool.empty()) {
        return result;
    }
    std::vector<ChecksumInfo> info(pool.size());
    for (size_t i = 0; i < pool.size(); ++i) {
        info[i] = analyzeChecksum(pool[i].text, mode);
        if (g_debug) {
            std::fprintf(stderr, "[msi] vote %s count=%d status=%d scheme=%s strength=%d\n",
                         pool[i].text.c_str(), pool[i].count,
                         static_cast<int>(info[i].status), info[i].scheme.c_str(),
                         info[i].strength);
        }
    }

    if (mode == ChecksumMode::Auto && pool.size() > 1) {
        auto stripped = [](const std::string& text) {
            size_t i = 0;
            while (i + 1 < text.size() && text[i] == '0') {
                ++i;
            }
            return text.substr(i);
        };
        std::vector<char> used(pool.size(), 0);
        std::vector<Vote> merged;
        std::vector<ChecksumInfo> mergedInfo;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const std::string key = stripped(pool[i].text);
            int rep = static_cast<int>(i);
            int totalCount = 0;
            float totalModule = 0.0f;
            bool anyValid = false;
            for (size_t j = i; j < pool.size(); ++j) {
                if (used[j] || stripped(pool[j].text) != key) {
                    continue;
                }
                used[j] = 1;
                totalCount += pool[j].count;
                totalModule += pool[j].moduleSum;
                if (info[j].status == ChecksumStatus::Valid) {
                    anyValid = true;
                }
                if (pool[j].text.size() > pool[rep].text.size()) {
                    rep = static_cast<int>(j);
                }
            }
            Vote mergedVote = pool[rep];
            mergedVote.count = totalCount;
            mergedVote.moduleSum = totalModule;
            merged.push_back(mergedVote);
            mergedInfo.push_back(anyValid ? info[rep] : analyzeChecksum(mergedVote.text, mode));
        }
        pool = std::move(merged);
        info = std::move(mergedInfo);
    }

    std::vector<char> drop(pool.size(), 0);
    for (size_t i = 0; i < pool.size(); ++i) {
        for (size_t j = 0; j < pool.size(); ++j) {
            if (i == j || pool[j].text.size() <= pool[i].text.size()) {
                continue;
            }
            if (pool[j].text.compare(0, pool[i].text.size(), pool[i].text) != 0) {
                continue;
            }
            if (info[j].strength > info[i].strength &&
                pool[j].count + 1 >= pool[i].count) {
                drop[i] = 1;
            }
        }
    }

    int best = -1;
    int bestDouble = -1;
    for (size_t i = 0; i < pool.size(); ++i) {
        if (drop[i]) {
            continue;
        }
        if (mode == ChecksumMode::Auto && info[i].strength == 0) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
        } else {
            const Vote& a = pool[i];
            const Vote& b = pool[best];
            bool better = false;
            if (a.count != b.count) {
                better = a.count > b.count;
            } else if (info[i].strength != info[best].strength) {
                better = info[i].strength > info[best].strength;
            } else {
                better = a.text.size() > b.text.size();
            }
            if (better) {
                best = static_cast<int>(i);
            }
        }
        if (info[i].strength >= 2) {
            if (bestDouble < 0 || pool[i].count > pool[bestDouble].count ||
                (pool[i].count == pool[bestDouble].count &&
                 pool[i].text.size() > pool[bestDouble].text.size())) {
                bestDouble = static_cast<int>(i);
            }
        }
    }
    if (best < 0) {
        return result;
    }
    if (bestDouble >= 0 && info[best].strength < 2 &&
        pool[bestDouble].count * 2 >= pool[best].count) {
        best = bestDouble;
    }
    const bool accepted = pool[best].count >= options.minVotes;
    if (!accepted) {
        return result;
    }

    result.found = true;
    result.text = pool[best].text;
    result.checksum = info[best].status;
    result.scheme = info[best].scheme;
    result.moduleWidth = pool[best].moduleSum / static_cast<float>(pool[best].count);
    result.votes = pool[best].count;
    result.vertical = pool[best].vertical;
    result.reversed = pool[best].reversed;
    if (g_debug) {
        std::fprintf(stderr, "[msi] winner=%s scheme=%s votes=%d\n",
                     result.text.c_str(), result.scheme.c_str(), result.votes);
    }
    return result;
}

Result decodeCore(const uint8_t* y, int width, int height, int rowStride,
                  ChecksumMode mode, const Options& options) {
    Result result;
    if (y == nullptr || width < 16 || height < 8 || rowStride < width) {
        return result;
    }

    const bool earlyExit = mode != ChecksumMode::Auto;
    std::vector<Vote> votes;
    bool done = false;

    auto addVote = [&](const std::string& text, float moduleWidth, bool vertical,
                       bool reversed) -> bool {
        if (isUniform(text)) {
            return false;
        }
        for (Vote& v : votes) {
            if (v.text == text && v.vertical == vertical && v.reversed == reversed) {
                v.count += 1;
                v.moduleSum += moduleWidth;
                return earlyExit && v.count >= options.minVotes;
            }
        }
        votes.push_back({text, moduleWidth, 1, vertical, reversed});
        return earlyExit && options.minVotes <= 1;
    };

    LineScanner scanner;
    std::vector<uint32_t> avgRow;
    std::vector<float> profile;
    int templateAttempts = 0;
    int lineIndex = 0;
    int lastVoteLine = -100;
    std::string lastVoteText;

    auto decodeLine = [&](const uint8_t* line, int count, int step, bool vertical) -> bool {
        std::string text;
        float moduleWidth = 0.0f;
        bool reversed = false;
        bool ok = false;
        scanner.extract(line, count, step);
        if (!scanner.empty()) {
            ok = scanner.decodeForward(options, text, moduleWidth);
            if (!ok) {
                scanner.reverse();
                reversed = true;
                ok = scanner.decodeForward(options, text, moduleWidth);
            }
        }
        if (!ok && templateAttempts < kTemplateBudgetTotal) {
            ++templateAttempts;
            float score = 0.0f;
            extractProfile(line, count, step, profile);
            ok = decodeProfileByTemplates(profile.data(), static_cast<int>(profile.size()),
                                          options, mode, text, moduleWidth, score);
            reversed = false;
            if (!ok) {
                std::reverse(profile.begin(), profile.end());
                ok = decodeProfileByTemplates(profile.data(), static_cast<int>(profile.size()),
                                              options, mode, text, moduleWidth, score);
                reversed = true;
            }
        }
        ++lineIndex;
        if (g_debug) {
            std::fprintf(stderr, "[msi] line runs=%zu %s %s\n",
                         scanner.runs().size(), vertical ? "V" : "H",
                         ok ? text.c_str() : "miss");
        }
        if (!ok) {
            return false;
        }
        if (lineIndex - lastVoteLine < 3 && text == lastVoteText) {
            return false;
        }
        lastVoteLine = lineIndex;
        lastVoteText = text;
        return addVote(text, moduleWidth, vertical, reversed);
    };

    Band bands[kMaxBands];
    const int bandCount = findBands(y, width, height, rowStride, bands, kMaxBands);
    if (g_debug) {
        std::fprintf(stderr, "[msi] bands=%d\n", bandCount);
    }
    int bandOrder[kMaxBands];
    for (int i = 0; i < bandCount; ++i) {
        bandOrder[i] = i;
    }
    for (int bi = 0; bi < bandCount && !done; ++bi) {
        const Band& band = bands[bi];
        const int bandHeight = band.y1 - band.y0;
        int y0 = band.y0 + bandHeight / 4;
        int y1 = band.y1 - bandHeight / 4;
        if (y1 <= y0) {
            y1 = std::min(band.y1, y0 + 1);
        }
        avgRow.assign(static_cast<size_t>(width), 0);
        int rows = 0;
        for (int yy = y0; yy < y1; ++yy) {
            const uint8_t* row = y + static_cast<size_t>(yy) * rowStride;
            for (int x = 0; x < width; ++x) {
                avgRow[x] += row[x];
            }
            ++rows;
        }
        std::vector<uint8_t> avgBytes(static_cast<size_t>(width));
        for (int x = 0; x < width; ++x) {
            avgBytes[x] = static_cast<uint8_t>(avgRow[x] / rows);
        }
        done = decodeLine(avgBytes.data(), width, 1, false);
        const int stride = std::max(1, (bandHeight - 2) / kMaxBandRows);
        for (int yy = band.y0 + 1; yy < band.y1 - 1 && !done; yy += stride) {
            done = decodeLine(y + static_cast<size_t>(yy) * rowStride, width, 1, false);
        }
    }

    if (!done && g_debug) {
        std::fprintf(stderr, "[msi] band tier votes=%zu\n", votes.size());
    }

    if (!done) {
        const int maxRows = std::max(1, options.maxRows);
        const int coverageStep = (height + maxRows - 1) / maxRows;
        const int rowStep = std::max(std::max(1, options.rowStep), coverageStep);
        for (int row = rowStep / 2; row < height && !done; row += rowStep) {
            done = decodeLine(y + static_cast<size_t>(row) * rowStride, width, 1, false);
        }
    }

    if (!done && options.scanVertical) {
        const int colStep = std::max(1, width / std::max(1, options.maxColumns));
        int scanned = 0;
        for (int x = colStep / 2; x < width && scanned < options.maxColumns && !done;
             x += colStep, ++scanned) {
            done = decodeLine(y + x, height, rowStride, true);
        }
    }

    return selectWinner(std::move(votes), mode, options);
}

}

void setDebug(int level) {
    g_debug = level;
}

int mod10CheckDigit(const std::string& data) {
    int sum = 0;
    bool doubleDigit = true;
    for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
        const int d = data[static_cast<size_t>(i)] - '0';
        const int product = d * (doubleDigit ? 2 : 1);
        sum += product / 10 + product % 10;
        doubleDigit = !doubleDigit;
    }
    return (10 - sum % 10) % 10;
}

int mod11CheckDigit(const std::string& data) {
    int sum = 0;
    int weight = 2;
    for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
        sum += (data[static_cast<size_t>(i)] - '0') * weight;
        if (++weight > 7) {
            weight = 2;
        }
    }
    const int check = (11 - sum % 11) % 11;
    return check == 10 ? -1 : check;
}

ChecksumStatus validateChecksum(const std::string& text, ChecksumMode mode) {
    return analyzeChecksum(text, mode).status;
}


constexpr int kBeamWidth = 24;
constexpr int kBeamMaxElements = 120;
constexpr int kBeamMaxStarts = 16;

struct BeamHyp {
    float score = 0.0f;
    int n = 0;
    uint8_t classes[kBeamMaxElements];
    uint8_t colors[kBeamMaxElements];
};

bool beamSpaceOk(uint8_t cls, bool variant) {
    if (cls == 0) {
        return true;
    }
    return variant ? (cls == 2 || cls == 3) : (cls == 1 || cls == 2);
}

void beamAppend(BeamHyp& dst, const BeamHyp& src, const uint8_t* cls, const uint8_t* col,
                int count, float extraCost) {
    dst = src;
    for (int i = 0; i < count; ++i) {
        if (dst.n >= kBeamMaxElements) {
            dst.n = -1;
            return;
        }
        dst.classes[dst.n] = cls[i];
        dst.colors[dst.n] = col[i];
        ++dst.n;
    }
    dst.score += extraCost;
}

bool beamExpandBlack(const BeamHyp& hyp, float width, float m, bool variant,
                     BeamHyp* out, int& count) {
    count = 0;
    const uint8_t blackCol[1] = {1};
    for (uint8_t b = 1; b <= 2; ++b) {
        const float cost = std::fabs(b * m - width) / m;
        BeamHyp next;
        const uint8_t c[1] = {b};
        beamAppend(next, hyp, c, blackCol, 1, cost);
        if (next.n >= 0 && count < 16) out[count++] = next;
    }
    static const uint8_t spacesStd[3] = {0, 1, 2};
    static const uint8_t spacesVar[3] = {0, 2, 3};
    const uint8_t* spaces = variant ? spacesVar : spacesStd;
    for (int si = 0; si < 3; ++si) {
        for (uint8_t b1 = 1; b1 <= 2; ++b1) {
            for (uint8_t b2 = 1; b2 <= 2; ++b2) {
                const uint8_t c[3] = {b1, spaces[si], b2};
                const uint8_t col[3] = {1, 0, 1};
                const float predicted = (b1 + spaces[si] + b2) * m;
                const float cost = std::fabs(predicted - width) / m + 0.35f;
                BeamHyp next;
                beamAppend(next, hyp, c, col, 3, cost);
                if (next.n >= 0 && count < 16) out[count++] = next;
            }
        }
    }
    return count > 0;
}

bool beamExpandWhite(const BeamHyp& hyp, float width, float m, bool variant,
                     BeamHyp* out, int& count) {
    count = 0;
    if (width > 5.0f * m) {
        BeamHyp next = hyp;
        next.score += 0.1f;
        out[count++] = next;
        return true;
    }
    static const uint8_t spacesStd[3] = {0, 1, 2};
    static const uint8_t spacesVar[3] = {0, 2, 3};
    const uint8_t* spaces = variant ? spacesVar : spacesStd;
    const uint8_t col[1] = {0};
    for (int si = 0; si < 3; ++si) {
        const float cost = std::fabs(spaces[si] * m - width) / m;
        BeamHyp next;
        const uint8_t c[1] = {spaces[si]};
        beamAppend(next, hyp, c, col, 1, cost);
        if (next.n >= 0 && count < 16) out[count++] = next;
    }
    return count > 0;
}

bool beamDecodeElements(const BeamHyp& hyp, bool variant, ChecksumMode mode,
                        const Options& opts, std::string& out) {
    if (hyp.n < 3) {
        return false;
    }
    if (hyp.colors[0] != 1 || hyp.classes[0] != 2) {
        return false;
    }
    if (hyp.colors[1] != 0 || !beamSpaceOk(hyp.classes[1], variant)) {
        return false;
    }
    int i = 2;
    int bits = 0;
    int bitCount = 0;
    std::string digits;
    while (true) {
        const bool stopPattern = i + 2 < hyp.n && hyp.colors[i] == 1 &&
            hyp.classes[i] == 1 && hyp.colors[i + 1] == 0 &&
            beamSpaceOk(hyp.classes[i + 1], variant) &&
            hyp.colors[i + 2] == 1 && hyp.classes[i + 2] == 1;
        const bool stopPosition = stopPattern &&
            (i + 3 == hyp.n || (i + 3 < hyp.n && hyp.colors[i + 3] == 0));
        if (stopPosition) {
            if (bitCount == 0 && static_cast<int>(digits.size()) >= opts.minDigits &&
                static_cast<int>(digits.size()) <= opts.maxDigits) {
                const ChecksumInfo info = analyzeChecksum(digits, mode);
                if (info.status == ChecksumStatus::Valid) {
                    out = digits;
                    return true;
                }
            }
        }
        if (i + 1 >= hyp.n) {
            return false;
        }
        if (hyp.colors[i] != 1 || hyp.colors[i + 1] != 0) {
            return false;
        }
        if (hyp.classes[i] != 1 && hyp.classes[i] != 2) {
            return false;
        }
        if (!beamSpaceOk(hyp.classes[i + 1], variant)) {
            return false;
        }
        bits = (bits << 1) | (hyp.classes[i] == 2 ? 1 : 0);
        if (++bitCount == 4) {
            if (bits > 9) {
                return false;
            }
            digits.push_back(static_cast<char>('0' + bits));
            bits = 0;
            bitCount = 0;
            if (static_cast<int>(digits.size()) > opts.maxDigits) {
                return false;
            }
        }
        i += 2;
    }
}

bool beamDecodeRuns(const std::vector<Run>& runs, float m, bool variant, ChecksumMode mode,
                    const Options& opts, std::string& outDigits) {
    const int n = static_cast<int>(runs.size());
    if (n < 30) {
        return false;
    }
    bool found = false;
    std::string best;
    float bestScore = 1e9f;
    int starts = 0;
    BeamHyp beam[kBeamWidth];
    BeamHyp next[kBeamWidth * 16];
    for (int seed = 0; seed < n && starts < kBeamMaxStarts; ++seed) {
        if (!runs[seed].black) {
            continue;
        }
        const bool quietBefore = (seed == 0) ||
            (!runs[seed - 1].black && runs[seed - 1].len >= 3.0f * m);
        if (!quietBefore) {
            continue;
        }
        ++starts;
        int beamCount = 0;
        {
            static const uint8_t spacesStd[3] = {0, 1, 2};
            static const uint8_t spacesVar[3] = {0, 2, 3};
            const uint8_t* spaces = variant ? spacesVar : spacesStd;
            BeamHyp seedHyp;
            seedHyp.n = 1;
            seedHyp.classes[0] = 2;
            seedHyp.colors[0] = 1;
            seedHyp.score = std::fabs(2.0f * m - runs[seed].len) / m;
            beam[beamCount++] = seedHyp;
            for (int si = 0; si < 3; ++si) {
                for (uint8_t b2 = 1; b2 <= 2; ++b2) {
                    BeamHyp hyp = seedHyp;
                    hyp.classes[1] = spaces[si];
                    hyp.colors[1] = 0;
                    hyp.classes[2] = b2;
                    hyp.colors[2] = 1;
                    hyp.n = 3;
                    hyp.score += std::fabs((2 + spaces[si] + b2) * m - runs[seed].len) / m + 0.35f;
                    if (beamCount < kBeamWidth) beam[beamCount++] = hyp;
                }
            }
        }
        for (int r = seed + 1; r < n; ++r) {
            int nextCount = 0;
            for (int h = 0; h < beamCount; ++h) {
                BeamHyp expanded[16];
                int expandedCount = 0;
                if (runs[r].black) {
                    if (!beamExpandBlack(beam[h], runs[r].len, m, variant, expanded, expandedCount)) {
                        continue;
                    }
                } else {
                    if (!beamExpandWhite(beam[h], runs[r].len, m, variant, expanded, expandedCount)) {
                        continue;
                    }
                }
                for (int e = 0; e < expandedCount && nextCount < kBeamWidth * 16; ++e) {
                    next[nextCount++] = expanded[e];
                }
            }
            if (nextCount == 0) {
                beamCount = 0;
                break;
            }
            std::sort(next, next + nextCount, [](const BeamHyp& a, const BeamHyp& b) {
                return a.score < b.score;
            });
            if (nextCount > kBeamWidth) {
                nextCount = kBeamWidth;
            }
            for (int i = 0; i < nextCount; ++i) {
                beam[i] = next[i];
            }
            beamCount = nextCount;
        }
        for (int h = 0; h < beamCount; ++h) {
            std::string digits;
            if (beamDecodeElements(beam[h], variant, mode, opts, digits)) {
                if (!found || beam[h].score < bestScore) {
                    found = true;
                    best = digits;
                    bestScore = beam[h].score;
                }
            }
        }
    }
    if (found) {
        outDigits = best;
    }
    return found;
}

int beamModuleCandidates(const std::vector<Run>& runs, float* out, int cap) {
    std::vector<float> spaces;
    std::vector<float> pairs;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (!runs[i].black && runs[i].len < 40.0f && runs[i].len > 0.5f) {
            spaces.push_back(runs[i].len);
        }
        if (runs[i].black && i + 1 < runs.size() && !runs[i + 1].black &&
            runs[i].len + runs[i + 1].len < 40.0f) {
            pairs.push_back(runs[i].len + runs[i + 1].len);
        }
    }
    if (spaces.empty() || pairs.empty()) {
        return 0;
    }
    std::sort(spaces.begin(), spaces.end());
    std::sort(pairs.begin(), pairs.end());
    const float sw = spaces[spaces.size() / 2];
    const float pw = pairs[pairs.size() / 2];
    const float candidates[4] = {sw / 2.0f, sw / 2.6f, pw / 3.0f, pw / 3.6f};
    int count = 0;
    for (float c : candidates) {
        if (c < 0.8f || c > 14.0f) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < count; ++i) {
            if (std::fabs(out[i] - c) < 0.06f * c) {
                dup = true;
            }
        }
        if (!dup && count < cap) {
            out[count++] = c;
        }
    }
    return count;
}

bool betterIsolated(const Result& isolated, const Result& pooled, ChecksumMode mode) {
    if (isolated.found && isolated.votes < 2) {
        return false;
    }
    if (!pooled.found) {
        return isolated.found;
    }
    if (!isolated.found) {
        return false;
    }
    const int isolatedStrength = isolated.votes >= 2
        ? analyzeChecksum(isolated.text, mode).strength : 0;
    const int pooledStrength = pooled.votes >= 2
        ? analyzeChecksum(pooled.text, mode).strength : 0;
    if (isolatedStrength != pooledStrength) {
        return isolatedStrength > pooledStrength;
    }
    return isolated.votes > pooled.votes;
}

Result decodeYPlane(const uint8_t* y, int width, int height, int rowStride,
                    ChecksumMode mode, const Options& options) {
    if (y == nullptr || width < 16 || height < 8 || rowStride < width) {
        return Result{};
    }

    Result result = decodeCore(y, width, height, rowStride, mode, options);
    if (!result.found || result.votes < 3 ||
        analyzeChecksum(result.text, mode).strength < 2) {
        Band bands[kMaxBands];
        const int bandCount = findBands(y, width, height, rowStride, bands, kMaxBands);
        const int attempts = std::min(bandCount, 3);
        for (int bi = 0; bi < attempts; ++bi) {
            const int margin = std::max(4, (bands[bi].y1 - bands[bi].y0) / 6);
            const int y0 = std::max(0, bands[bi].y0 - margin);
            const int y1 = std::min(height, bands[bi].y1 + margin);
            const int cropHeight = y1 - y0;
            if (cropHeight < 12) {
                continue;
            }
            std::vector<uint8_t> crop(static_cast<size_t>(width) * cropHeight);
            for (int row = 0; row < cropHeight; ++row) {
                std::memcpy(crop.data() + static_cast<size_t>(row) * width,
                            y + static_cast<size_t>(y0 + row) * rowStride, width);
            }
            const Result isolated = decodeCore(crop.data(), width, cropHeight, width,
                                               mode, options);
            if (betterIsolated(isolated, result, mode)) {
                result = isolated;
            }
            if (result.found && result.votes >= 3 &&
                analyzeChecksum(result.text, mode).strength >= 2) {
                break;
            }
        }
    }
    if (result.found && result.votes >= 3) {
        return result;
    }

    if (options.upscaleRetry && std::max(width, height) < kUpscaleMaxLongEdge) {
        const int w2 = width * 2;
        const int h2 = height * 2;
        std::vector<uint8_t> up(static_cast<size_t>(w2) * h2);
        for (int row = 0; row < h2; ++row) {
            const uint8_t* src = y + static_cast<size_t>(row / 2) * rowStride;
            uint8_t* dst = up.data() + static_cast<size_t>(row) * w2;
            for (int x = 0; x < w2; ++x) {
                dst[x] = src[x / 2];
            }
        }
        if (g_debug) {
            std::fprintf(stderr, "[msi] upscale retry %dx%d\n", w2, h2);
        }
        const Result retry = decodeCore(up.data(), w2, h2, w2, mode, options);
        if (retry.found && betterIsolated(retry, result, mode)) {
            result = retry;
        }
        if (result.found) {
            return result;
        }
    }

    if (options.contrastRetry) {
        uint8_t mn = 255;
        uint8_t mx = 0;
        for (int row = 0; row < height; row += 4) {
            const uint8_t* p = y + static_cast<size_t>(row) * rowStride;
            for (int x = 0; x < width; x += 4) {
                mn = std::min(mn, p[x]);
                mx = std::max(mx, p[x]);
            }
        }
        if (mx > mn && mx - mn < static_cast<int>(kStretchMaxRange)) {
            std::vector<uint8_t> stretched(static_cast<size_t>(width) * height);
            const float scale = 255.0f / static_cast<float>(mx - mn);
            for (int row = 0; row < height; ++row) {
                const uint8_t* src = y + static_cast<size_t>(row) * rowStride;
                uint8_t* dst = stretched.data() + static_cast<size_t>(row) * width;
                for (int x = 0; x < width; ++x) {
                    int v = static_cast<int>((src[x] - mn) * scale);
                    if (v < 0) v = 0;
                    if (v > 255) v = 255;
                    dst[x] = static_cast<uint8_t>(v);
                }
            }
            if (g_debug) {
                std::fprintf(stderr, "[msi] contrast retry range=%d\n", mx - mn);
            }
            const Result retry = decodeCore(stretched.data(), width, height, width, mode, options);
            if (retry.found && betterIsolated(retry, result, mode)) {
                result = retry;
            }
        }
    }

    if (!result.found && options.beamRetry) {
        std::vector<std::pair<std::string, int>> beamVotes;
        LineScanner beamScanner;
        std::vector<float> rescueProfile;
        int rescueTemplateAttempts = 0;
        {
            const int step = std::max(5, height / 24);
            int rows = 0;
            for (int yy = step / 2; yy < height - 1 && rows < 24; yy += step, ++rows) {
                beamScanner.extract(y + static_cast<size_t>(yy) * rowStride, width, 1);
                const std::vector<Run>& lineRuns = beamScanner.runs();
                if (g_debug) {
                    std::fprintf(stderr, "[msi] beam row=%d runs=%zu\n", yy, lineRuns.size());
                }
                if (rescueTemplateAttempts < 20 && lineRuns.size() >= 8) {
                    ++rescueTemplateAttempts;
                    float rescueScore = 0.0f;
                    float rescueModule = 0.0f;
                    std::string rescueDigits;
                    extractProfile(y + static_cast<size_t>(yy) * rowStride, width, 1,
                                   rescueProfile);
                    if (decodeProfileByTemplates(rescueProfile.data(),
                                                 static_cast<int>(rescueProfile.size()),
                                                 options, mode, rescueDigits,
                                                 rescueModule, rescueScore) &&
                        !isUniform(rescueDigits)) {
                        bool merged = false;
                        for (auto& vote : beamVotes) {
                            if (vote.first == rescueDigits) {
                                vote.second += 1;
                                merged = true;
                                break;
                            }
                        }
                        if (!merged) {
                            beamVotes.push_back({rescueDigits, 1});
                        }
                    }
                }
                if (lineRuns.size() < 30) {
                    continue;
                }
                float moduleCandidates[4];
                const int moduleCount = beamModuleCandidates(lineRuns, moduleCandidates, 4);
                if (g_debug) {
                    for (int q = 0; q < moduleCount; ++q) {
                        std::fprintf(stderr, "[msi]   m=%.2f\n", moduleCandidates[q]);
                    }
                }
                for (int mi = 0; mi < moduleCount; ++mi) {
                    for (int variant = 0; variant < 2; ++variant) {
                        std::string digits;
                        if (beamDecodeRuns(lineRuns, moduleCandidates[mi], variant == 1,
                                           mode, options, digits)) {
                            if (isUniform(digits)) {
                                continue;
                            }
                            if (g_debug) {
                                std::fprintf(stderr, "[msi]   beam hit %s\n", digits.c_str());
                            }
                            bool merged = false;
                            for (auto& vote : beamVotes) {
                                if (vote.first == digits) {
                                    vote.second += 1;
                                    merged = true;
                                    break;
                                }
                            }
                            if (!merged) {
                                beamVotes.push_back({digits, 1});
                            }
                        }
                    }
                }
            }
        }
        std::string bestText;
        int bestVotes = 0;
        for (const auto& vote : beamVotes) {
            const ChecksumInfo info = analyzeChecksum(vote.first, mode);
            if (info.status != ChecksumStatus::Valid) {
                continue;
            }
            const bool acceptable = vote.second >= 2;
            if (acceptable && (vote.second > bestVotes ||
                (vote.second == bestVotes && vote.first.size() > bestText.size()))) {
                bestText = vote.first;
                bestVotes = vote.second;
            }
        }
        if (!bestText.empty()) {
            Result beamResult;
            beamResult.found = true;
            beamResult.text = bestText;
            beamResult.votes = bestVotes;
            beamResult.checksum = analyzeChecksum(bestText, mode).status;
            beamResult.scheme = analyzeChecksum(bestText, mode).scheme;
            if (g_debug) {
                std::fprintf(stderr, "[msi] beam winner=%s votes=%d\n",
                             bestText.c_str(), bestVotes);
            }
            result = beamResult;
        }
    }

    return result;
}

}
