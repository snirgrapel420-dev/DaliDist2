/*
  ==============================================================================
    DaliDist — Dali Audio

    Signal flow
    -----------
      in ──┬─► [integer delay = OS latency] ─┬──────────────────────────────────────► bypass (raw, exact)
           │                                 └─► LR4 all-pass (sub guard) = A ──┬──► mix ─► out trim ─► out
           └─► 4x up ─► ColorEngine (color only) ─► 4x down ─► A + color ─► auto gain = wet ┘

    wet = A + color = LP(dry) + Chain(HP(dry)): the sub below SUB GUARD is never colored,
    and because dry and wet share the same all-pass, the parallel mix is phase-coherent.
  ==============================================================================
*/
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::String colorName (float v)
    {
        if (v < 25.0f) return "Warm";
        if (v < 50.0f) return "Rich";
        if (v < 75.0f) return "Open";
        return "Excited";
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout DaliDistAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pct = [] (float v, int) { return String (juce::roundToInt (v)) + "%"; };

    // Drive: skewed so the 5–20 % sweet spot gets most of the knob travel
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::drive, 1 }, "Drive",
        NormalisableRange<float> (0.0f, 100.0f, 0.01f, 0.55f), 15.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::color, 1 }, "Color",
        NormalisableRange<float> (0.0f, 100.0f, 0.01f), 40.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return colorName (v); })));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::mode, 1 }, "Mode",
        StringArray { "Tube", "Transformer", "Tape", "Console", "Acid" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::talk, 1 }, "Talk",
        NormalisableRange<float> (0.0f, 100.0f, 0.01f), 30.0f, AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::bite, 1 }, "Bite",
        NormalisableRange<float> (0.0f, 100.0f, 0.01f), 25.0f, AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    NormalisableRange<float> subRange (30.0f, 300.0f, 1.0f); subRange.setSkewForCentre (90.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::subGuard, 1 }, "Sub Guard",
        subRange, 90.0f, AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v)) + " Hz"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::drift, 1 }, "Drift",
        NormalisableRange<float> (0.0f, 100.0f, 0.01f), 30.0f, AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::mix, 1 }, "Mix",
        NormalisableRange<float> (0.0f, 100.0f, 0.01f), 100.0f, AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::output, 1 }, "Output",
        NormalisableRange<float> (-18.0f, 18.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::autoGain, 1 }, "Auto Gain", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::bypass, 1 }, "Bypass", false));

    return layout;
}

//==============================================================================
DaliDistAudioProcessor::DaliDistAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "DaliDistState", createParameterLayout())
{
    pDrive     = apvts.getRawParameterValue (ParamIDs::drive);
    pColor     = apvts.getRawParameterValue (ParamIDs::color);
    pMode      = apvts.getRawParameterValue (ParamIDs::mode);
    pTalk      = apvts.getRawParameterValue (ParamIDs::talk);
    pBite      = apvts.getRawParameterValue (ParamIDs::bite);
    pSubGuard  = apvts.getRawParameterValue (ParamIDs::subGuard);
    pDrift     = apvts.getRawParameterValue (ParamIDs::drift);
    pMix       = apvts.getRawParameterValue (ParamIDs::mix);
    pOutput    = apvts.getRawParameterValue (ParamIDs::output);
    pAutoGain  = apvts.getRawParameterValue (ParamIDs::autoGain);
    pBypass    = apvts.getRawParameterValue (ParamIDs::bypass);
}

bool DaliDistAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

juce::AudioProcessorParameter* DaliDistAudioProcessor::getBypassParameter() const
{
    return apvts.getParameter (ParamIDs::bypass);
}

dali::EngineParams DaliDistAudioProcessor::readEngineParams() const
{
    dali::EngineParams p;
    p.mode     = (dali::Mode) juce::jlimit (0, (int) dali::Mode::NumModes - 1, (int) pMode->load());
    p.drive    = pDrive->load() * 0.01f;
    p.color    = pColor->load() * 0.01f;
    p.talk     = pTalk->load() * 0.01f;
    p.bite     = pBite->load() * 0.01f;
    p.subGuard = pSubGuard->load();
    p.drift    = pDrift->load() * 0.01f;
    return p;
}

//==============================================================================
void DaliDistAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    numChannels = juce::jlimit (1, kMaxChannels, getTotalNumOutputChannels());
    maxBlock    = juce::jmax (1, samplesPerBlock);

    // Linear-phase FIR half-band oversampling with integer latency -> exact dry alignment
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) numChannels, (size_t) kOversamplingOrder,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    oversampler->initProcessing ((size_t) maxBlock);
    latency = (int) std::lround (oversampler->getLatencyInSamples());
    setLatencySamples (latency);

    engine.setParams (readEngineParams());
    engine.prepare (sampleRate * (double) (1 << kOversamplingOrder), numChannels);
    engine.snapSmoothers();

    colorBuffer.setSize (numChannels, maxBlock, false, true, true);

    for (auto& r : dryRing) r.assign ((size_t) latency + 1, 0.0f);
    ringWrite = 0;

    const float fs = (float) sampleRate;
    subSmoothed = pSubGuard->load();
    for (int c = 0; c < kMaxChannels; ++c)
    {
        for (auto* f : { &apL1[(size_t) c], &apL2[(size_t) c], &apH1[(size_t) c], &apH2[(size_t) c] })
        {
            f->set (subSmoothed, fs, 0.70710678f);
            f->reset();
        }
    }
    for (int c = 0; c < kMaxChannels; ++c)
    {
        kwDryA[(size_t) c].setCutoff (60.0f, fs);   kwWetA[(size_t) c].setCutoff (60.0f, fs);
        kwDryB[(size_t) c].setCutoff (1500.0f, fs); kwWetB[(size_t) c].setCutoff (1500.0f, fs);
        kwDryA[(size_t) c].reset(); kwWetA[(size_t) c].reset(); kwDryB[(size_t) c].reset(); kwWetB[(size_t) c].reset();
    }
    msDry = msWet = 0.0f;
    msCoef = 1.0f - std::exp (-1.0f / (1.2f * fs));   // ~1.2 s loudness window (no pumping)
    agCoef = 1.0f - std::exp (-1.0f / (0.30f * fs));  // gain glide
    agGain = 1.0f;

    mixSm.reset (sampleRate, 0.05);    mixSm.setCurrentAndTargetValue (pMix->load() * 0.01f);
    outSm.reset (sampleRate, 0.05);    outSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));
    bypassSm.reset (sampleRate, 0.03); bypassSm.setCurrentAndTargetValue (pBypass->load() > 0.5f ? 1.0f : 0.0f);
}

//==============================================================================
void DaliDistAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    for (int c = numChannels; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);

    engine.setParams (readEngineParams());

    // All-pass tracks SUB GUARD with the same ~30 ms glide the engine uses
    const float sr = (float) getSampleRate();
    subSmoothed += (pSubGuard->load() - subSmoothed) * (1.0f - std::exp (-(float) n / (0.03f * sr)));
    for (int c = 0; c < numChannels; ++c)
        for (auto* f : { &apL1[(size_t) c], &apL2[(size_t) c], &apH1[(size_t) c], &apH2[(size_t) c] })
            f->set (subSmoothed, sr, 0.70710678f);

    mixSm.setTargetValue (pMix->load() * 0.01f);
    outSm.setTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));
    bypassSm.setTargetValue (pBypass->load() > 0.5f ? 1.0f : 0.0f);

    float inPeak = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        inPeak = juce::jmax (inPeak, buffer.getMagnitude (c, 0, n));

    for (int start = 0; start < n; start += maxBlock)
        processChunk (buffer, start, juce::jmin (maxBlock, n - start));

    float outPeak = 0.0f;
    for (int c = 0; c < numChannels; ++c)
        outPeak = juce::jmax (outPeak, buffer.getMagnitude (c, 0, n));

    meterInDb.store (juce::Decibels::gainToDecibels (inPeak, -100.0f));
    meterOutDb.store (juce::Decibels::gainToDecibels (outPeak, -100.0f));
    meterColorDb.store (juce::Decibels::gainToDecibels (engine.takeColorRatio(), -100.0f));
    meterAutoGainDb.store (juce::Decibels::gainToDecibels (agGain, -100.0f));
    meterTalkHz.store (engine.getTalkFrequency());
    meterBite.store (engine.getBiteActivity());
}

void DaliDistAudioProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int len)
{
    // 1) Color path: input -> 4x up -> engine (color only) -> 4x down
    for (int c = 0; c < numChannels; ++c)
        colorBuffer.copyFrom (c, 0, buffer, c, start, len);

    juce::dsp::AudioBlock<float> colorBlock (colorBuffer.getArrayOfWritePointers(), (size_t) numChannels, (size_t) len);
    auto osBlock = oversampler->processSamplesUp (colorBlock);

    float* osPtrs[kMaxChannels] = { nullptr, nullptr };
    for (int c = 0; c < numChannels; ++c)
        osPtrs[c] = osBlock.getChannelPointer ((size_t) c);
    engine.process (osPtrs, numChannels, (int) osBlock.getNumSamples());

    oversampler->processSamplesDown (colorBlock);

    // 2) Per sample: aligned dry, all-passed dry A, wet = A + color, auto gain, mix, output, bypass
    const bool autoGainOn = pAutoGain->load() > 0.5f;
    float* io[kMaxChannels]        = { nullptr, nullptr };
    const float* col[kMaxChannels] = { nullptr, nullptr };
    for (int c = 0; c < numChannels; ++c)
    {
        io[c]  = buffer.getWritePointer (c, start);
        col[c] = colorBuffer.getReadPointer (c);
    }

    const int ringSize = latency + 1;
    const float invCh = 1.0f / (float) numChannels;

    for (int i = 0; i < len; ++i)
    {
        const int readPos = (ringWrite + 1) % ringSize;   // == write - latency (mod ringSize)
        float dry[kMaxChannels] {}, ap[kMaxChannels] {}, wet[kMaxChannels] {};
        float pDry = 0.0f, pWet = 0.0f;

        for (int c = 0; c < numChannels; ++c)
        {
            auto& ring = dryRing[(size_t) c];
            ring[(size_t) ringWrite] = io[c][i];
            dry[c] = ring[(size_t) readPos];

            auto& l1 = apL1[(size_t) c]; auto& l2 = apL2[(size_t) c];
            auto& h1 = apH1[(size_t) c]; auto& h2 = apH2[(size_t) c];
            l1.tick (dry[c]); l2.tick (l1.lpOut);
            h1.tick (dry[c]); h2.tick (h1.hpOut);
            ap[c]  = l2.lpOut + h2.hpOut;
            wet[c] = ap[c] + col[c][i];

            // Simple loudness weighting (LF roll-off + presence lift) for level matching
            const float dA = kwDryA[(size_t) c].hp (ap[c]);
            const float dW = dA + 0.6f * kwDryB[(size_t) c].hp (dA);
            const float wA = kwWetA[(size_t) c].hp (wet[c]);
            const float wW = wA + 0.6f * kwWetB[(size_t) c].hp (wA);
            pDry += dW * dW; pWet += wW * wW;
        }
        ringWrite = readPos;

        msDry += msCoef * (pDry * invCh - msDry);
        msWet += msCoef * (pWet * invCh - msWet);

        float agTarget = agGain;                           // hold during silence (no pumping on tails)
        if (! autoGainOn)
            agTarget = 1.0f;
        else if (msDry > 1.0e-8f && msWet > 1.0e-10f)
            agTarget = juce::jlimit (0.063f, 4.0f, std::sqrt (msDry / msWet));   // -24 .. +12 dB
        agGain += agCoef * (agTarget - agGain);

        const float mix = mixSm.getNextValue();
        const float out = outSm.getNextValue();
        const float byp = bypassSm.getNextValue();

        for (int c = 0; c < numChannels; ++c)
        {
            const float processed = (ap[c] + mix * (wet[c] * agGain - ap[c])) * out;
            io[c][i] = processed + byp * (dry[c] - processed);   // bypass = latency-aligned dry
        }
    }
}

//==============================================================================
void DaliDistAudioProcessor::delayDryInPlace (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int ringSize = latency + 1;
    for (int i = 0; i < n; ++i)
    {
        const int readPos = (ringWrite + 1) % ringSize;
        for (int c = 0; c < juce::jmin (numChannels, buffer.getNumChannels()); ++c)
        {
            auto& ring = dryRing[(size_t) c];
            ring[(size_t) ringWrite] = buffer.getSample (c, i);
            buffer.setSample (c, i, ring[(size_t) readPos]);
        }
        ringWrite = readPos;
    }
}

void DaliDistAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Host-side bypass: keep latency consistent so nothing shifts in time
    delayDryInPlace (buffer);
}

//==============================================================================
juce::AudioProcessorEditor* DaliDistAudioProcessor::createEditor()
{
    return new DaliDistAudioProcessorEditor (*this);
}

void DaliDistAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DaliDistAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DaliDistAudioProcessor();
}
