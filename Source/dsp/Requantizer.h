// Word-length reduction with dither and error-feedback noise shaping, last in the chain.
//
//     w = x - sum_k h[k] e[n-k]      error feedback (the shaper taps, see ShaperCoefficients.h)
//     y = Q(w + d)                   rounding to the target word length, saturated at full scale
//     e = y - w                      the total error, dither included, is what gets shaped
//
// Dither Type 1 is TPDF, 2 LSB peak to peak (two independent uniform +-0.5 LSB sources);
// Type 2 is RPDF, 1 LSB peak to peak, and is faded out after 100 ms of exact digital silence
// at the input so that silence stays silent. Each channel has its own generator and seed, so
// the output is bit-identical from run to run.
#pragma once

#include <cstdint>

namespace limiter1
{
enum class Quantize { None = 0, Bits8 = 1, Bits12 = 2, Bits16 = 3, Bits20 = 4, Bits24 = 5 };
enum class Dither { None = 0, Type1 = 1, Type2 = 2 };
enum class Shaping { None = 0, Moderate = 1, Normal = 2, Ultra = 3 };

/** Word length in bits for a Quantize setting, 0 for None. */
int quantizeBits (Quantize q) noexcept;

class Requantizer
{
public:
    static constexpr int kMaxTaps = 9;
    static constexpr double kSilenceGateSeconds = 0.1;   // Type 2 fades out after this much silence
    static constexpr double kSilenceRampSeconds = 0.01;  // ...over this long

    void prepare (double sampleRate);
    void reset() noexcept;
    void set (Quantize quantize, Dither dither, Shaping shaping) noexcept;

    /** One sample of one channel (0 or 1). Returns x untouched when Quantize is None. */
    double process (int channel, double x) noexcept;

    int numTaps() const noexcept { return taps; }
    const double* tapsFor() const noexcept { return h; }
    /** Which coefficient family the current sample rate uses: 0 below 44.1 kHz, 1, 2, 3 (see header). */
    int family() const noexcept { return currentFamily; }

private:
    struct Channel
    {
        double error[kMaxTaps] {};   // the last errors, newest first
        uint64_t rng = 0;
        int64_t silentSamples = 0;
        double ditherGain = 1.0;
    };

    static uint64_t seedFor (int channel) noexcept;
    static double uniform (uint64_t& state) noexcept;   // [-0.5, 0.5)

    Channel channels[2];
    double h[kMaxTaps] {};
    int taps = 0;
    int bits = 0;
    double scale = 1.0, maxCode = 0.0, minCode = 0.0;
    Quantize quantizeType = Quantize::None;
    Dither ditherType = Dither::None;
    Shaping shapingType = Shaping::None;
    int currentFamily = 1;
    int64_t gateSamples = 0;
    double rampStep = 0.0;
};
} // namespace limiter1
