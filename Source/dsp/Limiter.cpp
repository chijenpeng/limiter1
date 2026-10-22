// Look-ahead brickwall peak limiter. The public look-ahead design (Giannoulis, Massberg and
// Reiss 2012):
//   1. the audio is delayed by the look-ahead (1.5 ms);
//   2. on the undelayed signal, the gain each sample needs to stay under the ceiling;
//   3. its running minimum over the look-ahead window (peak hold, monotonic deque);
//   4. an exponential release in dB; Auto lengthens it while limiting is sustained;
//   5. two cascaded moving averages, together as long as the look-ahead, so the gain glides
//      down and arrives exactly when the peak does: no overshoot and no gain steps.
// The threshold is a gain before the detector and the delay alike, with limiting to 1.0 from
// there on, and the ceiling a gain after the limiter; both glide, and a ceiling that is being
// lowered is honoured immediately through the detector so no sample ever exceeds it.
// True Peak reads the peaks between samples on an 8x interpolated copy of the detector path
// with parabolic refinement; Analog adds a fixed margin on top of that reading.

#include "Limiter.h"

#include <algorithm>
#include <cmath>

namespace limiter1
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kParamSeconds = 0.02;      // input, threshold and ceiling gain glide
constexpr double kSustainSeconds = 0.3;     // Auto release: averaging time of the demanded reduction
constexpr double kAutoMsPerDb = 20.0;       // Auto release: each dB of sustained reduction adds this much release
constexpr double kAutoCapMs = 500.0;        // Auto release: never longer than this (or the setting, if longer)
constexpr double kAnalogMarginDb = 1.5;     // Analog domain: margin over the true-peak reading (worst measured need: 1.16 dB)

double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
} // namespace

double Limiter::analogMarginDb() noexcept { return kAnalogMarginDb; }

//==============================================================================
void Limiter::SlidingMin::prepare (int windowLength)
{
    window = windowLength;
    int size = 1;
    while (size < window + 1)
        size *= 2;
    mask = (uint32_t) size - 1;
    value.assign ((size_t) size, 1.0);
    when.assign ((size_t) size, 0);
    reset();
}

void Limiter::SlidingMin::reset() noexcept { head = tail = 0; now = 0; }

double Limiter::SlidingMin::push (double v) noexcept
{
    while (tail != head && value[(tail - 1) & mask] >= v)
        --tail;
    value[tail & mask] = v;
    when[tail & mask] = now;
    ++tail;
    while (when[head & mask] <= now - window)
        ++head;
    ++now;
    return value[head & mask];
}

void Limiter::MovingAverage::prepare (int length)
{
    ring.assign ((size_t) length, 1.0);
    reset();
}

void Limiter::MovingAverage::reset() noexcept
{
    std::fill (ring.begin(), ring.end(), 1.0);
    sum = (double) ring.size();
    pos = 0;
}

double Limiter::MovingAverage::push (double v) noexcept
{
    sum += v - ring[pos];
    ring[pos] = v;
    if (++pos == ring.size())
    {
        // Re-sum once per lap so rounding cannot drift.
        pos = 0;
        sum = 0.0;
        for (double x : ring)
            sum += x;
    }
    return sum / (double) ring.size();
}

//==============================================================================
void Limiter::prepare (double sampleRate, int)
{
    fs = sampleRate;
    // Two averages of length M need a hold of 2M - 1 samples and a delay of 2M - 2; the hold
    // is one longer so the sample after a peak keeps its gain too (a true peak lies between
    // two samples).
    const int m = std::max (2, (int) std::lround (0.5 * kLookAheadSeconds * fs) + 1);
    latency = kTpTaps / 2 + 2 * m - 2;
    hold.prepare (2 * m);
    smooth1.prepare (m);
    smooth2.prepare (m);
    int size = 1;
    while (size < latency + 1)
        size *= 2;
    delayMask = size - 1;
    for (auto& d : delay)
        d.assign ((size_t) size, 0.0);

    paramGlide = 1.0 - std::exp (-1.0 / (kParamSeconds * fs));
    sustainCoef = 1.0 - std::exp (-1.0 / (kSustainSeconds * fs));
    autoCapSamples = kAutoCapMs * 1e-3 * fs;
    autoSamplesPerDb = kAutoMsPerDb * 1e-3 * fs;
    designInterpolator();
    requantizer.prepare (fs);
    setParams (current);
    reset();
}

void Limiter::reset()
{
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0);
    for (auto& h : history)
        std::fill (std::begin (h), std::end (h), 0.0);
    for (auto& p : tpPrevious)
        std::fill (std::begin (p), std::end (p), 0.0);
    historyPos = delayPos = 0;
    hold.reset();
    smooth1.reset();
    smooth2.reset();
    envDb = sustainDb = segmentPeak = 0.0;
    updateTargets();
    gainIn[0] = gainInTarget[0];
    gainIn[1] = gainInTarget[1];
    preGain = preGainTarget;
    postGain = postGainTarget;
    requantizer.reset();
    meterValues = Meters();
}

void Limiter::setParams (const Params& p) noexcept
{
    current = p;
    current.inputGainDbL = std::clamp (p.inputGainDbL, kMinGainDb, 0.0f);
    current.inputGainDbR = std::clamp (p.inputGainDbR, kMinGainDb, 0.0f);
    current.thresholdDb = std::clamp (p.thresholdDb, kMinGainDb, 0.0f);
    current.ceilingDb = std::clamp (p.ceilingDb, kMinGainDb, 0.0f);
    current.releaseMs = std::clamp (p.releaseMs, 0.01f, 1000.0f);
    updateTargets();
    requantizer.set (current.quantize, current.dither, current.shaping);
}

void Limiter::updateTargets() noexcept
{
    gainInTarget[0] = dbToGain (current.inputGainDbL);
    gainInTarget[1] = dbToGain (current.inputGainDbR);
    preGainTarget = dbToGain (-current.thresholdDb);
    postGainTarget = dbToGain (current.ceilingDb);
    releaseTauSamples = std::max (1e-3, (double) current.releaseMs * 1e-3 * fs);
    releaseCoef = std::exp (-1.0 / releaseTauSamples);
}

/** Kaiser-windowed sinc for the seven points between samples; the samples themselves are the
    eighth phase. 32 taps with beta 5 ripple by 0.04 dB up to 0.45 fs; each phase is scaled so
    it never reads low in that band, only up to the ripple high (0.07 dB at most). Above 0.45 fs
    the peak between samples depends on the reconstruction filter, so no detector can match
    every meter there: a sharp 256-tap reference rings up to 5 dB over the sample peak on
    Nyquist-frequency bursts, which this reads 1-2 dB low. The Analog domain's margin covers
    the cases in the test; wider bands or a lower beta were tried and read the passband high
    instead (beta 4 at 0.47 fs: +0.9 dB on a plain 3 kHz tone), which is worse. */
void Limiter::designInterpolator()
{
    auto besselI0 = [] (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    constexpr double beta = 5.0, band = 0.45;
    for (int phase = 0; phase < kTpOversample - 1; ++phase)
    {
        const double frac = (phase + 1) / (double) kTpOversample;
        auto& h = interpolator[phase];
        for (int j = 0; j < kTpTaps; ++j)
        {
            const double t = (j - kTpCentre) - frac;   // distance from the point to tap j
            const double u = t / (kTpTaps * 0.5);
            h[j] = std::sin (kPi * t) / (kPi * t) * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - u * u))) / besselI0 (beta);
        }
        double lowest = 1e9;
        for (int k = 0; k <= 90; ++k)
        {
            const double w = 2.0 * kPi * band * k / 90.0;
            double re = 0.0, im = 0.0;
            for (int j = 0; j < kTpTaps; ++j)
            {
                re += h[j] * std::cos (w * j);
                im += h[j] * std::sin (w * j);
            }
            lowest = std::min (lowest, std::sqrt (re * re + im * im));
        }
        for (double& c : h)
            c /= lowest;
    }
}

/** The highest value the channel's waveform reaches between the detector's centre sample and
    the next one, from the oversampled points with a parabola through each local maximum. */
double Limiter::detectPeak (int channel) noexcept
{
    const double* w = history[channel] + historyPos + 1;   // the last kTpTaps samples, oldest first
    double cur[kTpOversample];
    cur[0] = w[kTpCentre];
    for (int phase = 1; phase < kTpOversample; ++phase)
    {
        const auto& h = interpolator[phase - 1];
        double v = 0.0;
        for (int j = 0; j < kTpTaps; ++j)
            v += w[j] * h[j];
        cur[phase] = v;
    }
    double* prev = tpPrevious[channel];
    const double next = w[kTpCentre + 1];
    double peak = 0.0;
    for (int k = 0; k < kTpOversample; ++k)
    {
        const double y1 = cur[k];
        const double a1 = std::abs (y1);
        peak = std::max (peak, a1);
        const double y0 = k == 0 ? prev[kTpOversample - 1] : cur[k - 1];
        const double y2 = k == kTpOversample - 1 ? next : cur[k + 1];
        if (a1 >= std::abs (y0) && a1 >= std::abs (y2))
        {
            const double denominator = y0 - 2.0 * y1 + y2;
            if (std::abs (denominator) > 1e-12)
            {
                const double p = 0.5 * (y0 - y2) / denominator;
                if (std::abs (p) <= 0.5)
                    peak = std::max (peak, std::abs (y1 - 0.25 * (y0 - y2) * p));
            }
        }
    }
    for (int k = 0; k < kTpOversample; ++k)
        prev[k] = cur[k];
    return peak;
}

template <int numChannels>
void Limiter::run (float* const* io, int numSamples) noexcept
{
    const bool interSample = current.domain != Domain::Digital;
    const double margin = current.domain == Domain::Analog ? dbToGain (kAnalogMarginDb) : 1.0;
    const bool autoRelease = current.autoRelease;
    double lowest = 1.0;
    double inPeak[2] = { 0.0, 0.0 }, outPeak[2] = { 0.0, 0.0 };

    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < numChannels; ++c)
            gainIn[c] += (gainInTarget[c] - gainIn[c]) * paramGlide;
        preGain += (preGainTarget - preGain) * paramGlide;
        postGain += (postGainTarget - postGain) * paramGlide;

        // Threshold gain before both the detector and the delay, so the limiter always sees
        // exactly what it will later scale; limiting is to `limit` from here on.
        double detected = 0.0;
        for (int c = 0; c < numChannels; ++c)
        {
            const double in = io[c][i] * gainIn[c];
            inPeak[c] = std::max (inPeak[c], std::abs (in));
            const double u = in * preGain;
            history[c][historyPos] = history[c][historyPos + kTpTaps] = u;
            delay[c][(size_t) delayPos] = u;
            const double peak = interSample ? detectPeak (c) * margin : std::abs (history[c][historyPos + 1 + kTpCentre]);
            detected = std::max (detected, peak);
        }
        historyPos = (historyPos + 1) % kTpTaps;
        // The interval before this sample counts too: an inter-sample peak needs the gain
        // down on both sides of it.
        const double previousSegment = segmentPeak;
        segmentPeak = detected;
        if (interSample)
            detected = std::max (detected, previousSegment);

        // A ceiling on its way down is honoured at once: the limiter aims below 1.0 by the
        // part of the move the post gain has not yet made.
        const double limit = std::min (1.0, postGainTarget / postGain);
        const double held = hold.push (detected > limit ? limit / detected : 1.0);
        const double holdDb = held < 1.0 ? -20.0 * std::log10 (held) : 0.0;
        if (holdDb >= envDb)
            envDb = holdDb;
        else
        {
            double coef = releaseCoef;
            if (autoRelease)
            {
                const double tau = std::min (releaseTauSamples + autoSamplesPerDb * sustainDb, std::max (releaseTauSamples, autoCapSamples));
                coef = std::exp (-1.0 / tau);
            }
            envDb = holdDb + (envDb - holdDb) * coef;
            if (envDb < 1e-6)
                envDb = 0.0;
        }
        // The reduction the signal demands, not the release tail: an isolated peak barely
        // moves it, dense material that keeps hitting the ceiling does.
        sustainDb += (holdDb - sustainDb) * sustainCoef;
        if (sustainDb < 1e-9)
            sustainDb = 0.0;

        const double gain = smooth2.push (smooth1.push (envDb > 0.0 ? dbToGain (-envDb) : 1.0));
        lowest = std::min (lowest, gain);
        const int read = (delayPos - latency) & delayMask;
        for (int c = 0; c < numChannels; ++c)
        {
            const double y = requantizer.process (c, delay[c][(size_t) read] * gain * postGain);
            outPeak[c] = std::max (outPeak[c], std::abs (y));
            io[c][i] = (float) y;
        }
        delayPos = (delayPos + 1) & delayMask;
    }

    // Gains that have arrived stop gliding, so nothing decays into denormal territory.
    for (int c = 0; c < numChannels; ++c)
        if (std::abs (gainIn[c] - gainInTarget[c]) < 1e-12)
            gainIn[c] = gainInTarget[c];
    if (std::abs (preGain - preGainTarget) < 1e-12)
        preGain = preGainTarget;
    if (std::abs (postGain - postGainTarget) < 1e-12)
        postGain = postGainTarget;

    for (int c = 0; c < 2; ++c)
    {
        meterValues.inputPeak[c] = (float) inPeak[c];
        meterValues.outputPeak[c] = (float) outPeak[c];
    }
    meterValues.gainReductionDb = (float) (-20.0 * std::log10 (std::max (lowest, 1e-9)));
}

void Limiter::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (numChannels >= 2)
        run<2> (channels, numSamples);
    else
        run<1> (channels, numSamples);
}
} // namespace limiter1
