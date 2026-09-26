/*
  ==============================================================================
    DaliDist — Editor
    Hierarchy: Drive (hero, glows with the harmonics actually generated)
               -> Color / Bite on the left, Talk / Sub Guard on the right
               -> Drift / Mix / Output and the live status strip at the bottom.
  ==============================================================================
*/
#include "PluginEditor.h"

namespace
{
    constexpr int kW = 940, kH = 580;

    const char* modeNames[] = { "Tube", "Transformer", "Tape", "Console", "Acid" };
    const char* modeBlurbs[] =
    {
        "Tube: round even harmonics, the coupling cap blooms after loud notes",
        "Transformer: iron density in the low mids, thick and forward",
        "Tape: highs saturate first, transients get rounded and glued",
        "Console: clean until the knee, then fast odd-order presence",
        "Acid: vocal second harmonic, bite on accents, follows the resonance"
    };

    juce::String hzText (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                            : juce::String (hz / 1000.0f, 2) + " kHz";
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
DaliDistAudioProcessorEditor::DaliDistAudioProcessorEditor (DaliDistAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      drive    (p.apvts, ParamIDs::drive,    "Drive", true),
      color    (p.apvts, ParamIDs::color,    "Color"),
      bite     (p.apvts, ParamIDs::bite,     "Bite"),
      talk     (p.apvts, ParamIDs::talk,     "Talk"),
      subGuard (p.apvts, ParamIDs::subGuard, "Sub guard"),
      drift    (p.apvts, ParamIDs::drift,    "Drift"),
      mix      (p.apvts, ParamIDs::mix,      "Mix"),
      output   (p.apvts, ParamIDs::output,   "Output")
{
    setLookAndFeel (&lnf);

    output.slider.getProperties().set ("bipolar", true);
    color.setCaption ("warm to excited");
    bite.setCaption ("attack attitude");
    subGuard.setCaption ("clean below");

    for (auto* k : { &drive, &color, &bite, &talk, &subGuard, &drift, &mix, &output })
        addAndMakeVisible (k);

    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);

    for (int i = 0; i < 5; ++i)
    {
        auto* b = modeButtons.add (new juce::TextButton (modeNames[i]));
        b->onClick = [this, i] { selectMode (i); };
        addAndMakeVisible (b);
    }

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

void DaliDistAudioProcessorEditor::selectMode (int index)
{
    if (auto* param = proc.apvts.getParameter (ParamIDs::mode))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 ((float) index));
        param->endChangeGesture();
    }
}

//==============================================================================
void DaliDistAudioProcessorEditor::timerCallback()
{
    const float colorDb = proc.meterColorDb.load();
    const bool  bypassed = proc.apvts.getRawParameterValue (ParamIDs::bypass)->load() > 0.5f;

    // The hero ring glows with the harmonics actually being generated
    const float target = bypassed ? 0.0f : juce::jlimit (0.0f, 1.0f, (colorDb + 42.0f) / 34.0f);
    colorGlow += 0.22f * (target - colorGlow);
    drive.setGlow (colorGlow);
    colorDbShown += 0.2f * (colorDb - colorDbShown);

    biteGlow += 0.35f * (juce::jlimit (0.0f, 1.0f, proc.meterBite.load() / 1.2f) - biteGlow);
    bite.setGlow (biteGlow);

    const float talkAmt = proc.apvts.getRawParameterValue (ParamIDs::talk)->load();
    const int mode = (int) proc.apvts.getRawParameterValue (ParamIDs::mode)->load();
    talk.setCaption ((talkAmt < 0.5f && mode != 4) ? juce::String ("off")
                                                   : "following " + hzText (proc.meterTalkHz.load()));

    inMeter.setLevelDb (proc.meterInDb.load());
    outMeter.setLevelDb (proc.meterOutDb.load());

    for (int i = 0; i < modeButtons.size(); ++i)
        modeButtons[i]->setToggleState (i == mode, juce::dontSendNotification);

    repaint (heroArea.expanded (40));
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
    g.drawText ("Analog color for acid and trance", 200, 44, 300, 20, Justification::left);

    // Neon hairline
    {
        ColourGradient cg (DaliColours::neon.withAlpha (0.0f), 20, 78, DaliColours::neon.withAlpha (0.0f), W - 20, 78, false);
        cg.addColour (0.5, DaliColours::neon.withAlpha (0.7f));
        g.setGradientFill (cg);
        g.fillRect (20.0f, 78.0f, W - 40.0f, 1.0f);
    }

    // --- Main panel
    auto panel = Rectangle<float> (20.0f, 130.0f, W - 40.0f, 322.0f);
    g.setColour (DaliColours::panel.withAlpha (0.55f));
    g.fillRoundedRectangle (panel, 14.0f);
    g.setColour (DaliColours::panelEdge);
    g.drawRoundedRectangle (panel, 14.0f, 1.0f);

    // Hero backdrop: soft violet bloom that follows the generated color
    {
        const auto c = heroArea.toFloat().withTrimmedBottom (44.0f).getCentre();
        const float r = 170.0f + 30.0f * colorGlow;
        ColourGradient bloom (DaliColours::neon.withAlpha (0.05f + 0.16f * colorGlow), c.x, c.y,
                              DaliColours::neon.withAlpha (0.0f), c.x + r, c.y, true);
        g.setGradientFill (bloom);
        g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
    }

    // --- Bottom strip
    auto strip = Rectangle<float> (20.0f, 466.0f, W - 40.0f, 98.0f);
    g.setColour (DaliColours::panel.withAlpha (0.4f));
    g.fillRoundedRectangle (strip, 12.0f);
    g.setColour (DaliColours::panelEdge.withAlpha (0.7f));
    g.drawRoundedRectangle (strip, 12.0f, 1.0f);

    // Status
    const int mode = juce::jlimit (0, 4, (int) proc.apvts.getRawParameterValue (ParamIDs::mode)->load());
    const bool bypassed = proc.apvts.getRawParameterValue (ParamIDs::bypass)->load() > 0.5f;
    auto s = statusArea;
    g.setColour (DaliColours::text);
    g.setFont (DaliLookAndFeel::font (13.0f, true));
    g.drawText (bypassed ? String ("Bypassed: you are hearing the dry signal at the same latency")
                         : String (modeBlurbs[mode]), s.removeFromTop (26), Justification::left);

    g.setFont (DaliLookAndFeel::font (12.5f));
    auto row = s.removeFromTop (24);
    g.setColour (DaliColours::textDim);
    g.drawText ("Harmonics", row.removeFromLeft (80), Justification::left);
    g.setColour (DaliColours::neonHot);
    g.drawText (bypassed || colorDbShown < -80.0f ? String ("-") : String (colorDbShown, 1) + " dB", row.removeFromLeft (90), Justification::left);
    g.setColour (DaliColours::textDim);
    g.drawText ("Auto gain", row.removeFromLeft (76), Justification::left);
    const float ag = proc.meterAutoGainDb.load();
    g.setColour (DaliColours::text);
    g.drawText ((ag >= 0 ? "+" : "") + String (ag, 1) + " dB", row, Justification::left);
}

void DaliDistAudioProcessorEditor::resized()
{
    const int W = getWidth();

    bypassButton.setBounds (W - 30 - 100, 26, 100, 34);
    autoGainButton.setBounds (W - 30 - 100 - 10 - 116, 26, 116, 34);

    const int bw = 130, gap = 8, total = 5 * bw + 4 * gap;
    for (int i = 0; i < modeButtons.size(); ++i)
        modeButtons[i]->setBounds ((W - total) / 2 + i * (bw + gap), 90, bw, 30);

    inMeter.setBounds (40, 150, 18, 286);
    outMeter.setBounds (W - 58, 150, 18, 286);

    color.setBounds (92, 140, 156, 152);
    bite.setBounds (92, 294, 156, 152);
    talk.setBounds (W - 248, 140, 156, 152);
    subGuard.setBounds (W - 248, 294, 156, 152);

    heroArea = juce::Rectangle<int> (W / 2 - 160, 138, 320, 312);
    drive.setBounds (heroArea);

    drift.setBounds (44, 472, 100, 90);
    mix.setBounds (150, 472, 100, 90);
    output.setBounds (256, 472, 100, 90);
    statusArea = juce::Rectangle<int> (390, 482, W - 420, 70);
}
