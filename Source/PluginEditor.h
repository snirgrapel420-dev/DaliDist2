/*
  ==============================================================================
    DaliDist v3 — Editor (multiband)
  ==============================================================================
*/
#pragma once

#include "PluginProcessor.h"
#include "DaliLookAndFeel.h"

//==============================================================================
/** Rotary knob + title + optional live caption. */
class DaliKnob : public juce::Component
{
public:
    DaliKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID,
              const juce::String& titleText, bool hero = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    void setCaption (const juce::String& c) { if (c != caption) { caption = c; repaint(); } }
    void setGlow (float g);

    juce::Slider slider;

private:
    juce::String title, caption;
    bool isHero;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
class NeonMeter : public juce::Component
{
public:
    explicit NeonMeter (juce::String labelText) : label (std::move (labelText)) {}
    void setLevelDb (float db);
    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float shown = -60.0f, peak = -60.0f;
    int peakHold = 0;
};

//==============================================================================
/** Frequency map: the three bands, the clean sub zone, draggable crossovers and sub guard. */
class BandMap : public juce::Component
{
public:
    explicit BandMap (DaliDistAudioProcessor&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    void setGlows (const std::array<float, dali::kNumBands>& g) { glows = g; repaint(); }

private:
    float freqToX (float f) const;
    float xToFreq (float x) const;
    int handleAt (float x) const;       // 0 = sub guard, 1 = low|mid, 2 = mid|high, -1 none
    juce::RangedAudioParameter* paramFor (int handle) const;

    DaliDistAudioProcessor& proc;
    std::array<float, dali::kNumBands> glows {};
    int dragging = -1, hover = -1;
};

//==============================================================================
/** One band: Drive (hero), Level, ON / SOLO and the five circuits. */
class BandPanel : public juce::Component
{
public:
    BandPanel (DaliDistAudioProcessor&, int bandIndex);

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh (float glowTarget);    // called from the editor timer

private:
    void selectMode (int m);

    DaliDistAudioProcessor& proc;
    const int band;
    DaliKnob drive, level;
    juce::TextButton onButton { "On" }, soloButton { "Solo" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAtt, soloAtt;
    juce::OwnedArray<juce::TextButton> modeButtons;
    float glow = 0.0f;
    bool active = true, soloed = false;
};

//==============================================================================
class DaliDistAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit DaliDistAudioProcessorEditor (DaliDistAudioProcessor&);
    ~DaliDistAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    DaliDistAudioProcessor& proc;
    DaliLookAndFeel lnf;

    BandMap bandMap;
    std::unique_ptr<BandPanel> bands[dali::kNumBands];

    DaliKnob master, color, talk, bite, subGuard, drift, mix, output;
    NeonMeter inMeter { "In" }, outMeter { "Out" };

    juce::TextButton autoGainButton { "Auto gain" }, bypassButton { "Bypass" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoGainAtt, bypassAtt;

    float colorDbShown = -60.0f, biteGlow = 0.0f;
    std::array<float, dali::kNumBands> bandGlow {};
    juce::Rectangle<int> statusArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DaliDistAudioProcessorEditor)
};
