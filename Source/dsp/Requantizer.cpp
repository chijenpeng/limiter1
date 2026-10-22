#include "Requantizer.h"
#include "ShaperCoefficients.h"

#include <algorithm>
#include <cmath>

namespace limiter1
{
int quantizeBits (Quantize q) noexcept
{
    switch (q)
    {
        case Quantize::Bits8: return 8;
        case Quantize::Bits12: return 12;
        case Quantize::Bits16: return 16;
        case Quantize::Bits20: return 20;
        case Quantize::Bits24: return 24;
        case Quantize::None: break;
    }
    return 0;
}

uint64_t Requantizer::seedFor (int channel) noexcept
{
    return channel == 0 ? 0x9E3779B97F4A7C15ull : 0xD1B54A32D192ED03ull;
}

double Requantizer::uniform (uint64_t& state) noexcept
{
    // xorshift64*: fast, decent, and the same on every platform.
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const uint64_t bits = (state * 0x2545F4914F6CDD1Dull) >> 11;
    return (double) bits * (1.0 / 9007199254740992.0) - 0.5;
}

void Requantizer::prepare (double sampleRate)
{
    currentFamily = sampleRate < 44000.0 ? 0 : sampleRate < 88000.0 ? 1 : sampleRate < 176000.0 ? 2 : 3;
    gateSamples = (int64_t) std::llround (kSilenceGateSeconds * sampleRate);
    rampStep = 1.0 / std::max (1.0, kSilenceRampSeconds * sampleRate);
    set (quantizeType, ditherType, shapingType);   // the shaper taps depend on the family
    reset();
}

void Requantizer::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        channels[c] = Channel();
        channels[c].rng = seedFor (c);
    }
}

void Requantizer::set (Quantize quantize, Dither dither, Shaping shaping) noexcept
{
    quantizeType = quantize;
    bits = quantizeBits (quantize);
    scale = bits > 0 ? std::ldexp (1.0, bits - 1) : 1.0;
    maxCode = scale - 1.0;
    minCode = -scale;
    ditherType = dither;
    shapingType = shaping;
    taps = 0;
    if (shaping != Shaping::None)
    {
        const ShaperTaps& t = kShaperTaps[currentFamily][(int) shaping - 1];
        taps = std::min (t.numTaps, kMaxTaps);
        for (int k = 0; k < taps; ++k)
            h[k] = t.h[k];
    }
}

double Requantizer::process (int channel, double x) noexcept
{
    if (bits == 0)
        return x;

    Channel& c = channels[channel];
    if (x == 0.0)
        ++c.silentSamples;
    else
        c.silentSamples = 0;

    double feedback = 0.0;
    for (int k = 0; k < taps; ++k)
        feedback += h[k] * c.error[k];
    const double w = x * scale - feedback;

    double d = 0.0;
    if (ditherType == Dither::Type1)
        d = uniform (c.rng) + uniform (c.rng);
    else if (ditherType == Dither::Type2)
    {
        const double target = c.silentSamples >= gateSamples ? 0.0 : 1.0;
        c.ditherGain += std::clamp (target - c.ditherGain, -rampStep, rampStep);
        d = uniform (c.rng) * c.ditherGain;
    }

    double y = std::floor (w + d + 0.5);
    const double e = y - w;
    for (int k = kMaxTaps - 1; k > 0; --k)
        c.error[k] = c.error[k - 1];
    c.error[0] = e;

    // Long after the input went silent, let the error state die away so that the output is
    // exact digital zero (Type 2) and nothing lingers at denormal level.
    if (c.silentSamples > gateSamples + (int64_t) (1.0 / rampStep) + 1)
        for (double& v : c.error)
            v = std::abs (v) < 1e-9 ? 0.0 : v * 0.9;

    y = std::clamp (y, minCode, maxCode);
    return y / scale;
}
} // namespace limiter1
