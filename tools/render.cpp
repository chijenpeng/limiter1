// Renders a WAV file through the plug-in's exact processor path.
//
//   limiter1_render in.wav out.wav [--input-l dB] [--input-r dB] [--threshold dB] [--ceiling dB]
//                     [--release ms] [--auto 0|1] [--domain digital|analog|tp]
//                     [--quantize none|8|12|16|20|24] [--dither none|1|2]
//                     [--shaping none|moderate|normal|ultra] [--block n] [--out-bits 16|24|32]
//                     [--keep-latency]
//
// The output is time-aligned with the input (the look-ahead latency is removed) unless
// --keep-latency is given. 32-bit output is float and keeps the quantized words exactly.

#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace
{
int usage()
{
    std::fprintf (stderr, "usage: limiter1_render in.wav out.wav [--input-l dB] [--input-r dB] [--threshold dB] [--ceiling dB]\n"
                          "       [--release ms] [--auto 0|1] [--domain digital|analog|tp] [--quantize none|8|12|16|20|24]\n"
                          "       [--dither none|1|2] [--shaping none|moderate|normal|ultra] [--block n] [--out-bits 16|24|32] [--keep-latency]\n");
    return 2;
}

int choiceIndex (const juce::String& value, const juce::StringArray& names, const juce::StringArray& aliases)
{
    for (int i = 0; i < names.size(); ++i)
        if (value.equalsIgnoreCase (names[i]) || (i < aliases.size() && value.equalsIgnoreCase (aliases[i])))
            return i;
    return -1;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
        return usage();
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::map<std::string, std::string> options;
    for (int i = 3; i < argc; ++i)
    {
        std::string key = argv[i];
        if (key.rfind ("--", 0) != 0)
            return usage();
        key = key.substr (2);
        if (key == "keep-latency")
        {
            options[key] = "1";
            continue;
        }
        if (i + 1 >= argc)
            return usage();
        options[key] = argv[++i];
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File inFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1]));
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inFile));
    if (reader == nullptr)
    {
        std::fprintf (stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const int numChannels = juce::jlimit (1, 2, (int) reader->numChannels);
    const double fs = reader->sampleRate;
    const int length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> audio (numChannels, length);
    reader->read (&audio, 0, length, 0, true, numChannels > 1);
    if (reader->numChannels > 2)
        std::fprintf (stderr, "note: only the first two channels are used\n");

    Limiter1Processor processor;
    juce::AudioProcessor::BusesLayout layout;
    const auto set = numChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    layout.inputBuses.add (set);
    layout.outputBuses.add (set);
    if (! processor.setBusesLayout (layout))
    {
        std::fprintf (stderr, "unsupported channel layout\n");
        return 1;
    }

    auto setFloat = [&] (const char* id, const std::string& key)
    {
        if (options.count (key))
            if (auto* p = processor.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (std::stof (options[key])));
    };
    auto setChoice = [&] (const char* id, const std::string& key, const juce::StringArray& names, const juce::StringArray& aliases)
    {
        if (! options.count (key))
            return true;
        const int index = choiceIndex (options[key], names, aliases);
        if (index < 0)
        {
            std::fprintf (stderr, "bad value for --%s: %s\n", key.c_str(), options[key].c_str());
            return false;
        }
        if (auto* p = processor.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
        return true;
    };
    setFloat (ParamID::inputL, "input-l");
    setFloat (ParamID::inputR, "input-r");
    setFloat (ParamID::threshold, "threshold");
    setFloat (ParamID::ceiling, "ceiling");
    setFloat (ParamID::release, "release");
    if (! setChoice (ParamID::autoRelease, "auto", { "0", "1" }, { "off", "on" })
        || ! setChoice (ParamID::domain, "domain", { "digital", "analog", "tp" }, { "d", "a", "truepeak" })
        || ! setChoice (ParamID::quantize, "quantize", { "none", "8", "12", "16", "20", "24" }, {})
        || ! setChoice (ParamID::dither, "dither", { "none", "1", "2" }, { "0", "type1", "type2" })
        || ! setChoice (ParamID::shaping, "shaping", { "none", "moderate", "normal", "ultra" }, {}))
        return 2;

    const int block = options.count ("block") ? juce::jmax (1, std::stoi (options["block"])) : 512;
    const int outBits = options.count ("out-bits") ? std::stoi (options["out-bits"]) : 32;
    if (outBits != 16 && outBits != 24 && outBits != 32)
        return usage();
    const bool keepLatency = options.count ("keep-latency") > 0;

    processor.setRateAndBufferSizeDetails (fs, block);
    processor.prepareToPlay (fs, block);
    const int latency = processor.getLatencySamples();

    juce::AudioBuffer<float> out (numChannels, length + latency);
    out.clear();
    juce::AudioBuffer<float> work (numChannels, block);
    juce::MidiBuffer midi;
    float deepest = 0.0f;
    for (int start = 0; start < length + latency; start += block)
    {
        const int n = juce::jmin (block, length + latency - start);
        work.clear();
        for (int ch = 0; ch < numChannels; ++ch)
            if (start < length)
                work.copyFrom (ch, 0, audio, ch, start, juce::jmin (n, length - start));
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), numChannels, n);
        processor.processBlock (view, midi);
        deepest = juce::jmax (deepest, processor.gainReductionDb.load());
        for (int ch = 0; ch < numChannels; ++ch)
            out.copyFrom (ch, start, view, ch, 0, n);
    }

    const juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[2]));
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
    if (stream == nullptr)
    {
        std::fprintf (stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    const auto writerOptions = juce::AudioFormatWriterOptions()
                             .withSampleRate (fs)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (outBits)
                             .withSampleFormat (outBits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                              : juce::AudioFormatWriterOptions::SampleFormat::integral);
    auto writer = wav.createWriterFor (stream, writerOptions);
    if (writer == nullptr)
    {
        std::fprintf (stderr, "cannot create a %d-bit WAV writer\n", outBits);
        return 1;
    }
    const int offset = keepLatency ? 0 : latency;
    const int samples = keepLatency ? length + latency : length;
    if (outBits == 32)
        writer->writeFromAudioSampleBuffer (out, offset, samples);
    else
    {
        // Integer words are written as left-justified 32-bit values so that a quantized
        // output lands exactly (JUCE's float path scales by 2^(bits-1) - 1, which is off by
        // one LSB near full scale).
        juce::AudioBuffer<int> words (numChannels, samples);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < samples; ++i)
            {
                const double v = std::clamp ((double) out.getSample (ch, offset + i) * 2147483648.0, -2147483648.0, 2147483647.0);
                words.setSample (ch, i, (int) std::llround (v));
            }
        writer->write (const_cast<const int**> (words.getArrayOfWritePointers()), samples);
    }
    std::printf ("%s -> %s: %d ch, %.0f Hz, %d samples, latency %d samples (%s), deepest gain reduction %.2f dB\n",
                 argv[1], argv[2], numChannels, fs, samples, latency, keepLatency ? "kept" : "removed", deepest);
    return 0;
}
