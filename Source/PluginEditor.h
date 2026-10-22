#pragma once

#include "PluginProcessor.h"

/** The embedded typefaces. Shared by all open editors and released with the last one, so
    nothing outlives the host's JUCE shutdown. */
struct LimiterFonts
{
    LimiterFonts();
    juce::Typeface::Ptr inter, mono;
};

/** Flat drawing for the one rotary knob; everything else paints itself. */
class LimiterLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

/** The dB scale shared by the faders and the meters: -30 at the bottom, 0 at the top. A
    component's "body" is its bounds minus the value row underneath; the handle centres and
    meter tips travel inside the body, inset by half a handle. */
namespace scale
{
    constexpr float kMinDb = -30.0f, kMaxDb = 0.0f;
    constexpr int kValueRow = 20;     // value text under a fader or meter, gap included
    constexpr int kTrackInset = 7;    // half the fader handle, so 0 dB and -30 dB sit at handle centres

    inline juce::Rectangle<int> bodyOf (juce::Rectangle<int> bounds) { return bounds.withTrimmedBottom (kValueRow); }
    inline juce::Rectangle<float> trackOf (juce::Rectangle<int> body) { return body.toFloat().reduced (0.0f, (float) kTrackInset); }
    inline float yForDb (float db, juce::Rectangle<int> body)
    {
        const auto track = trackOf (body);
        return juce::jmap (juce::jlimit (kMinDb, kMaxDb, db), kMinDb, kMaxDb, track.getBottom(), track.getY());
    }
}

/** A flat disc with a dot, its name above and its value below. Double-click the disc for the
    default; double-click the value to type one. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const char* paramId, const juce::String& name, const juce::String& help,
          int diameter, float sweepDegrees = 270.0f);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kLabelHeight = 16, kGap = 8, kCellWidth = 84;
    /** Label, disc and value. */
    int preferredHeight() const noexcept { return 2 * (kLabelHeight + kGap) + diameter; }
    int getDiameter() const noexcept { return diameter; }

private:
    void finishEditing (bool apply);

    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::String name;
    int diameter;
    juce::TextEditor editor;
    juce::SliderParameterAttachment attachment;
};

/** "LABEL  OPTION  OPTION ..." as plain text; the selected option is lit. */
class OptionRow final : public juce::Component
{
public:
    OptionRow (juce::RangedAudioParameter&, const juce::String& label, juce::StringArray options, const juce::String& help);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** The label's own width; rows stacked together share the widest plus the label gap. */
    int labelTextWidth() const;
    void setLabelColumn (int width) { labelColumn = width; repaint(); }
    int preferredWidth() const;

private:
    std::vector<juce::Range<int>> optionRanges() const;
    int optionAt (int x) const;

    juce::String label;
    juce::StringArray options;
    int labelColumn = 0, selected = 0, hovered = -1;
    juce::ParameterAttachment attachment;
};

/** One word, lit when its boolean parameter is on. */
class TextToggle final : public juce::Component
{
public:
    TextToggle (juce::RangedAudioParameter&, const juce::String& text, const juce::String& help);

    bool isOn() const noexcept { return on; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    juce::String text;
    bool on = false;
    juce::ParameterAttachment attachment;
};

/** A vertical dB fader on the shared scale: a thin track, a pale handle, the value underneath.
    Relative drag (Shift for fine), double-click resets, the wheel nudges. A partner fader can be
    dragged along, keeping the offset, while a link predicate is true. */
class Fader final : public juce::Component
{
public:
    Fader (juce::AudioProcessorValueTreeState&, const char* paramId, const juce::String& name, const juce::String& help);

    void linkTo (Fader* partner, std::function<bool()> isLinked);
    float getValueDb() const noexcept { return valueDb; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    static constexpr int kWidth = 36;

private:
    bool partnerLinked() const { return partner != nullptr && linked != nullptr && linked(); }
    /** Clamps a move so that this fader, and the partner if it moves too, stay in range. */
    float limitMove (float deltaDb, float fromDb, bool withPartner, float partnerFromDb) const;

    juce::RangedAudioParameter& param;
    float valueDb = 0.0f, dragStartDb = 0.0f, partnerStartDb = 0.0f;
    bool dragging = false, partnerDragging = false;
    Fader* partner = nullptr;
    std::function<bool()> linked;
    juce::ParameterAttachment attachment;
};

/** A thin peak meter on the shared scale, or a gain-reduction meter drawn downward from the top.
    Instant rise, steady fall, and a peak-hold line that stays 1.5 s. Updated by the editor's
    timer from the processor's block maxima; no ballistics are done on the audio thread. */
class Meter final : public juce::Component
{
public:
    enum class Kind { peak, reduction };

    Meter (Kind, std::function<float()> readDb, const juce::String& name, const juce::String& help);

    /** Reads the current value and advances the ballistics; repaints when anything moved. */
    void update();
    /** A thin line across the meter at this level (NaN for none): the ceiling or the threshold. */
    void setMarkerDb (float db);
    /** Shows the held level under the meter (the reduction meter does). */
    void setShowsValue (bool b) { showsValue = b; }
    /** A short caption under the meter instead ("L", "R"). */
    void setCaption (const juce::String& c) { caption = c; }

    void paint (juce::Graphics&) override;

    static constexpr int kWidth = 10;

private:
    Kind kind;
    std::function<float()> readDb;
    float shownDb = -100.0f, holdDb = -100.0f, markerDb = NAN;
    int holdFrames = 0;
    bool showsValue = false;
    juce::String caption;
};

/** The fixed-size design surface; the editor scales it to its window. */
class LimiterSurface final : public juce::Component,
                             private juce::Timer
{
public:
    explicit LimiterSurface (Limiter1Processor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** What the control under the mouse does; nullptr shows the default text. */
    void showHelpFor (juce::Component*);

    static constexpr int kWidth = 780, kHeight = 380;

private:
    void timerCallback() override;

    Limiter1Processor& processor;
    juce::SharedResourcePointer<LimiterFonts> fonts;

    OptionRow domain;
    Fader inputL, inputR, threshold, ceiling;
    Meter inMeterL, inMeterR, reduction, outMeterL, outMeterR;
    TextToggle inputLink, link;
    Knob release;
    OptionRow autoRelease, quantize, dither, shaping;

    juce::Rectangle<int> helpArea, scaleBody, inputTitle, thresholdTitle, ceilingTitle;
    int leftScaleX = 0, rightScaleX = 0;
    juce::String helpTitle, helpText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterSurface)
};

class Limiter1Editor final : public juce::AudioProcessorEditor
{
public:
    explicit Limiter1Editor (Limiter1Processor&);
    ~Limiter1Editor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Limiter1Processor& owner;
    juce::SharedResourcePointer<LimiterFonts> fonts;
    LimiterLookAndFeel lookAndFeel;
    LimiterSurface surface;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Limiter1Editor)
};
