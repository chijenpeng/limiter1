#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace limiter1;

namespace
{
    const juce::Identifier versionId ("version");
    constexpr int kStateVersion = 1;

    /** Holds the largest value seen since the editor last read it (the editor exchanges it to 0). */
    void storeMax (std::atomic<float>& target, float value) noexcept
    {
        auto current = target.load (std::memory_order_relaxed);
        while (value > current && ! target.compare_exchange_weak (current, value, std::memory_order_relaxed)) {}
    }

    juce::AudioParameterFloatAttributes dbAttributes()
    {
        return juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (
            [] (float v, int) { return juce::String (v, 1) + " dB"; });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout Limiter1Processor::createLayout()
{
    using Range = juce::NormalisableRange<float>;
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Bool = juce::AudioParameterBool;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const Range gainRange (Limiter::kMinGainDb, 0.0f, 0.1f);

    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::inputL, 1 }, "Input L", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::inputR, 1 }, "Input R", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::threshold, 1 }, "Threshold", gainRange, 0.0f, dbAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::ceiling, 1 }, "Out Ceiling", gainRange, 0.0f, dbAttributes()));

    // Release: five decades, 0.01 ms to 1000 ms, logarithmic.
    Range releaseRange (0.01f, 1000.0f,
                        [] (float, float, float x) { return 0.01f * std::pow (10.0f, 5.0f * juce::jlimit (0.0f, 1.0f, x)); },
                        [] (float, float, float v) { return std::log10 (juce::jlimit (0.01f, 1000.0f, v) / 0.01f) / 5.0f; },
                        [] (float, float, float v) { return juce::jlimit (0.01f, 1000.0f, v); });
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::release, 1 }, "Release", releaseRange, 100.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (
                                             [] (float v, int) { return juce::String (v, v < 1.0f ? 2 : v < 100.0f ? 1 : 0) + " ms"; })));
    layout.add (std::make_unique<Bool> (juce::ParameterID { ParamID::autoRelease, 1 }, "Auto Release", false));
    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::domain, 1 }, "Domain", juce::StringArray { "Digital", "Analog", "True Peak" }, 0));
    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::quantize, 1 }, "Quantize", juce::StringArray { "None", "8 bit", "12 bit", "16 bit", "20 bit", "24 bit" }, 5));
    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::dither, 1 }, "Dither", juce::StringArray { "None", "Type 1", "Type 2" }, 1));
    layout.add (std::make_unique<Choice> (juce::ParameterID { ParamID::shaping, 1 }, "Shaping", juce::StringArray { "None", "Moderate", "Normal", "Ultra" }, 2));
    layout.add (std::make_unique<Bool> (juce::ParameterID { ParamID::link, 1 }, "Link Threshold and Ceiling", true));
    layout.add (std::make_unique<Bool> (juce::ParameterID { ParamID::inputLink, 1 }, "Link Inputs", true));
    return layout;
}

Limiter1Processor::Limiter1Processor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "OneLimiter", createLayout())
{
    link.setRange (Limiter::kMinGainDb, 0.0f);
    resetLink();
}

void Limiter1Processor::resetLink() noexcept
{
    link.reset (param (ParamID::threshold), param (ParamID::ceiling), param (ParamID::link) > 0.5f);
}

bool Limiter1Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

void Limiter1Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    limiter.prepare (sampleRate, samplesPerBlock);
    resetLink();
    setLatencySamples (limiter.latencySamples());
}

void Limiter1Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels(), 2);
    for (int ch = numChannels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (linkResetPending.exchange (false))
        resetLink();
    const auto linked = link.update (param (ParamID::threshold), param (ParamID::ceiling), param (ParamID::link) > 0.5f);
    if (linked.writeThreshold)
        pendingThreshold.store (linked.thresholdDb);
    if (linked.writeCeiling)
        pendingCeiling.store (linked.ceilingDb);
    if (linked.writeThreshold || linked.writeCeiling)
        triggerAsyncUpdate();
    effectiveThresholdDb.store (linked.thresholdDb);
    effectiveCeilingDb.store (linked.ceilingDb);

    Params p;
    p.inputGainDbL = param (ParamID::inputL);
    p.inputGainDbR = param (ParamID::inputR);
    p.thresholdDb = linked.thresholdDb;
    p.ceilingDb = linked.ceilingDb;
    p.releaseMs = param (ParamID::release);
    p.autoRelease = param (ParamID::autoRelease) > 0.5f;
    p.domain = (Domain) choice (ParamID::domain, 2);
    p.quantize = (Quantize) choice (ParamID::quantize, 5);
    p.dither = (Dither) choice (ParamID::dither, 2);
    p.shaping = (Shaping) choice (ParamID::shaping, 3);
    limiter.setParams (p);

    limiter.process (buffer.getArrayOfWritePointers(), numChannels, buffer.getNumSamples());

    const auto& m = limiter.meters();
    storeMax (inputPeakL, m.inputPeak[0]);
    storeMax (inputPeakR, numChannels > 1 ? m.inputPeak[1] : m.inputPeak[0]);
    storeMax (outputPeakL, m.outputPeak[0]);
    storeMax (outputPeakR, numChannels > 1 ? m.outputPeak[1] : m.outputPeak[0]);
    storeMax (gainReductionDb, m.gainReductionDb);
}

void Limiter1Processor::writeBack (const char* id, std::atomic<float>& pending)
{
    const float value = pending.exchange (NAN);
    if (auto* parameter = apvts.getParameter (id); parameter != nullptr && ! std::isnan (value))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void Limiter1Processor::handleAsyncUpdate()
{
    // The Link coupling moved a control: tell the host and the UI (message thread).
    writeBack (ParamID::threshold, pendingThreshold);
    writeBack (ParamID::ceiling, pendingCeiling);
}

void Limiter1Processor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (versionId, kStateVersion, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void Limiter1Processor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    linkResetPending.store (true);   // a restored pair is the new baseline for Link: nothing moves on load
}

juce::AudioProcessorEditor* Limiter1Processor::createEditor()
{
    return new Limiter1Editor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Limiter1Processor();
}
