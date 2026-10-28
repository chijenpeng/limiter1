#pragma once

#include "dsp/Limiter.h"
#include "dsp/LinkedControls.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

/** Parameter IDs. Stable: never reorder or rename, sessions depend on them. */
namespace ParamID
{
    inline constexpr auto inputL = "input_l", inputR = "input_r", threshold = "threshold", ceiling = "ceiling",
                          release = "release", autoRelease = "auto_release", domain = "domain", quantize = "quantize",
                          dither = "dither", shaping = "shaping", link = "link", inputLink = "input_link";
}

class Limiter1Processor final : public juce::AudioProcessor,
                                  private juce::AsyncUpdater
{
public:
    Limiter1Processor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // For the UI
    juce::AudioProcessorValueTreeState apvts;

    /** Maxima since the editor last read them, linear (0..1) and dB of reduction; raw, no ballistics.
        The processor only raises them (storeMax); the editor exchanges them to 0 on each read. */
    std::atomic<float> inputPeakL { 0.0f }, inputPeakR { 0.0f }, outputPeakL { 0.0f }, outputPeakR { 0.0f }, gainReductionDb { 0.0f };
    /** What the limiter is actually using after the Link coupling (equal to the parameters
        except for the block or two before a coupled write-back lands). */
    std::atomic<float> effectiveThresholdDb { 0.0f }, effectiveCeilingDb { 0.0f };

    const limiter1::Limiter& getLimiter() const noexcept { return limiter; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleAsyncUpdate() override;
    float param (const char* id) const noexcept { return apvts.getRawParameterValue (id)->load(); }
    int choice (const char* id, int lastIndex) const noexcept { return juce::jlimit (0, lastIndex, juce::roundToInt (param (id))); }
    /** The current Threshold / Ceiling pair becomes Link's baseline: nothing moves. */
    void resetLink() noexcept;
    void writeBack (const char* id, std::atomic<float>& pending);

    limiter1::Limiter limiter;
    limiter1::LinkedControls link;
    std::atomic<float> pendingThreshold { NAN }, pendingCeiling { NAN };
    std::atomic<bool> linkResetPending { false };   // a restored state re-baselines Link, on the audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Limiter1Processor)
};
