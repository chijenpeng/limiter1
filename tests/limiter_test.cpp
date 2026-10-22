// Offline checks of the limiter and re-quantizer. Prints PASS/FAIL per check and returns
// non-zero when anything failed. No JUCE; every measurement tool is in this file.

#include "dsp/Limiter.h"
#include "dsp/LinkedControls.h"
#include "dsp/ShaperCoefficients.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace limiter1;

namespace
{
constexpr double kPi = 3.14159265358979323846;
const double kRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
int failures = 0;

std::string fmt (const char* f, ...)
{
    char buffer[1024];
    va_list args;
    va_start (args, f);
    std::vsnprintf (buffer, sizeof (buffer), f, args);
    va_end (args);
    return buffer;
}

void check (bool ok, const std::string& what)
{
    std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

double db (double linear) { return 20.0 * std::log10 (std::max (linear, 1e-30)); }
double fromDb (double d) { return std::pow (10.0, d / 20.0); }

double peakOf (const std::vector<float>& x, size_t from = 0)
{
    double p = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        p = std::max (p, (double) std::abs (x[i]));
    return p;
}

Params baseParams()
{
    Params p;
    p.quantize = Quantize::None;
    p.dither = Dither::None;
    p.shaping = Shaping::None;
    return p;
}

Limiter makeLimiter (double fs, const Params& p, int block = 512)
{
    Limiter l;
    l.setParams (p);
    l.prepare (fs, block);
    return l;
}

using BlockCallback = std::function<void (size_t)>;

void runMono (Limiter& l, std::vector<float>& x, int block, const BlockCallback& before = {})
{
    for (size_t start = 0; start < x.size(); start += (size_t) block)
    {
        if (before)
            before (start);
        float* ch[1] = { x.data() + start };
        l.process (ch, 1, (int) std::min ((size_t) block, x.size() - start));
    }
}

void runStereo (Limiter& l, std::vector<float>& a, std::vector<float>& b, int block, const BlockCallback& before = {})
{
    for (size_t start = 0; start < a.size(); start += (size_t) block)
    {
        if (before)
            before (start);
        float* ch[2] = { a.data() + start, b.data() + start };
        l.process (ch, 2, (int) std::min ((size_t) block, a.size() - start));
    }
}

double besselI0 (double v)
{
    double s = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= (v / (2.0 * k)) * (v / (2.0 * k));
        s += term;
    }
    return s;
}

/** Reference reconstruction: 16x, 256-tap Kaiser (beta 9) windowed sinc per phase, flat to
    within 0.001 dB up to 0.47 fs. The peak of the continuous waveform as a long DAC filter
    would reproduce it. Every sample interval is evaluated when `everywhere`, otherwise only
    next to samples above a quarter of the sample peak (where a true peak can be). */
double referencePeak (const std::vector<float>& x, size_t from, bool everywhere = true)
{
    static constexpr int phases = 16, half = 128;
    static const auto taps = []
    {
        std::vector<std::vector<double>> t ((size_t) phases - 1, std::vector<double> (2 * half));
        for (int p = 0; p < phases - 1; ++p)
            for (int j = 0; j < 2 * half; ++j)
            {
                const double d = (j - (half - 1)) - (p + 1) / (double) phases, u = d / half;
                t[(size_t) p][(size_t) j] = std::sin (kPi * d) / (kPi * d) * besselI0 (9.0 * std::sqrt (std::max (0.0, 1.0 - u * u))) / besselI0 (9.0);
            }
        return t;
    }();
    const double samplePeak = peakOf (x, from);
    double peak = samplePeak;
    for (size_t n = std::max<size_t> (from, half - 1); n + half < x.size(); ++n)
    {
        if (! everywhere && std::max (std::abs (x[n]), std::abs (x[n + 1])) < 0.25 * samplePeak)
            continue;
        for (const auto& phase : taps)
        {
            double v = 0.0;
            const float* w = x.data() + n - (half - 1);
            for (int j = 0; j < 2 * half; ++j)
                v += w[j] * phase[(size_t) j];
            peak = std::max (peak, std::abs (v));
        }
    }
    return peak;
}

void fadeIn (std::vector<float>& x, double fs)
{
    const size_t n = std::min (x.size(), (size_t) (0.01 * fs));
    for (size_t i = 0; i < n; ++i)
        x[i] *= (float) (0.5 - 0.5 * std::cos (kPi * (double) i / (double) n));
}

struct Signal
{
    std::string name;
    std::vector<float> data;
    bool bandLimited;
};

/** Signals 20 dB over full scale. */
std::vector<Signal> hotSignals (double fs)
{
    std::vector<Signal> signals;
    const size_t length = (size_t) (0.25 * fs);
    std::mt19937 rng (3);

    std::vector<float> square (length);
    for (size_t i = 0; i < length; ++i)
        square[i] = std::fmod ((double) i * 100.0 / fs, 1.0) < 0.5 ? 10.0f : -10.0f;
    signals.push_back ({ "square 100 Hz", square, false });

    std::vector<float> impulses (length, 0.0f);
    for (size_t i = 500; i + 1 < length; i += 200 + rng() % 3000)
    {
        impulses[i] = (rng() & 1) ? 10.0f : -10.0f;
        if (rng() % 3 == 0)
            impulses[i + 1] = -impulses[i];   // alternating-sign doublets
    }
    signals.push_back ({ "impulses", impulses, false });

    std::vector<float> bursts (length, 0.0f);
    for (size_t i = 400; i + 40 < length; i += 1000 + rng() % 2000)
        for (size_t k = 0; k < 8 + rng() % 24; ++k)
            bursts[i + k] = (k & 1) ? 10.0f : -10.0f;
    signals.push_back ({ "alternating-sign bursts", bursts, false });

    std::vector<float> tone (length);
    for (size_t i = 0; i < length; ++i)
        tone[i] = (float) (10.0 * std::sin (2.0 * kPi * 0.45 * (double) i + 0.3));
    fadeIn (tone, fs);
    signals.push_back ({ "0.45 fs tone", tone, true });

    std::vector<float> steps (length, 0.0f);
    for (size_t i = 0; i < length; ++i)
        steps[i] = (i / (length / 8)) % 2 ? 10.0f : (i / (length / 16)) % 2 ? -10.0f : 0.0f;
    signals.push_back ({ "DC steps", steps, false });

    std::uniform_real_distribution<double> uniform (-1.0, 1.0);
    std::vector<float> white (length);
    for (auto& v : white)
        v = (float) (10.0 * uniform (rng));
    signals.push_back ({ "white noise", white, false });

    // Noise through a 511-tap windowed-sinc low-pass at 0.44 fs: the band-limited case where
    // a true peak is well defined.
    std::vector<double> raw (length + 511), taps (511);
    for (auto& v : raw)
        v = uniform (rng);
    for (int j = 0; j < 511; ++j)
    {
        const double t = j - 255.0, u = t / 256.0, window = 0.42 + 0.5 * std::cos (kPi * u) + 0.08 * std::cos (2.0 * kPi * u);
        taps[(size_t) j] = (t == 0.0 ? 0.88 : std::sin (0.88 * kPi * t) / (kPi * t)) * window;
    }
    std::vector<float> limited (length);
    for (size_t i = 0; i < length; ++i)
    {
        double v = 0.0;
        for (size_t j = 0; j < 511; ++j)
            v += raw[i + j] * taps[j];
        limited[i] = (float) v;
    }
    const double scale = 10.0 / peakOf (limited);
    for (auto& v : limited)
        v = (float) (v * scale);
    fadeIn (limited, fs);
    signals.push_back ({ "band-limited noise", limited, true });
    return signals;
}

//==============================================================================
void testCeiling()
{
    std::printf ("Ceiling: hot signals at 44.1/48/96/192 kHz, three domains, three releases, three ceilings\n");
    const char* domainNames[] = { "Digital", "Analog", "True Peak" };
    double worstSample[3] = { -100.0, -100.0, -100.0 };
    double worstReferenceBandLimited[3] = { -100.0, -100.0, -100.0 };
    double lowestReferenceBandLimited[3] = { 100.0, 100.0, 100.0 };   // the limiter must not over-limit either
    double worstReferenceFullBand[3] = { -100.0, -100.0, -100.0 };
    std::string worstFullBandName[3];
    for (double fs : kRates)
    {
        const auto signals = hotSignals (fs);
        for (int domain = 0; domain < 3; ++domain)
            for (float releaseMs : { 0.01f, 7.0f, 1000.0f })
                for (float ceilingDb : { 0.0f, -0.3f, -6.0f })
                {
                    Params p = baseParams();
                    p.domain = (Domain) domain;
                    p.releaseMs = releaseMs;
                    p.ceilingDb = ceilingDb;
                    p.autoRelease = releaseMs == 7.0f;
                    const double ceiling = fromDb (ceilingDb);
                    for (const auto& s : signals)
                    {
                        auto x = s.data;
                        auto l = makeLimiter (fs, p);
                        runMono (l, x, 480);
                        const size_t from = (size_t) l.latencySamples();
                        worstSample[domain] = std::max (worstSample[domain], db (peakOf (x, from) / ceiling));
                        if (domain != 0)
                        {
                            const double r = db (referencePeak (x, from, ! s.bandLimited) / ceiling);
                            if (s.bandLimited)
                            {
                                worstReferenceBandLimited[domain] = std::max (worstReferenceBandLimited[domain], r);
                                lowestReferenceBandLimited[domain] = std::min (lowestReferenceBandLimited[domain], r);
                            }
                            else if (r > worstReferenceFullBand[domain])
                            {
                                worstReferenceFullBand[domain] = r;
                                worstFullBandName[domain] = fmt ("%s at %.0f Hz, ceiling %.1f dB", s.name.c_str(), fs, ceilingDb);
                            }
                        }
                    }
                }
    }
    for (int domain = 0; domain < 3; ++domain)
        check (worstSample[domain] <= 1e-6, fmt ("%s: no sample above the ceiling (worst %+.7f dB re ceiling)", domainNames[domain], worstSample[domain]));
    check (worstReferenceBandLimited[2] <= 0.05 && lowestReferenceBandLimited[2] >= -0.1,
           fmt ("True Peak: 16x reference reconstruction of band-limited signals reaches the ceiling within 0.05 dB above / 0.1 dB below (every run between %+.4f and %+.4f dB)",
                lowestReferenceBandLimited[2], worstReferenceBandLimited[2]));
    std::printf ("        True Peak, full-band signals (informational, depends on the reconstruction filter): worst %+.3f dB, %s\n",
                 worstReferenceFullBand[2], worstFullBandName[2].c_str());
    const double margin = Limiter::analogMarginDb();
    check (worstReferenceFullBand[1] <= 0.0 && worstReferenceBandLimited[1] <= -margin + 0.05 && lowestReferenceBandLimited[1] >= -margin - 0.1,
           fmt ("Analog: 16x reference reconstruction never above the ceiling on any signal (worst %+.3f dB full-band on %s; band-limited runs between %+.3f and %+.3f dB; margin %.2f dB)",
                worstReferenceFullBand[1], worstFullBandName[1].c_str(), lowestReferenceBandLimited[1], worstReferenceBandLimited[1], margin));
}

void testLatency()
{
    std::printf ("Latency: an impulse arrives latencySamples() later, unchanged, for every rate and domain\n");
    bool ok = true;
    std::string report;
    for (double fs : kRates)
    {
        int reported = -1;
        for (int domain = 0; domain < 3; ++domain)
        {
            Params p = baseParams();
            p.domain = (Domain) domain;
            auto l = makeLimiter (fs, p);
            std::vector<float> x ((size_t) fs / 10, 0.0f);
            x[1000] = 0.25f;
            runMono (l, x, 100);
            const auto at = (int) (std::max_element (x.begin(), x.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }) - x.begin());
            const bool good = at - 1000 == l.latencySamples() && std::abs (x[(size_t) at] - 0.25f) < 1e-6f;
            if (domain == 0)
                reported = l.latencySamples();
            ok = ok && good && reported == l.latencySamples();
            if (! good)
                std::printf ("        %.0f Hz domain %d: latency %d, measured %d, value %.6f\n", fs, domain, l.latencySamples(), at - 1000, x[(size_t) at]);
        }
        report += fmt ("%s%.1f kHz: %d samples (%.2f ms)", report.empty() ? "" : ", ", fs / 1000.0, reported, 1000.0 * reported / fs);
    }
    check (ok, "latency as reported, identical in all three domains: " + report);
}

void testBelowThreshold()
{
    std::printf ("Below-threshold gain and Link\n");
    LinkedControls link;
    link.setRange (-30.0f, 0.0f);
    link.reset (0.0f, 0.0f, false);
    auto r = link.update (-6.0f, -1.0f, false);
    bool ok = r.thresholdDb == -6.0f && r.ceilingDb == -1.0f && ! r.writeThreshold && ! r.writeCeiling;
    r = link.update (-6.0f, -1.0f, true);                  // Link on: baseline, no jump
    ok = ok && r.thresholdDb == -6.0f && r.ceilingDb == -1.0f && ! r.writeCeiling;
    r = link.update (-9.0f, -1.0f, true);                  // threshold -3 dB: ceiling follows
    ok = ok && r.thresholdDb == -9.0f && std::abs (r.ceilingDb + 4.0f) < 1e-5f && r.writeCeiling;
    r = link.update (-9.0f, -1.0f, true);                  // the write has not landed yet
    ok = ok && std::abs (r.ceilingDb + 4.0f) < 1e-5f && ! r.writeCeiling && ! r.writeThreshold;
    r = link.update (-9.0f, -4.0f, true);                  // it lands: not a move
    ok = ok && r.thresholdDb == -9.0f && std::abs (r.ceilingDb + 4.0f) < 1e-5f && ! r.writeCeiling && ! r.writeThreshold;
    r = link.update (-9.0f, -2.0f, true);                  // ceiling +2 dB: threshold follows
    ok = ok && std::abs (r.thresholdDb + 7.0f) < 1e-5f && r.ceilingDb == -2.0f && r.writeThreshold;
    r = link.update (-7.0f, -2.0f, true);
    r = link.update (-29.0f, -2.0f, true);                 // -22 dB: the ceiling clamps at -24
    ok = ok && r.thresholdDb == -29.0f && std::abs (r.ceilingDb + 24.0f) < 1e-5f && r.writeCeiling;
    r = link.update (-29.0f, -24.0f, true);
    r = link.update (-29.0f, -24.0f, false);               // Link off: nothing moves
    ok = ok && r.thresholdDb == -29.0f && std::abs (r.ceilingDb + 24.0f) < 1e-5f && ! r.writeCeiling;
    r = link.update (-6.0f, -24.0f, false);                // unlinked: independent
    ok = ok && r.thresholdDb == -6.0f && std::abs (r.ceilingDb + 24.0f) < 1e-5f;
    check (ok, "Link coupling: equal-dB moves, write-back round trip, clamping, no jump when toggled");

    for (int linked = 0; linked < 2; ++linked)
    {
        LinkedControls c;
        c.setRange (-30.0f, 0.0f);
        c.reset (0.0f, 0.0f, false);
        // Unlinked: threshold -6, ceiling -1. Linked: from 0/0 with Link on, the threshold moved to
        // -3 and then to -6 takes the ceiling along to -6 (the ceiling parameter is left where it was).
        auto v = linked ? (c.update (-3.0f, 0.0f, true), c.update (-6.0f, 0.0f, true)) : c.update (-6.0f, -1.0f, false);
        Params p = baseParams();
        p.thresholdDb = v.thresholdDb;
        p.ceilingDb = v.ceilingDb;
        auto l = makeLimiter (48000.0, p);
        std::vector<float> x (48000);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (float) (0.1 * std::sin (2.0 * kPi * 1000.0 * (double) i / 48000.0));
        runMono (l, x, 256);
        const double out = db (peakOf (x, x.size() / 2));
        const double expected = -20.0 + (v.ceilingDb - v.thresholdDb);
        check (std::abs (out - expected) < 0.01,
               fmt ("Link %s: -20 dBFS tone with threshold %.0f, ceiling %.0f comes out at %.3f dBFS (expected %.0f)", linked ? "on " : "off", v.thresholdDb, v.ceilingDb, out, expected));
    }

    // Gain reduction meter on a square 20 dB over.
    {
        Params p = baseParams();
        auto l = makeLimiter (48000.0, p);
        std::vector<float> x (48000);
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (i / 240) % 2 ? 10.0f : -10.0f;
        float shown = 0.0f;
        runMono (l, x, 480, [&] (size_t) { shown = l.meters().gainReductionDb; });
        check (std::abs (shown - 20.0f) < 0.01f, fmt ("gain reduction meter on a square 20 dB over: %.3f dB", shown));
    }
}

/** The limiter's gain per sample, recovered as output / delayed input where the input is
    large enough to resolve it; NaN elsewhere. */
std::vector<double> gainTrack (const std::vector<float>& in, const std::vector<float>& out, int latency, double minInput)
{
    std::vector<double> g (out.size(), std::nan (""));
    for (size_t n = (size_t) latency; n < out.size(); ++n)
        if (std::abs (in[n - (size_t) latency]) >= minInput)
            g[n] = out[n] / in[n - (size_t) latency];
    return g;
}

void testRelease()
{
    std::printf ("Release: 95 %% recovery of the gain-reduction envelope in about 3 tau after a burst 6 dB over\n");
    const double fs = 48000.0;
    const int m = (int) std::lround (0.5 * Limiter::kLookAheadSeconds * fs) + 1;
    for (float tauMs : { 1.0f, 10.0f, 100.0f, 1000.0f })
    {
        Params p = baseParams();
        p.releaseMs = tauMs;
        auto l = makeLimiter (fs, p);
        const size_t burstStart = 9600, burstEnd = burstStart + 4800 + 13;   // the burst's last sample is a crest
        std::vector<float> x ((size_t) (0.4 * fs + 4.0 * tauMs * 1e-3 * fs));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (float) ((i >= burstStart && i < burstEnd ? 2.0 : 0.1) * std::sin (2.0 * kPi * 1000.0 * (double) i / fs));
        auto y = x;
        runMono (l, y, 256);
        const auto g = gainTrack (x, y, l.latencySamples(), 0.07);
        double maxGr = 0.0;
        for (size_t n = burstStart; n < burstEnd; ++n)
            if (! std::isnan (g[n + (size_t) l.latencySamples()]))
                maxGr = std::max (maxGr, -db (g[n + (size_t) l.latencySamples()]));
        size_t recovered = 0;
        for (size_t n = burstEnd + (size_t) l.latencySamples(); n < g.size(); ++n)
            if (! std::isnan (g[n]) && -db (g[n]) <= 0.05 * maxGr)
            {
                recovered = n - (size_t) l.latencySamples() - burstEnd;
                break;
            }
        const double measuredMs = 1000.0 * (double) recovered / fs;
        // The look-ahead hold expires as the delayed peak passes, so from the last crest the
        // envelope decays as exp (-t / tau) at once; 95 % recovery is at ln (20) tau = 2.996 tau,
        // seen through the two averages (M - 1 samples of delay).
        const double expectedMs = std::log (20.0) * tauMs + 1000.0 * (m - 1) / fs;
        check (recovered > 0 && std::abs (measuredMs - expectedMs) <= 0.1 * 3.0 * tauMs + 0.3 && std::abs (maxGr - 6.0) < 0.05,
               fmt ("tau %7.1f ms: %.2f dB reduction recovers 95 %% in %.2f ms (expected %.2f = ln 20 tau + smoothing)", tauMs, maxGr, measuredMs, expectedMs));
    }
}

/** Power at `hz` over x[from..], Goertzel. */
double powerAt (const std::vector<float>& x, size_t from, double fs, double hz)
{
    const double w = 2.0 * kPi * hz / fs, c = 2.0 * std::cos (w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    const size_t n = x.size() - from;
    for (size_t i = from; i < x.size(); ++i)
    {
        s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos (w), im = s2 * std::sin (w);
    return (re * re + im * im) / ((double) n * (double) n);
}

void testAutoRelease()
{
    std::printf ("Auto release\n");
    const double fs = 48000.0;
    double thd[2] = { 0.0, 0.0 }, ripple[2] = { 0.0, 0.0 };
    for (int autoOn = 0; autoOn < 2; ++autoOn)
    {
        Params p = baseParams();
        p.releaseMs = 1.0f;
        p.autoRelease = autoOn == 1;
        auto l = makeLimiter (fs, p);
        std::vector<float> x ((size_t) (3.0 * fs));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (float) (2.0 * std::sin (2.0 * kPi * 50.0 * (double) i / fs));
        runMono (l, x, 256);
        const size_t from = x.size() - (size_t) fs;
        const double fundamental = powerAt (x, from, fs, 50.0);
        double harmonics = 0.0;
        for (int k = 2; k <= 40; ++k)
            harmonics += powerAt (x, from, fs, 50.0 * k);
        thd[autoOn] = std::sqrt (harmonics / fundamental);
        // Pumping: the cycle peaks over the last second should all be the same.
        double lo = 1e9, hi = 0.0;
        for (size_t c = from; c + 960 <= x.size(); c += 960)
        {
            const double pk = peakOf (std::vector<float> (x.begin() + (long) c, x.begin() + (long) c + 960));
            lo = std::min (lo, pk);
            hi = std::max (hi, pk);
        }
        ripple[autoOn] = db (hi / lo);
    }
    check (thd[1] < thd[0], fmt ("50 Hz tone 6 dB over, release 1 ms: THD %.2f %% with Auto off, %.2f %% with Auto on", 100.0 * thd[0], 100.0 * thd[1]));
    check (ripple[1] < 0.1, fmt ("steady tone with Auto on: cycle-to-cycle level variation %.3f dB (no pumping)", ripple[1]));

    // Recovery after an isolated 5 ms peak and after 2 s of limiting, release 50 ms.
    for (int autoOn = 0; autoOn < 2; ++autoOn)
    {
        double recovery[2];
        for (int sustained = 0; sustained < 2; ++sustained)
        {
            Params p = baseParams();
            p.releaseMs = 50.0f;
            p.autoRelease = autoOn == 1;
            auto l = makeLimiter (fs, p);
            const size_t burst = sustained ? 96000 : 240;
            std::vector<float> x (burst + 96000);
            std::mt19937 rng (5);
            std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = i < burst ? 3.16f * (i % 2 ? 1.0f : -1.0f) * (0.5f + 0.5f * std::abs (noise (rng))) : 0.01f;
            size_t recovered = 0;
            runMono (l, x, 48, [&] (size_t start) { if (start > burst + 200 && recovered == 0 && l.meters().gainReductionDb < 1.0f) recovered = start; });
            recovery[sustained] = 1000.0 * (double) (recovered - burst) / fs;
        }
        std::printf ("        auto %s: back within 1 dB %.0f ms after an isolated peak, %.0f ms after 2 s of 10 dB limiting (release 50 ms)\n",
                     autoOn ? "on " : "off", recovery[0], recovery[1]);
        if (autoOn)
            check (recovery[0] < 200.0 && recovery[1] > 1.5 * recovery[0], "Auto on: an isolated peak recovers at the setting, sustained limiting releases more slowly");
    }

    // Stability on 10 s of dense material.
    {
        Params p = baseParams();
        p.releaseMs = 1.0f;
        p.autoRelease = true;
        p.thresholdDb = -12.0f;
        p.ceilingDb = -0.3f;
        p.domain = Domain::TruePeak;
        auto l = makeLimiter (fs, p);
        std::vector<float> a ((size_t) (10.0 * fs)), b (a.size());
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
        for (size_t i = 0; i < a.size(); ++i)
        {
            const double env = 0.5 + 0.5 * std::sin (2.0 * kPi * 2.3 * (double) i / fs) * std::sin (2.0 * kPi * 0.37 * (double) i / fs);
            const double tone = std::sin (2.0 * kPi * 55.0 * (double) i / fs) + 0.5 * std::sin (2.0 * kPi * 3000.0 * (double) i / fs);
            a[i] = (float) (4.0 * env * (noise (rng) * 0.7 + tone));
            b[i] = (float) (4.0 * env * (noise (rng) * 0.7 + tone * 0.9));
        }
        float deepest = 0.0f;
        bool finite = true;
        runStereo (l, a, b, 512, [&] (size_t) { deepest = std::max (deepest, l.meters().gainReductionDb); finite = finite && std::isfinite (l.meters().gainReductionDb); });
        for (float v : a) finite = finite && std::isfinite (v);
        for (float v : b) finite = finite && std::isfinite (v);
        const double peak = std::max (peakOf (a, (size_t) l.latencySamples()), peakOf (b, (size_t) l.latencySamples()));
        check (finite && deepest < 40.0f && db (peak / fromDb (-0.3)) <= 1e-6,
               fmt ("10 s of dense stereo material, threshold -12, Auto on, release 1 ms: finite, deepest reduction %.1f dB, peak %+.6f dB re ceiling", deepest, db (peak / fromDb (-0.3))));
    }
}

/** One random control change, drawn in a stated order. C++ leaves the evaluation order of function
    arguments unspecified (clang draws left to right, MSVC right to left), so each draw is its own
    statement and both orders are run on every compiler. */
struct ControlDraw
{
    float threshold, ceiling;
    bool link;
};

ControlDraw drawControls (std::mt19937& rng, bool rightToLeft)
{
    ControlDraw d {};
    if (rightToLeft)
    {
        d.link = (rng() & 1) != 0;
        d.ceiling = -(float) (rng() % 13);
        d.threshold = -(float) (rng() % 25);
    }
    else
    {
        d.threshold = -(float) (rng() % 25);
        d.ceiling = -(float) (rng() % 13);
        d.link = (rng() & 1) != 0;
    }
    return d;
}

/** Largest one-sample change of the tone's gain, as a fraction of the highest gain within 2M + 2
    samples either side. */
double largestGainStep (double fs, bool changing, bool rightToLeft)
{
    const int m = (int) std::lround (0.5 * Limiter::kLookAheadSeconds * fs) + 1;
    Params p = baseParams();
    if (! changing)
    {
        p.thresholdDb = -24.0f;   // the fastest legitimate gain motion: deep limiting, 0.01 ms release
        p.releaseMs = 0.01f;
    }
    auto l = makeLimiter (fs, p);
    std::vector<float> x ((size_t) (2.0 * fs));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (0.7 * std::sin (2.0 * kPi * 150.0 * (double) i / fs));
    auto a = x, b = x;
    std::mt19937 rng (11);
    LinkedControls link;
    link.setRange (-30.0f, 0.0f);
    link.reset (0.0f, 0.0f, false);
    runStereo (l, a, b, 256, [&] (size_t s)
    {
        if (! changing || (s / 256) % 8 != 0)
            return;
        Params q = baseParams();
        const ControlDraw d = drawControls (rng, rightToLeft);
        const auto r = link.update (d.threshold, d.ceiling, d.link);
        q.thresholdDb = r.thresholdDb;
        q.ceilingDb = r.ceilingDb;
        q.releaseMs = (float) std::pow (10.0, -2.0 + 5.0 * (double) (rng() % 100) / 99.0);
        q.autoRelease = (rng() & 1) != 0;
        q.domain = (Domain) (rng() % 3);
        q.inputGainDbL = q.inputGainDbR = -(float) (rng() % 7);
        l.setParams (q);
    });
    const size_t latency = (size_t) l.latencySamples();
    std::vector<double> gain (a.size(), -1.0);   // negative: unresolvable near zero crossings
    for (size_t n = latency; n < a.size(); ++n)
        if (std::abs (x[n - latency]) >= 0.2)
            gain[n] = a[n] / x[n - latency];
    const size_t reach = (size_t) (2 * m + 2);
    double step = 0.0;
    for (size_t n = latency + 1; n < a.size(); ++n)
    {
        if (gain[n] < 0.0 || gain[n - 1] < 0.0)
            continue;
        double highest = 0.0;
        for (size_t k = n > reach ? n - reach : 0; k < std::min (a.size(), n + reach + 1); ++k)
            highest = std::max (highest, gain[k]);
        step = std::max (step, std::abs (gain[n] - gain[n - 1]) / highest);
    }
    return step;
}

void testNoClicks()
{
    std::printf ("No clicks: abrupt control changes over a steady tone\n");
    double worst[2] = { 0.0, 0.0 }, deepStatic = 0.0, bound = 0.0;
    for (double fs : kRates)
    {
        const int m = (int) std::lround (0.5 * Limiter::kLookAheadSeconds * fs) + 1;
        // The gain is measured as a fraction of the highest gain within 2M + 2 samples (a step is
        // judged against the level it moves towards, not the near-silent level it leaves: a
        // 25 dB release from deep limiting is one step of 1.0 against 0.06). The limiter's own
        // gain is the hold's step response through two moving averages of length M: at most 1/M of
        // the step per sample. The input, threshold and ceiling gains glide for 20 ms, and the
        // output carries their product, whose relative change per sample is the sum of theirs:
        // each starts at most (R - 1) / (0.02 fs) after a jump of ratio R, which is 24 dB for the
        // threshold and, through Link, for the ceiling, and 6 dB for the input gain.
        bound = std::max (bound, 1.0 / m + (2.0 * (fromDb (24.0) - 1.0) + (fromDb (6.0) - 1.0)) / (0.02 * fs));
        deepStatic = std::max (deepStatic, largestGainStep (fs, false, false));
        for (int order = 0; order < 2; ++order)
            worst[order] = std::max (worst[order], largestGainStep (fs, true, order != 0));
    }
    const char* names[2] = { "left to right", "right to left" };
    for (int order = 0; order < 2; ++order)
        check (worst[order] <= bound, fmt ("threshold, ceiling, link, release, auto, domain and input gain changed every 43-87 ms, drawn %s: largest one-sample gain change %.2f %% (bound %.2f %%; static deep limiting at 0.01 ms release: %.2f %%)",
                                           names[order], 100.0 * worst[order], 100.0 * bound, 100.0 * deepStatic));
}

//==============================================================================
/** Iterative radix-2 FFT, in place. */
void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

/** Averaged power spectrum (Hann, 8192 points) of a signal; bins 0..N/2. */
std::vector<double> powerSpectrum (const std::vector<double>& x)
{
    constexpr size_t n = 8192;
    std::vector<double> psd (n / 2 + 1, 0.0);
    std::vector<std::complex<double>> frame (n);
    size_t frames = 0;
    for (size_t start = 0; start + n <= x.size(); start += n / 2, ++frames)
    {
        for (size_t i = 0; i < n; ++i)
            frame[i] = x[start + i] * (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n));
        fft (frame);
        for (size_t k = 0; k <= n / 2; ++k)
            psd[k] += std::norm (frame[k]);
    }
    for (double& v : psd)
        v /= (double) std::max<size_t> (frames, 1);
    return psd;
}

double terhardtWeight (double hz)
{
    if (hz < 20.0)
        return 0.0;
    const double f = hz / 1000.0;
    const double t = 3.64 * std::pow (f, -0.8) - 6.5 * std::exp (-0.6 * (f - 3.3) * (f - 3.3)) + 1e-3 * f * f * f * f;
    return std::pow (10.0, -t / 10.0);
}

double weightedPower (const std::vector<double>& psd, double fs)
{
    double p = 0.0;
    for (size_t k = 0; k < psd.size(); ++k)
        p += psd[k] * terhardtWeight (fs * (double) k / (2.0 * (double) (psd.size() - 1)));
    return p;
}

/** Error signal (output minus input) of the re-quantizer on a sine of `amplitude` at 997 Hz. */
std::vector<double> requantizerError (double fs, Quantize q, Dither d, Shaping s, double amplitude, double seconds, std::vector<double>* output = nullptr)
{
    Requantizer r;
    r.prepare (fs);
    r.set (q, d, s);
    const size_t n = (size_t) (seconds * fs);
    std::vector<double> e (n);
    if (output)
        output->resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double x = amplitude * std::sin (2.0 * kPi * 997.0 * (double) i / fs);
        const double y = r.process (0, x);
        e[i] = y - x;
        if (output)
            (*output)[i] = y;
    }
    return e;
}

void testQuantizer()
{
    std::printf ("Quantizer\n");
    const double fs = 48000.0;
    const Quantize depths[] = { Quantize::Bits8, Quantize::Bits12, Quantize::Bits16, Quantize::Bits20, Quantize::Bits24 };
    {
        bool multiples = true;
        for (Quantize q : depths)
        {
            Params p = baseParams();
            p.quantize = q;
            p.dither = Dither::Type1;
            p.shaping = Shaping::Normal;
            auto l = makeLimiter (fs, p);
            std::vector<float> a (24000), b (24000);
            std::mt19937 rng (2);
            std::uniform_real_distribution<float> noise (-0.25f, 0.25f);
            for (size_t i = 0; i < a.size(); ++i) { a[i] = noise (rng); b[i] = noise (rng); }
            runStereo (l, a, b, 256);
            const double scale = std::ldexp (1.0, quantizeBits (q) - 1);
            for (size_t i = 0; i < a.size(); ++i)
                for (float v : { a[i], b[i] })
                    multiples = multiples && (double) v * scale == std::floor ((double) v * scale);
        }
        check (multiples, "every output sample is a whole number of LSBs at 8, 12, 16, 20 and 24 bits (through the full chain, Type 1, Normal)");
    }
    {
        bool saturates = true;
        for (Quantize q : depths)
        {
            Requantizer r;
            r.prepare (fs);
            r.set (q, Dither::Type1, Shaping::Ultra);
            const double scale = std::ldexp (1.0, quantizeBits (q) - 1);
            for (int i = 0; i < 2000; ++i)
            {
                const double x = (i % 4 == 0 ? 1.0 : i % 4 == 1 ? 1.5 : i % 4 == 2 ? -1.0 : -1.5) * (1.0 + 0.001 * i);
                const double y = r.process (0, x);
                saturates = saturates && y <= (scale - 1.0) / scale && y >= -1.0 && (x > 0 ? y > 0.9 : y < -0.9);
            }
        }
        check (saturates, "beyond full scale the output saturates at +-full scale and never wraps");
    }
    {
        Requantizer r;
        r.prepare (fs);
        r.set (Quantize::Bits16, Dither::None, Shaping::None);
        bool plain = true;
        std::mt19937 rng (4);
        std::uniform_real_distribution<double> noise (-1.0, 1.0);
        for (int i = 0; i < 100000; ++i)
        {
            const double x = noise (rng);
            plain = plain && r.process (i & 1, x) == std::floor (x * 32768.0 + 0.5) / 32768.0;
        }
        check (plain, "Dither None + Shaping None is plain rounding to the nearest LSB");
    }
    {
        // Type 2 goes black on silence, Type 1 does not.
        int zeroAfterMs[3] = { -1, -1, -1 };
        for (int type = 1; type <= 2; ++type)
            for (Shaping s : { Shaping::None, Shaping::Ultra })
            {
                Params p = baseParams();
                p.quantize = Quantize::Bits16;
                p.dither = (Dither) type;
                p.shaping = s;
                auto l = makeLimiter (fs, p);
                std::vector<float> x (48000, 0.0f);
                for (size_t i = 0; i < 9600; ++i)
                    x[i] = (float) (0.5 * std::sin (2.0 * kPi * 440.0 * (double) i / fs));
                runMono (l, x, 256);
                const size_t silenceStart = 9600 + (size_t) l.latencySamples();
                size_t lastNonZero = 0;
                for (size_t i = silenceStart; i < x.size(); ++i)
                    if (x[i] != 0.0f)
                        lastNonZero = i;
                const int ms = lastNonZero == 0 ? 0 : (int) std::lround (1000.0 * (double) (lastNonZero - silenceStart) / fs);
                if (type == 2)
                    zeroAfterMs[s == Shaping::None ? 0 : 1] = ms;
                else if (s == Shaping::Ultra)
                    zeroAfterMs[2] = lastNonZero > x.size() - 4800 ? 1 : 0;
            }
        check (zeroAfterMs[0] >= 0 && zeroAfterMs[0] < 200 && zeroAfterMs[1] >= 0 && zeroAfterMs[1] < 200,
               fmt ("Type 2 output is exact digital zero %d ms (no shaping) / %d ms (Ultra) after the input goes silent", zeroAfterMs[0], zeroAfterMs[1]));
        check (zeroAfterMs[2] == 1, "Type 1 keeps dithering through silence");
    }
    {
        const auto e1 = requantizerError (fs, Quantize::Bits16, Dither::Type1, Shaping::None, 0.1, 2.0);
        const auto e2 = requantizerError (fs, Quantize::Bits16, Dither::Type2, Shaping::None, 0.1, 2.0);
        auto power = [] (const std::vector<double>& e) { double p = 0.0; for (double v : e) p += v * v; return p / (double) e.size(); };
        const double p1 = 10.0 * std::log10 (power (e1) * 32768.0 * 32768.0), p2 = 10.0 * std::log10 (power (e2) * 32768.0 * 32768.0);
        // Total error: TPDF 1/6 + rounding 1/12 = 1/4 LSB^2 against RPDF 1/12 + 1/12 = 1/6 LSB^2:
        // 1.76 dB apart (the dither alone is 3 dB apart).
        check (std::abs ((p1 - p2) - 1.76) < 0.2,
               fmt ("noise floor at 16 bits, no shaping: Type 1 %.2f dB re LSB^2 (theory -6.02), Type 2 %.2f dB (theory -7.78); difference %.2f dB (theory 1.76)", p1, p2, p1 - p2));
    }
}

void testNoiseShaping()
{
    std::printf ("Noise shaping\n");
    const double targets[] = { 6.0, 8.5, 10.5 };
    const char* names[] = { "Moderate", "Normal", "Ultra" };
    for (double fs : { 44100.0, 48000.0 })
    {
        const auto none = powerSpectrum (requantizerError (fs, Quantize::Bits16, Dither::Type1, Shaping::None, 0.1, 6.0));
        const double reference = weightedPower (none, fs);
        for (int s = 0; s < 3; ++s)
        {
            const auto shaped = powerSpectrum (requantizerError (fs, Quantize::Bits16, Dither::Type1, (Shaping) (s + 1), 0.1, 6.0));
            const double reduction = 10.0 * std::log10 (reference / weightedPower (shaped, fs));
            double boost = 0.0, boostHz = 0.0;
            for (size_t k = 1; k < shaped.size(); ++k)
                if (shaped[k] / none[k] > boost)
                {
                    boost = shaped[k] / none[k];
                    boostHz = fs * (double) k / (2.0 * (double) (shaped.size() - 1));
                }
            check (std::abs (reduction - targets[s]) <= 1.0,
                   fmt ("%.1f kHz, 16-bit Type 1, %-8s: weighted noise reduction %.2f dB (target %.1f), peak boost %.1f dB at %.1f kHz",
                        fs / 1000.0, names[s], reduction, targets[s], 10.0 * std::log10 (boost), boostHz / 1000.0));
        }
    }
    {
        // Bounded on 60 s of full-scale noise and of silence, every shaper.
        bool bounded = true;
        double worst = 0.0;
        for (int s = 1; s <= 3; ++s)
            for (int silent = 0; silent < 2; ++silent)
            {
                Requantizer r;
                r.prepare (44100.0);
                r.set (Quantize::Bits16, Dither::Type1, (Shaping) s);
                std::mt19937 rng (9);
                std::uniform_real_distribution<double> noise (-1.0, 1.0);
                for (int i = 0; i < 60 * 44100; ++i)
                {
                    const double x = silent ? 0.0 : noise (rng);
                    const double y = r.process (0, x);
                    const double err = std::abs (y - x) * 32768.0;
                    worst = std::max (worst, silent ? std::abs (y) * 32768.0 : err);
                    bounded = bounded && std::isfinite (y) && err < 16.0;
                }
            }
        check (bounded, fmt ("error-feedback loop bounded over 60 s of full-scale noise and 60 s of silence (largest error %.2f LSB)", worst));
    }
    {
        // Low-level sines at 8 bits: plain truncation versus Normal shaping without dither. At
        // -60 dBFS (0.13 LSB) both are silent: without dither nothing below about half an LSB
        // can come through a minimum-phase shaper either. At -50 dBFS (0.4 LSB) truncation is
        // still silent while the shaper's error feedback carries the tone; at -40 dBFS (1.3 LSB)
        // truncation is a stepped wave full of harmonics.
        const double fs = 44100.0;
        for (double levelDb : { -50.0, -40.0 })
        {
            std::vector<double> outT, outS;
            const auto eT = requantizerError (fs, Quantize::Bits8, Dither::None, Shaping::None, fromDb (levelDb), 4.0, &outT);
            const auto eS = requantizerError (fs, Quantize::Bits8, Dither::None, Shaping::Normal, fromDb (levelDb), 4.0, &outS);
            const double wT = 10.0 * std::log10 (weightedPower (powerSpectrum (eT), fs) + 1e-30), wS = 10.0 * std::log10 (weightedPower (powerSpectrum (eS), fs) + 1e-30);
            auto harmonics = [&] (const std::vector<double>& y)
            {
                std::vector<float> f (y.begin(), y.end());
                double h = 0.0;
                for (int k = 2; k <= 10; ++k)
                    h += powerAt (f, 0, fs, 997.0 * k);
                return 10.0 * std::log10 (h + 1e-30) - (levelDb - 6.02);   // dB re the sine's power
            };
            auto fundamental = [&] (const std::vector<double>& y)
            {
                std::vector<float> f (y.begin(), y.end());
                return 10.0 * std::log10 (2.0 * powerAt (f, 0, fs, 997.0) + 1e-30);
            };
            // The harmonic comparison only means something where truncation produces output.
            const bool truncationSilent = fundamental (outT) < -200.0;
            check (wS < wT && (truncationSilent || harmonics (outS) < harmonics (outT)),
                   fmt ("%.0f dBFS 997 Hz sine at 8 bits, no dither: Terhardt-weighted error %.1f dB lower with Normal shaping than truncated; harmonics 2-10 %.1f dB re the sine shaped vs %s truncated; fundamental %.1f vs %s dBFS",
                        levelDb, wT - wS, harmonics (outS), truncationSilent ? "(silent)" : fmt ("%.1f dB", harmonics (outT)).c_str(),
                        fundamental (outS), truncationSilent ? "(silent)" : fmt ("%.1f", fundamental (outT)).c_str()));
        }
    }
}

void testDeterminismAndDenormals()
{
    std::printf ("Determinism and denormals\n");
    const double fs = 48000.0;
    {
        Params p = baseParams();
        p.quantize = Quantize::Bits16;
        p.dither = Dither::Type1;
        p.shaping = Shaping::Ultra;
        p.thresholdDb = -8.0f;
        p.ceilingDb = -0.3f;
        p.autoRelease = true;
        p.domain = Domain::TruePeak;
        std::vector<float> a0 (96000), b0 (96000);
        std::mt19937 rng (13);
        std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
        for (size_t i = 0; i < a0.size(); ++i) { a0[i] = noise (rng); b0[i] = noise (rng); }
        auto a1 = a0, b1 = b0, a2 = a0, b2 = b0;
        auto l1 = makeLimiter (fs, p);
        runStereo (l1, a1, b1, 256);
        auto l2 = makeLimiter (fs, p);
        runStereo (l2, a2, b2, 333);
        l2.reset();
        auto a3 = a0, b3 = b0;
        runStereo (l2, a3, b3, 256);
        const bool same = std::memcmp (a1.data(), a2.data(), a1.size() * 4) == 0 && std::memcmp (b1.data(), b2.data(), b1.size() * 4) == 0
                          && std::memcmp (a1.data(), a3.data(), a1.size() * 4) == 0 && std::memcmp (b1.data(), b3.data(), b1.size() * 4) == 0;
        check (same, "two instances, different block sizes, and a reset instance give bit-identical output (Type 1, Ultra, Auto, True Peak)");
    }
    {
        auto timeRun = [&] (bool decaying)
        {
            Params p = baseParams();
            p.domain = Domain::TruePeak;
            p.autoRelease = true;
            auto l = makeLimiter (fs, p);
            std::vector<float> a ((size_t) (30.0 * fs)), b (a.size());
            for (size_t i = 0; i < a.size(); ++i)
            {
                const double t = (double) i / fs;
                const double env = decaying ? std::pow (10.0, -60.0 * t / 20.0) : 1.0;   // 60 dB/s: float denormals after 12.6 s, zero after 15 s
                a[i] = b[i] = (float) (env * std::sin (2.0 * kPi * 1000.0 * t));
            }
            const auto start = std::chrono::steady_clock::now();
            runStereo (l, a, b, 512);
            return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        };
        const double steady = timeRun (false), tail = timeRun (true);
        check (tail < 1.5 * steady + 0.05, fmt ("30 s stereo at 48 kHz: steady tone %.3f s, decaying tail into denormals and silence %.3f s (%.0fx realtime)", steady, tail, 30.0 / tail));
    }
    {
        Params p = baseParams();
        p.thresholdDb = -6.0f;
        p.ceilingDb = -0.5f;
        p.domain = Domain::TruePeak;
        std::vector<float> x (48000);
        std::mt19937 rng (17);
        std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
        for (auto& v : x) v = noise (rng);
        auto mono = x, left = x, right = x;
        auto lm = makeLimiter (fs, p);
        runMono (lm, mono, 256);
        auto ls = makeLimiter (fs, p);
        runStereo (ls, left, right, 256);
        const bool same = std::memcmp (mono.data(), left.data(), x.size() * 4) == 0 && std::memcmp (mono.data(), right.data(), x.size() * 4) == 0;
        check (same, "mono and stereo (same signal on both channels) apply identical gains, bit for bit");
        p.quantize = Quantize::Bits16;
        p.dither = Dither::Type1;
        auto left2 = x, right2 = x;
        auto ls2 = makeLimiter (fs, p);
        runStereo (ls2, left2, right2, 256);
        check (std::memcmp (left2.data(), right2.data(), x.size() * 4) != 0, "with dither the two channels get independent noise");
    }
}
} // namespace

int main (int argc, char** argv)
{
    std::printf ("Limiter1 DSP tests\n");
    const auto start = std::chrono::steady_clock::now();
    const std::string only = argc > 1 ? argv[1] : "";
    const std::pair<const char*, void (*)()> tests[] = {
        { "latency", testLatency }, { "threshold", testBelowThreshold }, { "release", testRelease },
        { "auto", testAutoRelease }, { "clicks", testNoClicks }, { "quantizer", testQuantizer },
        { "shaping", testNoiseShaping }, { "determinism", testDeterminismAndDenormals }, { "ceiling", testCeiling },
    };
    for (const auto& [name, test] : tests)
        if (only.empty() || only == name)
            test();
    std::printf ("%d failure(s), %.1f s\n", failures, std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
    return failures == 0 ? 0 : 1;
}
