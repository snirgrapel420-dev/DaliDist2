/*
  ==============================================================================
    DaliDist — Dali Audio
    Analog Color Box for Goa / Acid / Trance
  ==============================================================================
*/
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DaliDistDSP.h"

namespace ParamIDs
{
    inline constexpr const char* drive     = "drive";
    inline constexpr const char* color     = "color";
    inline constexpr const char* mode      = "mode";
    inline constexpr const char* talk      = "talk";
    inline constexpr const char* bite      = "bite";
    inline constexpr const char* subGuard  = "subguard";
    inline constexpr const char* drift     = "drift";
    inline constexpr const char* mix       = "mix";
    inline constexpr const char* output    = "output";
    inline constexpr const char* autoGain  = "autogain";
    inline constexpr const char* bypass    = "bypass";
}

class DaliDistAudioProcessor : public juce::AudioProcessor
{
public:
    DaliDistAudioProcessor();
    ~DaliDistAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    // --- Meters for the editor (written on the audio thread, read by the UI)
    std::atomic<float> meterInDb       { -100.0f };
    std::atomic<float> meterOutDb      { -100.0f };
    std::atomic<float> meterColorDb    { -100.0f };   // generated harmonics relative to input
    std::atomic<float> meterAutoGainDb { 0.0f };
    std::atomic<float> meterTalkHz     { 1000.0f };   // resonance the TALK circuit is following
    std::atomic<float> meterBite       { 0.0f };      // transient activity 0..~4

private:
    static constexpr int kOversamplingOrder = 2;      // 2^2 = 4x
    static constexpr int kMaxChannels = 2;

    dali::EngineParams readEngineParams() const;
    void processChunk (juce::AudioBuffer<float>& buffer, int start, int len);
    void delayDryInPlace (juce::AudioBuffer<float>& buffer);

    // Parameter pointers
    std::atomic<float>* pDrive = nullptr; std::atomic<float>* pColor = nullptr;
    std::atomic<float>* pMode = nullptr;  std::atomic<float>* pTalk = nullptr;
    std::atomic<float>* pBite = nullptr;  std::atomic<float>* pSubGuard = nullptr;
    std::atomic<float>* pDrift = nullptr; std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pOutput = nullptr; std::atomic<float>* pAutoGain = nullptr;
    std::atomic<float>* pBypass = nullptr;

    // DSP
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    dali::ColorEngine engine;
    juce::AudioBuffer<float> colorBuffer;
    int numChannels = 2, maxBlock = 512, latency = 0;

    // Sample-exact integer delay for the dry path (matches oversampling latency)
    std::array<std::vector<float>, kMaxChannels> dryRing;
    int ringWrite = 0;

    // LR4 all-pass on the dry (LP + HP at SUB GUARD) so dry and wet are phase-coherent for the mix
    std::array<dali::SVF, kMaxChannels> apL1, apL2, apH1, apH2;
    float subSmoothed = 90.0f;

    // Auto gain: loudness-weighted, slow, gated
    std::array<dali::OnePole, kMaxChannels> kwDryA, kwDryB, kwWetA, kwWetB;
    float msDry = 0.0f, msWet = 0.0f, msCoef = 0.0f;
    float agGain = 1.0f, agCoef = 0.0f;

    juce::SmoothedValue<float> mixSm, outSm, bypassSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DaliDistAudioProcessor)
};
