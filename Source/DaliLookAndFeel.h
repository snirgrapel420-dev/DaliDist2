/*
  ==============================================================================
    Dali Audio — Neon LookAndFeel
    Dark near-black violet surfaces, one neon purple accent, glow used only
    where something is actually happening (value arcs, active modes, live color).
  ==============================================================================
*/
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace DaliColours
{
    const juce::Colour bgTop     { 0xff110c18 };
    const juce::Colour bgBottom  { 0xff07050a };
    const juce::Colour panel     { 0xff151020 };
    const juce::Colour panelEdge { 0xff2a2138 };
    const juce::Colour track     { 0xff231b2f };
    const juce::Colour neon      { 0xffb24bff };
    const juce::Colour neonHot   { 0xffe38bff };
    const juce::Colour neonDeep  { 0xff5b1fa8 };
    const juce::Colour text      { 0xffece6f7 };
    const juce::Colour textDim   { 0xff8a809e };
}

class DaliLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DaliLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, DaliColours::bgBottom);
        setColour (juce::TextButton::textColourOffId, DaliColours::textDim);
        setColour (juce::TextButton::textColourOnId, DaliColours::text);
    }

    static juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    /** Neon stroke: soft halo passes, then the core line. glow 0..1 scales the halo. */
    static void neonStroke (juce::Graphics& g, const juce::Path& p, float thickness, juce::Colour c, float glow)
    {
        const juce::PathStrokeType::JointStyle js = juce::PathStrokeType::curved;
        const juce::PathStrokeType::EndCapStyle ec = juce::PathStrokeType::rounded;
        for (int i = 4; i >= 1; --i)
        {
            g.setColour (c.withAlpha (juce::jlimit (0.0f, 1.0f, (0.035f + 0.07f * glow) * (5 - i) * 0.5f)));
            g.strokePath (p, juce::PathStrokeType (thickness + (float) i * (2.5f + 2.5f * glow), js, ec));
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thickness, js, ec));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        using namespace juce;
        const auto& props = s.getProperties();
        const float glow    = (float) props.getWithDefault ("glow", 0.0f);
        const bool bipolar  = (bool)  props.getWithDefault ("bipolar", false);
        const bool isHero   = (bool)  props.getWithDefault ("hero", false);

        auto area = Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float size   = jmin (area.getWidth(), area.getHeight());
        const float radius = size * 0.5f - size * 0.08f;
        const auto  c      = area.getCentre();
        const float trackW = jmax (3.0f, size * (isHero ? 0.035f : 0.05f));
        const float angle  = startAngle + pos * (endAngle - startAngle);

        // Body
        const float bodyR = radius * (isHero ? 0.80f : 0.74f);
        g.setGradientFill (ColourGradient (Colour (0xff231c2e), c.x, c.y - bodyR,
                                           Colour (0xff0a080e), c.x, c.y + bodyR, false));
        g.fillEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2, bodyR * 2);
        g.setColour (DaliColours::panelEdge);
        g.drawEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2, bodyR * 2, 1.0f);

        if (isHero)   // fine scale ticks around the hero knob
        {
            for (int i = 0; i <= 40; ++i)
            {
                const float a = startAngle + (endAngle - startAngle) * (float) i / 40.0f;
                const bool major = (i % 10 == 0);
                const float r1 = radius + trackW * 1.6f, r2 = r1 + (major ? size * 0.03f : size * 0.015f);
                g.setColour ((a <= angle ? DaliColours::neon : DaliColours::textDim).withAlpha (major ? 0.8f : 0.35f));
                g.drawLine (c.x + r1 * std::sin (a), c.y - r1 * std::cos (a),
                            c.x + r2 * std::sin (a), c.y - r2 * std::cos (a), 1.0f);
            }
        }

        // Track
        Path bg;
        bg.addCentredArc (c.x, c.y, radius, radius, 0.0f, startAngle, endAngle, true);
        g.setColour (DaliColours::track);
        g.strokePath (bg, PathStrokeType (trackW, PathStrokeType::curved, PathStrokeType::rounded));

        // Value arc
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        if (std::abs (angle - from) > 0.01f)
        {
            Path v;
            v.addCentredArc (c.x, c.y, radius, radius, 0.0f, jmin (from, angle), jmax (from, angle), true);
            neonStroke (g, v, trackW, DaliColours::neon.interpolatedWith (DaliColours::neonHot, glow * 0.6f), glow);
        }

        // End dot
        const float dotR = trackW * 0.95f;
        const Point<float> dot (c.x + radius * std::sin (angle), c.y - radius * std::cos (angle));
        g.setColour (DaliColours::neonHot.withAlpha (0.25f + 0.35f * glow));
        g.fillEllipse (dot.x - dotR * 2, dot.y - dotR * 2, dotR * 4, dotR * 4);
        g.setColour (DaliColours::text);
        g.fillEllipse (dot.x - dotR * 0.6f, dot.y - dotR * 0.6f, dotR * 1.2f, dotR * 1.2f);

        // Value text in the body
        g.setColour (DaliColours::text);
        g.setFont (font (isHero ? size * 0.11f : jmax (11.0f, size * 0.15f), isHero));
        g.drawFittedText (s.getTextFromValue (s.getValue()),
                          Rectangle<float> (c.x - bodyR, c.y - bodyR * 0.35f, bodyR * 2, bodyR * 0.7f).toNearestInt(),
                          Justification::centred, 1);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.5f);
        const float corner = r.getHeight() * 0.28f;
        const bool on = b.getToggleState();
        const bool hot = (bool) b.getProperties().getWithDefault ("hot", false);   // e.g. SOLO: brighter accent
        const auto accent = hot ? DaliColours::neonHot : DaliColours::neon;

        g.setColour (on ? (hot ? DaliColours::neon.withAlpha (0.30f) : DaliColours::neonDeep.withAlpha (0.35f)) : DaliColours::panel);
        g.fillRoundedRectangle (r, corner);

        if (on)
        {
            juce::Path p; p.addRoundedRectangle (r, corner);
            neonStroke (g, p, 1.2f, accent, hot ? 0.6f : 0.3f);
        }
        else
        {
            g.setColour (highlighted || down ? DaliColours::neon.withAlpha (0.55f) : DaliColours::panelEdge);
            g.drawRoundedRectangle (r, corner, 1.0f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setFont (font (juce::jmin (13.0f, (float) b.getHeight() * 0.42f), true).withExtraKerningFactor (0.06f));
        g.setColour (b.getToggleState() ? DaliColours::text : DaliColours::textDim);
        g.drawFittedText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, 1);
    }
};
