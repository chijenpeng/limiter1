# Limiter1: signal path, measured behaviour and limits

This is the algorithm as built, with every constant and the measurement behind it. All of it is in `Source/dsp/` (no JUCE) and checked by `tests/limiter_test.cpp`.

```
in L/R ──► input faders ──► ×(−Threshold) ──┬──► delay (look-ahead) ──► ×gain ──► ×Ceiling ──► re-quantizer ──► out
                                            │                              ▲
                                            └──► peak detector ──► 1/peak ──► running min ──► release ──► 2 moving averages
                                                 (Digital / True Peak / Analog)   (hold)      (dB, Auto)
```

Processing is double precision throughout; the input and output are float. Stereo detection is
linked (the maximum of both channels) and the same gain goes to both channels, so limiting one
side never shifts the image. Mono is the same path with one channel.

## 1. Gains

| Stage | Control | Implementation |
|---|---|---|
| Input faders | Input L / Input R, −30..0 dB | linear gain per channel, before everything; mono uses L |
| Threshold | −30..0 dB | a gain of −Threshold dB applied before both the detector and the delay; from there the limiter limits to 1.0 |
| Out Ceiling | −30..0 dB | a gain of Ceiling dB applied after the limiter |

Below the threshold the signal therefore gets a constant gain of Ceiling − Threshold dB, exactly
as the controls are labelled. All three gains glide with a one-pole of 20 ms time constant so
moving a fader never clicks. Because the threshold gain is applied before the detector, the
detector always sees exactly what it will later scale, and a threshold moving in either direction
cannot cause an overshoot. A ceiling moving *down* is honoured immediately: while the post gain
is still above its target the limiter aims below 1.0 by the part of the move not yet made
(`limit = min(1, target / current)`), so the output never exceeds the new ceiling even during the
glide; a ceiling moving up is trivially safe.

**Link** (plug-in only, `Source/dsp/LinkedControls.h`): while on, moving Threshold moves Ceiling by
the same number of dB and vice versa, clamped at the ends of the range. Turning Link on or off
takes the current pair as the baseline, so it never jumps the gain. The coupled value is written
back to the other parameter on the message thread, so host, UI and saved state agree; the
helper recognises its own write coming back and does not treat it as a user move.

## 2. Look-ahead, hold and smoothing

The look-ahead is 1.5 ms, realised as in Giannoulis, Massberg and Reiss (2012): with `M = round(0.5 · 1.5 ms · fs) + 1`,

* the required gain `r = limit / peak` (1 when under) is pushed through a running minimum over
  the last `2M` samples (a monotonic deque, O(1) per sample): the **hold**;
* the held value drives the release envelope (below);
* the envelope's linear gain goes through **two cascaded moving averages** of length `M`
  (a triangular window of `2M − 1` samples), so the gain glides down and reaches its value
  exactly when the delayed peak arrives;
* the audio is delayed by `2M − 2` samples plus the detector's own centre delay of 16 samples.

Zero overshoot follows from the window arithmetic (the hold is one sample longer than the
smoothing so that both samples around an inter-sample peak are covered), and the test confirms
it on squares, impulses, alternating-sign bursts, 0.45·fs tones, DC steps and white noise 20 dB
over, at every rate, domain, release and ceiling: the worst sample is +0.0000000 dB re ceiling.

**Latency** is the same for every setting and every domain, so host compensation never changes:

| Rate | M | latency (samples) | ms |
|---|---|---|---|
| 44.1 kHz | 34 | 82 | 1.86 |
| 48 kHz | 37 | 88 | 1.83 |
| 96 kHz | 73 | 160 | 1.67 |
| 192 kHz | 145 | 304 | 1.58 |

(16 samples of that are the true-peak detector, which is in the path whatever the domain.)

## 3. Release

The Release control (0.01–1000 ms, continuous) is the **time constant τ** of the exponential
recovery of the gain-reduction envelope *in dB*: after the hold lets go, `GR_dB(t) = GR_dB(0) · e^(−t/τ)`.
95 % recovery therefore takes ln 20 · τ ≈ 3τ, so the release you hear is about three times the marked
value. Measured at 48 kHz after a 6 dB over-threshold
burst (recovery to 5 % of the reduction, from the last peak, seen through the smoothing):

| τ | measured | expected ln 20 · τ + (M − 1)/fs |
|---|---|---|
| 1 ms | 3.85 ms | 3.75 ms |
| 10 ms | 30.85 ms | 30.71 ms |
| 100 ms | 300.35 ms | 300.32 ms |
| 1000 ms | 2996.5 ms | 2996.5 ms |

Because the hold expires as the delayed peak passes, the recovery starts right after the peak
in the output; the 1.5 ms of look-ahead is not added to it.

## 4. Auto Release

Program-dependent release. The limiter keeps a 300 ms one-pole average of the reduction the
signal *demands* (the held value, not the release tail), `sustain_dB`. With Auto on the
effective time constant is

```
τ_eff = min( τ + 20 ms · sustain_dB ,  max(τ, 500 ms) )
```

An isolated transient barely moves the average, so it recovers at the Release setting; dense
material that keeps hitting the ceiling lengthens the release progressively, by 20 ms per dB of
sustained reduction, up to 500 ms (or the setting itself if that is longer). The average is of
the demanded reduction, not of the envelope, so there is no feedback loop and nothing to
oscillate. Measured:

* 50 Hz tone 6 dB over, Release 1 ms: THD 12.4 % with Auto off, 1.0 % with Auto on; the cycle
  peaks of the output vary by 0.000 dB (no pumping).
* Release 50 ms: back within 1 dB 127 ms after an isolated 5 ms peak (119 ms with Auto off),
  364 ms after 2 s of 10 dB limiting.
* 10 s of dense stereo material with threshold −12 and Release 1 ms: bounded, deepest 31.7 dB.

## 5. Domains

* **Digital**: the detector reads sample peaks. No sample exceeds the ceiling.
* **True Peak**: the detector reads an 8× interpolated copy of the signal — seven phases of a
  32-tap Kaiser-windowed sinc (β = 5), each phase scaled so that it never reads low up to
  0.45·fs (it reads at most 0.07 dB high there) — and refines every local maximum with a
  parabola through its three neighbouring oversampled points. Against a 16×, 256-tap (β = 9)
  reference reconstruction, the output of band-limited signals (content below 0.45·fs) reaches
  the ceiling to within −0.034 / +0.000 dB in every run (the check is two-sided: the limiter
  must neither let a peak through nor over-limit). On signals with content right up to Nyquist
  (alternating-sign bursts, naive squares, impulses) the peak of the reconstruction depends on
  the DAC's filter; against the sharp reference the output can reach +1.15 dB over the ceiling
  (worst case in the test: 8–31-sample Nyquist bursts at 96 kHz). The detector is computed
  only when the domain needs it; its 16 samples of delay are always in the path, so switching
  domains never changes the latency.
* **Analog**: True Peak detection with a fixed **1.5 dB margin** (the reading is multiplied by
  1.189 before the gain is derived). With it the 16× reference reconstruction never exceeds the
  ceiling on any of the adversarial signals at any rate: worst −0.35 dB (bursts at 96 kHz), and
  band-limited signals come out between −1.53 and −1.50 dB re ceiling. The margin was set from
  the measured need of 1.16 dB plus spare.

Why the margin is what it is, and its limit: the ideal reconstruction of an N-sample burst at
exactly the Nyquist frequency overshoots its sample peak by a sum of sinc tails that grows
like log N, so a short detector with a tapered window cannot see all of it. Measured with the
probe in the test's reference (256 taps, 16×) against the 32-tap detector, on a bare burst:

| N | reference | detector | under-read |
|---|---|---|---|
| 2 | +0.76 dB | +0.76 dB | 0.00 dB |
| 8 | +3.11 dB | +2.76 dB | 0.35 dB |
| 16 | +4.16 dB | +3.09 dB | 1.06 dB |
| 31 | +5.03 dB | +3.09 dB | 1.94 dB |
| 64 | +5.61 dB | +3.09 dB | 2.52 dB |
| ≥128 | +5.71 dB | +3.09 dB | 2.62 dB |

(In the limiter the need is smaller than the bare under-read — 1.16 dB for the test's
8–31-sample bursts 20 dB over — because the gain is already pulled down around the burst.)
Bursts of 64 samples and more at exactly Nyquist would need about 2.6 dB; no real converter
rings as much as the ideal sinc, and no real programme contains such bursts, so 1.5 dB was
kept. Alternatives tried and rejected: scaling the detector up to 0.47·fs (β = 4) reads a
plain 3 kHz tone 0.9 dB high; 48–64-tap detectors still read bursts 1.0–1.3 dB low and cost
8–16 samples of latency.

The 4× true-peak detectors used in loudness meters under-read a pure tone near Nyquist by up to
0.55 dB because the peak falls between the oversampled points; 8× plus the parabola brings that
to about 0.002 dB at 0.45·fs.

## 6. Re-quantizer

After the ceiling gain, last in the chain. Nothing is touched when Quantize is None.

```
w[n] = x[n] − Σ_k h[k] · e[n−k]        k = 1..N, the shaper taps (none when Shaping is None)
y[n] = round(w[n] + d[n])              to the nearest LSB, then saturated at −FS .. +FS − 1 LSB
e[n] = y[n] − w[n]                     the total error, dither included
```

so `y = x + (1 − H(z)) · e`: the dither sits inside the loop and the whole error is shaped,
which is the Lipshitz / Vanderkooy / Wannamaker (1991) structure. With dither the error is
white and signal-independent, so the shaped noise spectrum is `|NTF|²` times a flat floor.
Saturation never wraps. Output words are exact multiples of 1 LSB at 8, 12, 16, 20 and 24 bits.

**Dither**

| Setting | Noise | Total error with rounding |
|---|---|---|
| None | — | rounding only: distortion at low level |
| Type 1 | TPDF, 2 LSB peak to peak (two independent uniform ±0.5 LSB) | ¼ LSB² (−6.02 dB re LSB²), measured −6.01 |
| Type 2 | RPDF, 1 LSB peak to peak | ⅙ LSB² (−7.78 dB), measured −7.76 |

So Type 2 is 1.76 dB quieter than Type 1 in total error (3 dB in dither alone). Type 2 is "black" on silence: after 100 ms of
exact digital zero at its input the dither is faded out over 10 ms (and, in fact, RPDF at
±0.5 LSB on a zero input already rounds to zero by itself; with shaping the error recursion
decays within about 10 ms because all shaper zeros are inside radius 0.95). Measured: Type 2
output is exact zero 0 ms (no shaping) / 10 ms (Ultra) after the input goes silent; Type 1 keeps
dithering. Each channel has its own xorshift64\* generator with a fixed seed; `reset()` reseeds,
so a render is bit-identical every time.

**Noise shaping.** Error-feedback FIR, designed offline and stored in
`Source/dsp/ShaperCoefficients.h` (`NTF(z) = 1 − Σ h[k] z^−k`). The objective is the noise power
weighted by the inverse of Terhardt's threshold in quiet,
`T(f) = 3.64 f^−0.8 − 6.5 e^(−0.6 (f − 3.3)²) + 10^−3 f⁴` dB (f in kHz), `W = 10^(−T/10)`;
the reduction is `10 log10(∫W / ∫|NTF|² W)` against a flat floor. For each setting the tool
finds, by bisection, the *smallest* cap on `max |NTF|²` at which the target reduction (6 / 8.5 /
10.5 dB, the figures chosen for the three settings) is reachable at every rate of the family, so the shapers are as
mild as the targets allow — deliberately short of the 9-tap F-weighted designs in the
literature, so the shaped noise stays gentle and tolerates cascading. Every filter is
minimum-phase with all zeros inside radius 0.95 (Schur–Cohn test), so by Gerzon–Craven the
unweighted noise is the least possible for the shape and the loop goes quiet on silence.

44.1 / 48 kHz, 9 taps (`h[1..9]`):

| Setting | taps | reduction 44.1 k / 48 k | max boost | unweighted noise gain |
|---|---|---|---|---|
| Moderate | 0.6655, −0.0224, −0.2222, −0.0966, 0.0027, −0.0356, −0.0890, −0.0697, −0.0204 | 6.04 / 6.43 dB | 3.87 dB | 1.81 dB |
| Normal | 1.0774, −0.3395, −0.2977, 0.0907, 0.0751, −0.1012, −0.0993, −0.0085, 0.0143 | 8.50 / 9.20 dB | 6.56 dB | 3.80 dB |
| Ultra | 1.4203, −0.8066, −0.1459, 0.3120, −0.0833, −0.1674, 0.0054, 0.0370, −0.0403 | 10.54 / 11.39 dB | 9.26 dB | 5.83 dB |

Measured on the running re-quantizer (16-bit, Type 1, error spectrum by 8192-point FFT,
Terhardt-weighted): 6.01 / 8.45 / 10.50 dB at 44.1 kHz and 6.43 / 9.20 / 11.39 dB at 48 kHz, peak
spectral boost 5.9 / 8.5 / 10.9 dB at 17–18 kHz (the boost of the *shaped noise against the flat
floor at that bin*, which includes the Hann leakage; the design figure is the NTF maximum).

88.2 / 96 kHz, 9 taps: reduction 6.18 / 8.59 / 10.58 dB (88.2 k), 6.58 / 9.22 / 11.30 dB (96 k);
max boost 1.60 / 2.30 / 3.12 dB; unweighted 0.71 / 1.32 / 1.89 dB.
176.4 / 192 kHz, 9 taps: reduction 6.29 / 8.66 / 10.67 dB (176.4 k), 6.73 / 9.23 / 11.16 dB
(192 k); max boost 0.86 / 1.41 / 1.72 dB; unweighted 0.40 / 0.69 / 0.89 dB.
Below 44.1 kHz (designed at 32 kHz), 2 taps at fixed caps of 4 / 6 / 8 dB: reduction 2.61 / 3.90 /
5.06 dB — smaller, because less of the spectrum lies above 15 kHz. The full coefficient sets, with the
figures, are in the generated header.

Shaping without dither works too: the error feedback turns the rounding
distortion of low-level signals into noise. At 8 bits with Normal shaping and no dither a
−40 dBFS sine has its harmonics 2–10 at −33 dB re the sine instead of −13 dB truncated, and the
Terhardt-weighted error is 10.5 dB lower; at −50 dBFS (0.4 LSB) truncation outputs nothing at all
while the shaped output still carries the tone at −52.5 dBFS. Below about 0.13 LSB
(−60 dBFS at 8 bits) nothing comes through either way without dither.

The loop is bounded by construction (|e| ≤ 0.5 LSB + dither); measured over 60 s of full-scale
noise and 60 s of silence the largest error is 4.4 LSB and the output stays finite.

## 7. Realtime behaviour

No allocations or locks in `process()`; all buffers are sized in `prepare()`. The recursive
states (envelope, sustain average, gain glides) are snapped to their targets when within 1e-12
(or 1e-6 dB), and the shaper state decays to zero in long silence, so nothing lingers at
denormal level. Measured: 30 s stereo at 48 kHz takes about 0.14 s for a steady tone and 0.11 s for a
tail decaying into float denormals and silence (about 270× realtime, True Peak + Auto on).
Two instances, different block sizes and a reset instance give bit-identical output.

## 8. Provenance and limits

Chosen or measured for this plug-in (nothing here was taken from a measurement of any other
product): the 1.5 ms look-ahead and its two-moving-average realisation; the 16-sample detector
delay in every domain; Release as a dB time constant; the Auto law (20 ms per dB of sustained
reduction over a 300 ms average, 500 ms cap); the 8x-with-parabola true-peak detector and its
0.45·fs normalisation; the 1.5 dB Analog margin; TPDF / RPDF as the two dither types and the
1.76 dB difference that follows; the 100 ms / 10 ms silence gate; the shaper design method
(Terhardt weighting, minimum cap for the target, 9 taps, zeros inside 0.95) and every
coefficient; the 20 ms parameter glides; the −30 dB floor of the input faders.

Limits to know:

* The True Peak ceiling holds for content below 0.45 × the sample rate. Full-band content
  (Nyquist bursts, naive squares) can reconstruct up to +1.15 dB over the ceiling in the worst
  test case, depending on the converter's filter.
* The Analog domain's 1.5 dB margin is a margin chosen for this plug-in. It is bounded by the
  probes in section 5; bursts of 64 samples or more at exactly Nyquist would need about 2.6 dB.
* Latency is fixed at 82 / 88 / 160 / 304 samples at 44.1 / 48 / 96 / 192 kHz, whatever the
  settings.
* Sample rates below 44.1 kHz use a shorter shaper (2 taps) with a smaller noise reduction.
