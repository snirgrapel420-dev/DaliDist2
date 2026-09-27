/*
  ==============================================================================
    DaliDist v3 — Editor (multiband)
    Hierarchy: band map (where the bands are, what each is doing)
               -> three band panels (Drive hero, Level, On/Solo, circuit)
               -> global strip (Drive for all bands, Color, Talk, Bite, Sub guard, Drift, Mix, Output).
  ==============================================================================
*/
#include "PluginEditor.h"

namespace
{
    constexpr int kW = 1080, kH = 700;

    const char* modeNames[]  = { "Tube", "Transformer", "Tape", "Console", "Acid" };
    const char* modeShort[]  = { "Tube", "Xfmr", "Tape", "Cnsl", "Acid" };
    const char* bandTitles[] = { "LOW", "MID", "HIGH" };

    juce::String hzText (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                            : juce::String (hz / 1000.0f, hz < 10000.0f ? 1 : 0) + " kHz";
    }

    float rawParam (DaliDistAudioProcessor& p, const juce::String& id)
    {
        if (auto* v = p.apvts.getRawParameterValue (id)) return v->load();
        return 0.0f;
    }

    float glowFromDb (float db) { return juce::jlimit (0.0f, 1.0f, (db + 42.0f) / 34.0f); }

    void setParam (DaliDistAudioProcessor& p, const juce::String& id, float plainValue)
    {
        if (auto* param = p.apvts.getParameter (id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
            param->endChangeGesture();
        }
    }
}

//==============================================================================
DaliKnob::DaliKnob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID,
                    const juce::String& titleText, bool hero)
    : title (titleText), isHero (hero)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                juce::MathConstants<float>::pi * 2.75f, true);
    slider.setMouseDragSensitivity (hero ? 360 : 240);
    if (hero) slider.getProperties().set ("hero", true);
    addAndMakeVisible (slider);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramID, slider);
    if (auto* p = state.getParameter (paramID))
        slider.setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));
}

void DaliKnob::setGlow (float g)
{
    const float old = (float) slider.getProperties().getWithDefault ("glow", 0.0f);
    if (std::abs (old - g) > 0.01f)
    {
        slider.getProperties().set ("glow", g);
        slider.repaint();
    }
}

void DaliKnob::resized()
{
    auto r = getLocalBounds();
    r.removeFromBottom (isHero ? 44 : 36);
    const int side = juce::jmin (r.getWidth(), r.getHeight());
    slider.setBounds (r.withSizeKeepingCentre (side, side));
}

void DaliKnob::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().removeFromBottom (isHero ? 44 : 36);
    g.setColour (DaliColours::text);
    g.setFont (DaliLookAndFeel::font (isHero ? 17.0f : 13.5f, true).withExtraKerningFactor (0.04f));
    g.drawText (title, r.removeFromTop (isHero ? 24 : 19), juce::Justification::centred);
    if (caption.isNotEmpty())
    {
        g.setColour (DaliColours::textDim);
        g.setFont (DaliLookAndFeel::font (11.5f));
        g.drawText (caption, r, juce::Justification::centredTop);
    }
}

//==============================================================================
void NeonMeter::setLevelDb (float db)
{
    shown = db > shown ? db : juce::jmax (db, shown - 1.2f);
    if (db >= peak) { peak = db; peakHold = 45; }
    else if (--peakHold < 0) peak = juce::jmax (-60.0f, peak - 0.8f);
    repaint();
}

void NeonMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto labelArea = r.removeFromBottom (18.0f);
    auto bar = r.reduced (2.0f, 0.0f);

    g.setColour (DaliColours::track);
    g.fillRoundedRectangle (bar, 4.0f);

    auto toY = [&] (float db) { return bar.getBottom() - bar.getHeight() * juce::jlimit (0.0f, 1.0f, juce::jmap (db, -48.0f, 3.0f, 0.0f, 1.0f)); };

    const float top = toY (shown);
    if (top < bar.getBottom() - 1.0f)
    {
        auto fill = bar.withTop (top);
        g.setGradientFill (juce::ColourGradient (DaliColours::neonDeep, bar.getCentreX(), bar.getBottom(),
                                                 DaliColours::neonHot, bar.getCentreX(), bar.getY(), false));
        g.fillRoundedRectangle (fill, 4.0f);
    }

    g.setColour (peak > -0.1f ? juce::Colour (0xffff4d7a) : DaliColours::text.withAlpha (0.8f));
    g.fillRect (bar.getX(), toY (peak) - 1.0f, bar.getWidth(), 2.0f);

    g.setColour (DaliColours::textDim);
    g.setFont (DaliLookAndFeel::font (11.0f, true));
    g.drawText (label, labelArea, juce::Justification::centred);
}


//==============================================================================
BandMap::BandMap (DaliDistAudioProcessor& p) : proc (p) {}

float BandMap::freqToX (float f) const
{
    const auto r = getLocalBounds().toFloat().reduced (14.0f, 0.0f);
    return r.getX() + r.getWidth() * std::log (juce::jlimit (20.0f, 20000.0f, f) / 20.0f) / std::log (1000.0f);
}

float BandMap::xToFreq (float x) const
{
    const auto r = getLocalBounds().toFloat().reduced (14.0f, 0.0f);
    return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth()));
}

juce::RangedAudioParameter* BandMap::paramFor (int handle) const
{
    const char* ids[] = { ParamIDs::subGuard, ParamIDs::xLow, ParamIDs::xHigh };
    return handle >= 0 && handle < 3 ? proc.apvts.getParameter (ids[handle]) : nullptr;
}

int BandMap::handleAt (float x) const
{
    const float fr[] = { rawParam (proc, ParamIDs::subGuard), rawParam (proc, ParamIDs::xLow), rawParam (proc, ParamIDs::xHigh) };
    int best = -1; float bestD = 8.0f;
    for (int h = 0; h < 3; ++h)
    {
        const float d = std::abs (freqToX (fr[h]) - x);
        if (d < bestD) { bestD = d; best = h; }
    }
    return best;
}

void BandMap::mouseMove (const juce::MouseEvent& e)
{
    const int h = handleAt ((float) e.x);
    if (h != hover) { hover = h; repaint(); }
    setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void BandMap::mouseExit (const juce::MouseEvent&) { hover = -1; repaint(); }

void BandMap::mouseDown (const juce::MouseEvent& e)
{
    dragging = handleAt ((float) e.x);
    if (auto* p = paramFor (dragging)) p->beginChangeGesture();
}

void BandMap::mouseDrag (const juce::MouseEvent& e)
{
    if (auto* p = paramFor (dragging))
    {
        const auto range = p->getNormalisableRange();
        const float f = juce::jlimit (range.start, range.end, xToFreq ((float) e.x));
        p->setValueNotifyingHost (p->convertTo0to1 (f));
        repaint();
    }
}

void BandMap::mouseUp (const juce::MouseEvent&)
{
    if (auto* p = paramFor (dragging)) p->endChangeGesture();
    dragging = -1;
}

void BandMap::paint (juce::Graphics& g)
{
    using namespace juce;
    auto bounds = getLocalBounds().toFloat();

    g.setColour (DaliColours::panel.withAlpha (0.55f));
    g.fillRoundedRectangle (bounds, 12.0f);
    g.setColour (DaliColours::panelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 12.0f, 1.0f);

    const float top = 10.0f, bottom = bounds.getHeight() - 20.0f;
    const float left = freqToX (20.0f), right = freqToX (20000.0f);

    // Grid
    g.setFont (DaliLookAndFeel::font (10.5f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f);
        g.setColour (DaliColours::panelEdge.withAlpha (0.6f));
        g.drawVerticalLine (roundToInt (x), top, bottom);
        g.setColour (DaliColours::textDim.withAlpha (0.7f));
        g.drawText (f < 1000.0f ? String ((int) f) : String ((int) (f / 1000.0f)) + "k",
                    Rectangle<float> (x - 20.0f, bottom + 3.0f, 40.0f, 14.0f), Justification::centred);
    }

    const float sub = rawParam (proc, ParamIDs::subGuard);
    const float xl  = rawParam (proc, ParamIDs::xLow);
    const float xh  = juce::jmax (rawParam (proc, ParamIDs::xHigh), xl * 2.0f);
    const float xs[] = { freqToX (sub), freqToX (xl), freqToX (xh), right };

    // Clean sub zone: hatched
    {
        auto zone = Rectangle<float> (left, top, xs[0] - left, bottom - top);
        g.setColour (Colour (0xff0b0910));
        g.fillRect (zone);
        g.saveState();
        g.reduceClipRegion (zone.toNearestInt());
        g.setColour (DaliColours::textDim.withAlpha (0.12f));
        for (float x = zone.getX() - zone.getHeight(); x < zone.getRight(); x += 7.0f)
            g.drawLine (x, zone.getBottom(), x + zone.getHeight(), zone.getY(), 1.0f);
        g.restoreState();
        if (zone.getWidth() > 44.0f)
        {
            g.setColour (DaliColours::textDim);
            g.setFont (DaliLookAndFeel::font (10.5f, true).withExtraKerningFactor (0.1f));
            g.drawText ("CLEAN", zone, Justification::centred);
        }
    }

    // Bands
    bool anySolo = false;
    for (int b = 0; b < dali::kNumBands; ++b)
        anySolo = anySolo || rawParam (proc, ParamIDs::band (b, ParamIDs::solo)) > 0.5f;

    const float masterMul = rawParam (proc, ParamIDs::master) * 0.01f;
    for (int b = 0; b < dali::kNumBands; ++b)
    {
        const bool on     = rawParam (proc, ParamIDs::band (b, ParamIDs::on)) > 0.5f;
        const bool soloed = rawParam (proc, ParamIDs::band (b, ParamIDs::solo)) > 0.5f;
        const bool audible = ! anySolo || soloed;
        const float gl = on ? glows[(size_t) b] : 0.0f;

        auto zone = Rectangle<float> (xs[b], top, xs[b + 1] - xs[b], bottom - top).reduced (1.0f, 0.0f);
        const float dim = audible ? 1.0f : 0.3f;

        ColourGradient fill (DaliColours::neon.withAlpha ((0.05f + 0.30f * gl) * dim), 0, zone.getY(),
                             DaliColours::neon.withAlpha (0.01f * dim), 0, zone.getBottom(), false);
        g.setGradientFill (fill);
        g.fillRect (zone);

        // Energy line on top of the band: height = what this band is doing right now
        const float y = zone.getBottom() - zone.getHeight() * (0.18f + 0.72f * gl);
        Path topLine; topLine.startNewSubPath (zone.getX(), y); topLine.lineTo (zone.getRight(), y);
        DaliLookAndFeel::neonStroke (g, topLine, 1.6f,
                                     (soloed ? DaliColours::neonHot : DaliColours::neon).withMultipliedAlpha (on ? dim : 0.35f),
                                     on ? gl * dim : 0.0f);

        if (zone.getWidth() > 60.0f)
        {
            const int mode = jlimit (0, 4, (int) rawParam (proc, ParamIDs::band (b, ParamIDs::mode)));
            const float drv = jlimit (0.0f, 100.0f, rawParam (proc, ParamIDs::band (b, ParamIDs::drive)) * masterMul);
            g.setColour (DaliColours::text.withAlpha (audible ? 1.0f : 0.4f));
            g.setFont (DaliLookAndFeel::font (12.5f, true).withExtraKerningFactor (0.08f));
            g.drawText (bandTitles[b], zone.withHeight (22.0f).translated (0, 6), Justification::centred);
            g.setColour (DaliColours::textDim.withAlpha (audible ? 1.0f : 0.4f));
            g.setFont (DaliLookAndFeel::font (11.0f));
            g.drawText (on ? String (zone.getWidth() > 120.0f ? modeNames[mode] : modeShort[mode]) + "  " + String (roundToInt (drv)) + "%"
                           : String ("off"),
                        zone.withHeight (16.0f).translated (0, 27), Justification::centred);
        }
    }

    // Handles
    const float handleFreqs[] = { sub, xl, xh };
    for (int h = 0; h < 3; ++h)
    {
        const float x = xs[h];
        const bool hot = (h == hover || h == dragging);
        const auto c = h == 0 ? DaliColours::textDim : DaliColours::neonHot;
        Path line; line.startNewSubPath (x, top); line.lineTo (x, bottom);
        if (h == 0) { g.setColour (c.withAlpha (hot ? 0.9f : 0.55f)); g.strokePath (line, PathStrokeType (hot ? 2.0f : 1.2f)); }
        else        DaliLookAndFeel::neonStroke (g, line, hot ? 2.0f : 1.3f, c, hot ? 0.8f : 0.35f);

        g.setColour (hot ? DaliColours::text : c);
        g.fillEllipse (x - 4.5f, top - 1.0f, 9.0f, 9.0f);

        if (hot)
        {
            auto label = Rectangle<float> (x - 34.0f, bottom - 22.0f, 68.0f, 18.0f);
            g.setColour (DaliColours::bgBottom.withAlpha (0.85f));
            g.fillRoundedRectangle (label, 5.0f);
            g.setColour (DaliColours::text);
            g.setFont (DaliLookAndFeel::font (11.0f, true));
            g.drawText (hzText (handleFreqs[h]), label, Justification::centred);
        }
    }
    ignoreUnused (left);
}

//==============================================================================
BandPanel::BandPanel (DaliDistAudioProcessor& p, int bandIndex)
    : proc (p), band (bandIndex),
      drive (p.apvts, ParamIDs::band (bandIndex, ParamIDs::drive), "Drive", true),
      level (p.apvts, ParamIDs::band (bandIndex, ParamIDs::level), "Level")
{
    level.slider.getProperties().set ("bipolar", true);
    addAndMakeVisible (drive);
    addAndMakeVisible (level);

    onButton.setClickingTogglesState (true);
    soloButton.setClickingTogglesState (true);
    soloButton.getProperties().set ("hot", true);
    addAndMakeVisible (onButton);
    addAndMakeVisible (soloButton);
    onAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::band (band, ParamIDs::on), onButton);
    soloAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::band (band, ParamIDs::solo), soloButton);

    for (int m = 0; m < 5; ++m)
    {
        auto* b = modeButtons.add (new juce::TextButton (modeShort[m]));
        b->setTooltip (modeNames[m]);
        b->onClick = [this, m] { selectMode (m); };
        addAndMakeVisible (b);
    }
}

void BandPanel::selectMode (int m)
{
    setParam (proc, ParamIDs::band (band, ParamIDs::mode), (float) m);
}

void BandPanel::refresh (float glowTarget)
{
    const bool on = rawParam (proc, ParamIDs::band (band, ParamIDs::on)) > 0.5f;
    bool anySolo = false;
    for (int b = 0; b < dali::kNumBands; ++b)
        anySolo = anySolo || rawParam (proc, ParamIDs::band (b, ParamIDs::solo)) > 0.5f;
    const bool isSolo = rawParam (proc, ParamIDs::band (band, ParamIDs::solo)) > 0.5f;

    glow += 0.22f * ((on ? glowTarget : 0.0f) - glow);
    drive.setGlow (glow);

    const bool newActive = on && (! anySolo || isSolo);
    const float alpha = on ? (newActive ? 1.0f : 0.5f) : 0.35f;
    drive.setAlpha (alpha);
    level.setAlpha (alpha);

    const int mode = (int) rawParam (proc, ParamIDs::band (band, ParamIDs::mode));
    for (int i = 0; i < modeButtons.size(); ++i)
    {
        modeButtons[i]->setToggleState (i == mode, juce::dontSendNotification);
        modeButtons[i]->setAlpha (alpha);
    }

    if (newActive != active || isSolo != soloed) { active = newActive; soloed = isSolo; }
    repaint (getLocalBounds().removeFromTop (44));
}

void BandPanel::paint (juce::Graphics& g)
{
    using namespace juce;
    auto r = getLocalBounds().toFloat();

    g.setColour (DaliColours::panel.withAlpha (active ? 0.55f : 0.35f));
    g.fillRoundedRectangle (r, 14.0f);
    if (soloed)
    {
        Path p; p.addRoundedRectangle (r.reduced (1.0f), 14.0f);
        DaliLookAndFeel::neonStroke (g, p, 1.2f, DaliColours::neonHot, 0.4f);
    }
    else
    {
        g.setColour (DaliColours::panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.0f);
    }

    // Title with a glow that follows what the band is doing
    auto header = getLocalBounds().removeFromTop (44).reduced (16, 0);
    const auto titleFont = DaliLookAndFeel::font (17.0f, true).withExtraKerningFactor (0.12f);
    g.setFont (titleFont);
    if (glow > 0.02f)
    {
        g.setColour (DaliColours::neon.withAlpha (0.18f * glow));
        for (int dx = -2; dx <= 2; dx += 2)
            for (int dy = -2; dy <= 2; dy += 2)
                g.drawText (bandTitles[band], header.translated (dx, dy), Justification::centredLeft);
    }
    g.setColour (active ? DaliColours::text : DaliColours::textDim);
    g.drawText (bandTitles[band], header, Justification::centredLeft);

    // Frequency range of the band
    const float sub = rawParam (proc, ParamIDs::subGuard);
    const float xl  = rawParam (proc, ParamIDs::xLow);
    const float xh  = jmax (rawParam (proc, ParamIDs::xHigh), xl * 2.0f);
    const float lo[] = { sub, xl, xh }, hi[] = { xl, xh, 20000.0f };
    g.setColour (DaliColours::textDim);
    g.setFont (DaliLookAndFeel::font (11.5f));
    g.drawText (hzText (lo[band]) + " - " + (band == 2 ? String ("20 kHz") : hzText (hi[band])),
                header.withTrimmedLeft (64), Justification::centredLeft);
}

void BandPanel::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (44).reduced (12, 9);
    soloButton.setBounds (header.removeFromRight (58));
    header.removeFromRight (6);
    onButton.setBounds (header.removeFromRight (50));

    auto modes = r.removeFromBottom (42).reduced (10, 7);
    const int gap = 5, bw = (modes.getWidth() - 4 * gap) / 5;
    for (int i = 0; i < modeButtons.size(); ++i)
        modeButtons[i]->setBounds (modes.getX() + i * (bw + gap), modes.getY(), bw, modes.getHeight());

    r.reduce (6, 0);
    drive.setBounds (r.removeFromLeft ((int) (r.getWidth() * 0.6f)));
    level.setBounds (r.withSizeKeepingCentre (r.getWidth(), juce::jmin (r.getHeight(), 140)));
}

//==============================================================================
DaliDistAudioProcessorEditor::DaliDistAudioProcessorEditor (DaliDistAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), bandMap (p),
      master   (p.apvts, ParamIDs::master,   "Drive"),
      color    (p.apvts, ParamIDs::color,    "Color"),
      talk     (p.apvts, ParamIDs::talk,     "Talk"),
      bite     (p.apvts, ParamIDs::bite,     "Bite"),
      subGuard (p.apvts, ParamIDs::subGuard, "Sub guard"),
      drift    (p.apvts, ParamIDs::drift,    "Drift"),
      mix      (p.apvts, ParamIDs::mix,      "Mix"),
      output   (p.apvts, ParamIDs::output,   "Output")
{
    setLookAndFeel (&lnf);

    output.slider.getProperties().set ("bipolar", true);
    master.setCaption ("all bands");
    color.setCaption ("warm to excited");
    bite.setCaption ("attack attitude");
    subGuard.setCaption ("clean below");

    addAndMakeVisible (bandMap);
    for (int b = 0; b < dali::kNumBands; ++b)
    {
        bands[b] = std::make_unique<BandPanel> (p, b);
        addAndMakeVisible (*bands[b]);
    }

    for (auto* k : { &master, &color, &talk, &bite, &subGuard, &drift, &mix, &output })
        addAndMakeVisible (k);

    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);

    autoGainButton.setClickingTogglesState (true);
    bypassButton.setClickingTogglesState (true);
    addAndMakeVisible (autoGainButton);
    addAndMakeVisible (bypassButton);
    autoGainAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::autoGain, autoGainButton);
    bypassAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::bypass, bypassButton);

    setSize (kW, kH);
    startTimerHz (30);
}

DaliDistAudioProcessorEditor::~DaliDistAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void DaliDistAudioProcessorEditor::timerCallback()
{
    const bool bypassed = rawParam (proc, ParamIDs::bypass) > 0.5f;

    for (int b = 0; b < dali::kNumBands; ++b)
    {
        const float target = bypassed ? 0.0f : glowFromDb (proc.meterBandColorDb[b].load());
        bandGlow[(size_t) b] += 0.22f * (target - bandGlow[(size_t) b]);
        bands[b]->refresh (target);
    }
    bandMap.setGlows (bandGlow);

    colorDbShown += 0.2f * (proc.meterColorDb.load() - colorDbShown);

    biteGlow += 0.35f * (juce::jlimit (0.0f, 1.0f, proc.meterBite.load() / 1.2f) - biteGlow);
    bite.setGlow (biteGlow);

    bool anyAcid = false;
    for (int b = 0; b < dali::kNumBands; ++b)
        anyAcid = anyAcid || (int) rawParam (proc, ParamIDs::band (b, ParamIDs::mode)) == 4;
    talk.setCaption ((rawParam (proc, ParamIDs::talk) < 0.5f && ! anyAcid) ? juce::String ("off")
                                                                           : "mid follows " + hzText (proc.meterTalkHz.load()));

    inMeter.setLevelDb (proc.meterInDb.load());
    outMeter.setLevelDb (proc.meterOutDb.load());

    repaint (statusArea);
}

//==============================================================================
void DaliDistAudioProcessorEditor::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto W = (float) getWidth();

    g.setGradientFill (ColourGradient (DaliColours::bgTop, 0, 0, DaliColours::bgBottom, 0, (float) getHeight(), false));
    g.fillAll();

    // --- Header: brand + logo with neon glow
    g.setColour (DaliColours::textDim);
    g.setFont (DaliLookAndFeel::font (12.0f, true).withExtraKerningFactor (0.18f));
    g.drawText ("Dali Audio", 30, 16, 200, 16, Justification::left);

    const auto logoFont = DaliLookAndFeel::font (34.0f, true).withExtraKerningFactor (0.02f);
    g.setFont (logoFont);
    for (int i = 3; i >= 1; --i)
    {
        g.setColour (DaliColours::neon.withAlpha (0.10f));
        for (int dx = -i; dx <= i; dx += i)
            for (int dy = -i; dy <= i; dy += i)
                g.drawText ("DaliDist", 28 + dx, 30 + dy, 260, 40, Justification::left);
    }
    g.setColour (DaliColours::text);
    g.drawText ("DaliDist", 28, 30, 260, 40, Justification::left);

    g.setColour (DaliColours::textDim);
    g.setFont (DaliLookAndFeel::font (12.5f));
    g.drawText ("Multiband analog color for acid and trance", 200, 44, 300, 20, Justification::left);

    // Neon hairline
    {
        ColourGradient cg (DaliColours::neon.withAlpha (0.0f), 20, 78, DaliColours::neon.withAlpha (0.0f), W - 20, 78, false);
        cg.addColour (0.5, DaliColours::neon.withAlpha (0.7f));
        g.setGradientFill (cg);
        g.fillRect (20.0f, 78.0f, W - 40.0f, 1.0f);
    }

    // --- Global strip
    auto strip = Rectangle<float> (20.0f, 500.0f, W - 40.0f, 184.0f);
    g.setColour (DaliColours::panel.withAlpha (0.4f));
    g.fillRoundedRectangle (strip, 12.0f);
    g.setColour (DaliColours::panelEdge.withAlpha (0.7f));
    g.drawRoundedRectangle (strip, 12.0f, 1.0f);

    // --- Status (header, between logo and buttons)
    const bool bypassed = rawParam (proc, ParamIDs::bypass) > 0.5f;
    auto s = statusArea;
    g.setFont (DaliLookAndFeel::font (12.5f));
    if (bypassed)
    {
        g.setColour (DaliColours::text);
        g.drawText ("Bypassed: dry signal at the same latency", s, Justification::centredRight);
    }
    else
    {
        const float ag = proc.meterAutoGainDb.load();
        auto row = s;
        g.setColour (DaliColours::text);
        g.drawText ((ag >= 0 ? "+" : "") + String (ag, 1) + " dB", row.removeFromRight (62), Justification::centredLeft);
        g.setColour (DaliColours::textDim);
        g.drawText ("Auto gain", row.removeFromRight (70), Justification::centredLeft);
        row.removeFromRight (14);
        g.setColour (DaliColours::neonHot);
        g.drawText (colorDbShown < -80.0f ? String ("-") : String (colorDbShown, 1) + " dB", row.removeFromRight (66), Justification::centredLeft);
        g.setColour (DaliColours::textDim);
        g.drawText ("Harmonics", row.removeFromRight (76), Justification::centredLeft);
    }
}

void DaliDistAudioProcessorEditor::resized()
{
    const int W = getWidth();

    bypassButton.setBounds (W - 30 - 100, 26, 100, 34);
    autoGainButton.setBounds (W - 30 - 100 - 10 - 116, 26, 116, 34);
    statusArea = juce::Rectangle<int> (480, 30, W - 30 - 226 - 16 - 480, 26);

    bandMap.setBounds (20, 92, W - 40, 120);

    const int gap = 14, pw = (W - 40 - 2 * gap) / 3;
    for (int b = 0; b < dali::kNumBands; ++b)
        bands[b]->setBounds (20 + b * (pw + gap), 224, pw, 262);

    inMeter.setBounds (40, 516, 18, 150);
    outMeter.setBounds (W - 58, 516, 18, 150);

    const int x0 = 76, x1 = W - 76, kw = (x1 - x0) / 8;
    int i = 0;
    for (auto* k : { &master, &color, &talk, &bite, &subGuard, &drift, &mix, &output })
        k->setBounds (x0 + (i++) * kw, 510, kw, 164);
}
