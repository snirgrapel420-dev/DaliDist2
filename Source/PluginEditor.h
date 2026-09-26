/*
  ==============================================================================
    DaliDist — Editor
  ==============================================================================
*/
#pragma once

#include "PluginProcessor.h"
#include "DaliLookAndFeel.h"

//==============================================================================
/** Rotary knob + title + optional live caption (e.g. the frequency TALK is following). */
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
    void selectMode (int index);

    DaliDistAudioProcessor& proc;
    DaliLookAndFeel lnf;

    DaliKnob drive, color, bite, talk, subGuard, drift, mix, output;
    NeonMeter inMeter { "In" }, outMeter { "Out" };

    juce::OwnedArray<juce::TextButton> modeButtons;
    juce::TextButton autoGainButton { "Auto gain" }, bypassButton { "Bypass" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoGainAtt, bypassAtt;

    float colorGlow = 0.0f, biteGlow = 0.0f, colorDbShown = -60.0f;
    juce::Rectangle<int> heroArea, statusArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DaliDistAudioProcessorEditor)
};
