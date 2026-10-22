// Look-ahead brickwall peak limiter with input faders, three peak-detection domains, a
// program-dependent release and an IDR-style re-quantizer on its output. No JUCE dependency;
// double precision inside, float in and out. See docs/dsp.md for the algorithm and the numbers.
#pragma once

#include "Requantizer.h"

#include <cstdint>
#include <vector>

namespace limiter1
{
enum class Domain { Digital = 0, Analog = 1, TruePeak = 2 };

struct Params
{
    float inputGainDbL = 0.0f, inputGainDbR = 0.0f;   // -30..0 dB input faders (mono uses L)
    float thresholdDb = 0.0f;                         // -30..0 dB: peaks above this are limited
    float ceilingDb = 0.0f;                           // -30..0 dB: the highest output level
    float releaseMs = 100.0f;                         // 0.01..1000 ms, time constant of the dB envelope
    bool autoRelease = false;                         // program-dependent release
    Domain domain = Domain::Digital;
    Quantize quantize = Quantize::Bits24;
    Dither dither = Dither::Type1;
    Shaping shaping = Shaping::Normal;
};

/** Block maxima, published after every process() call. */
struct Meters
{
    float inputPeak[2] { 0.0f, 0.0f };    // after the input faders, linear
    float outputPeak[2] { 0.0f, 0.0f };   // final output, linear
    float gainReductionDb = 0.0f;         // deepest limiter reduction in the block, >= 0
};

class Limiter
{
public:
    static constexpr double kLookAheadSeconds = 0.0015;
    static constexpr float kMinGainDb = -30.0f;

    /** Allocates everything; call before process() and whenever the sample rate changes. */
    void prepare (double sampleRate, int maxBlockSize);
    /** Clears all state, including the dither generators (output is deterministic from here). */
    void reset();

    void setParams (const Params& p) noexcept;
    const Params& params() const noexcept { return current; }

    /** In place; numChannels is 1 or 2. Realtime-safe: no allocations or locks. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Constant for every setting and domain at a given sample rate. */
    int latencySamples() const noexcept { return latency; }
    double sampleRate() const noexcept { return fs; }
    const Meters& meters() const noexcept { return meterValues; }

    /** The Analog domain's safety margin on top of the true-peak reading, in dB (see Limiter.cpp). */
    static double analogMarginDb() noexcept;

private:
    static constexpr int kTpTaps = 32;                 // per interpolation phase
    static constexpr int kTpOversample = 8;            // phases per sample interval
    static constexpr int kTpCentre = kTpTaps / 2 - 1;  // the newest sample the detector interpolates around
    static constexpr int kTpHistory = 2 * kTpTaps;

    class SlidingMin
    {
    public:
        void prepare (int windowLength);
        void reset() noexcept;
        double push (double v) noexcept;

    private:
        std::vector<double> value;
        std::vector<int64_t> when;
        int64_t now = 0;
        uint32_t head = 0, tail = 0, mask = 0;
        int window = 1;
    };

    class MovingAverage
    {
    public:
        void prepare (int length);
        void reset() noexcept;
        double push (double v) noexcept;

    private:
        std::vector<double> ring;
        size_t pos = 0;
        double sum = 0.0;
    };

    void designInterpolator();
    void updateTargets() noexcept;
    double detectPeak (int channel) noexcept;
    template <int numChannels>
    void run (float* const* io, int numSamples) noexcept;

    Params current;
    double fs = 48000.0;
    int latency = 0, delayMask = 0, delayPos = 0, historyPos = 0;
    std::vector<double> delay[2];
    double history[2][kTpHistory] {};
    double interpolator[kTpOversample - 1][kTpTaps] {};
    double tpPrevious[2][kTpOversample] {};   // the previous sample interval's oversampled values, per channel
    SlidingMin hold;
    MovingAverage smooth1, smooth2;
    Requantizer requantizer;

    double gainInTarget[2] { 1.0, 1.0 }, gainIn[2] { 1.0, 1.0 };   // input faders
    double preGainTarget = 1.0, preGain = 1.0;                     // -threshold
    double postGainTarget = 1.0, postGain = 1.0;                   // ceiling
    double paramGlide = 0.0;
    double releaseTauSamples = 1.0, releaseCoef = 0.0, sustainCoef = 0.0;
    double autoCapSamples = 0.0, autoSamplesPerDb = 0.0;
    double envDb = 0.0, sustainDb = 0.0, segmentPeak = 0.0;
    Meters meterValues;
};
} // namespace limiter1
