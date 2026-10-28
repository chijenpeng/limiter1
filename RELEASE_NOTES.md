# Limiter1 release notes

## v0.1.1

- Meters: the Attenuation meter and the input/output peak meters now show every peak and reduction
  between two screen refreshes; before, a short event between refreshes could be missed by the
  display. Audio is unchanged.
- Link between Threshold and Out Ceiling is now on by default for new instances; saved sessions
  keep their setting.
- Checked before release: CTest 1/1 (limiter_test passed); editor snapshot rendered to
  docs/editor.png and checked; AU and VST3 slices arm64 + x86_64 (lipo).
- Windows x64: VST3 built natively with MSVC on windows-2022; CTest 1/1 (limiter_test, 0 failures) passed there. Requires the Microsoft Visual C++ x64 Redistributable.

## v0.1.0

Limiter1 is a look-ahead brickwall peak limiter with an output re-quantizer: Digital, Analog and
True Peak ceilings, Auto Release, TPDF/RPDF dither and three noise-shaping settings. It is an
independent plug-in identity (`com.c0dypeng.limiter1`, `Cdpg` / `Lim1`).

- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built natively with MSVC on windows-2022; CTest 1/1 (limiter_test, 0 failures)
  passed there. Requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- Checked before release: CTest 1/1 (limiter_test: 39 checks, 0 failures); editor snapshot rendered; AU and VST3 slices arm64 + x86_64; ad-hoc signatures, ZIP integrity and SHA-256 verified by the packaging script.

Known limits: the True Peak ceiling holds for content below 0.45 × the sample rate; full-band
content (Nyquist bursts) can reconstruct up to 1.15 dB over it in the worst test case. The Analog
domain's 1.5 dB margin is chosen for this plug-in and bounded by the documented probes. Latency
is fixed at 82 / 88 / 160 / 304 samples at 44.1 / 48 / 96 / 192 kHz and is reported to the host.

Each ZIP includes installation instructions, dependency notices and build provenance, with a
SHA-256 checksum companion. No DAW-host test has been run.
