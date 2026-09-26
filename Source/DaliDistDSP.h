/*
  ==============================================================================
    DaliDist v2 — Analog Chain Engine
    Dali Audio

    Pure C++ (no JUCE dependency) so the engine can be unit-tested on its own.
    Runs at the OVERSAMPLED rate and outputs ONLY the color: P(HP(x)) - HP(x),
    where HP is an LR4 high-pass at SUB GUARD. The processor builds
        wet = AP(dry) + color  ==  LP(dry) + P(HP(dry))
    with AP = LR4 all-pass (LP + HP) on the delayed dry, and mixes against AP(dry),
    so parallel mix never notches at the crossover and the sub is never touched.
    Drive 0 -> color is exactly 0.

    Chain (per channel):
      sub guard HP -> TALK (resonance-tracking pre-emphasis)
      -> Stage A (asymmetric triode, grid-conduction coupling-cap blocking)  [inverting]
      -> interstage coupling HP + treble loss
      -> Stage B (second circuit, BITE = transient-driven extra drive, tape hysteresis) [inverting]
      -> output transformer (LF flux saturation) -> TALK de-emphasis -> transformer HF resonance
  ==============================================================================
*/
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dali
{
constexpr float kPi = 3.14159265358979323846f;

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float smoothstep (float e0, float e1, float x) noexcept
{
    const float t = std::clamp ((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

//==============================================================================
// Filters
//==============================================================================

/** Topology-preserving one-pole (Zavalishin). */
struct OnePole
{
    float g = 0.0f, s = 0.0f;

    void setCutoff (float fc, float fs) noexcept
    {
        const float w = std::tan (kPi * std::min (fc, 0.49f * fs) / fs);
        g = w / (1.0f + w);
    }
    void reset() noexcept { s = 0.0f; }

    inline float lp (float x) noexcept
    {
        const float v = (x - s) * g;
        const float y = v + s;
        s = y + v;
        return y;
    }
    inline float hp (float x) noexcept { return x - lp (x); }
};

/** Simper / Cytomic trapezoidal SVF. */
struct SVF
{
    float a1 = 1, a2 = 0, a3 = 0, k = 1.41421356f;
    float ic1 = 0, ic2 = 0;
    float lpOut = 0, hpOut = 0;

    void set (float fc, float fs, float q) noexcept
    {
        const float g = std::tan (kPi * std::min (fc, 0.49f * fs) / fs);
        k  = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }

    inline void tick (float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lpOut = v2;
        hpOut = v0 - k * v1 - v2;
    }
};

/** First-order shelf H(s) = K (s + wz) / (s + wp), bilinear with prewarp.
    K = sqrt(wp/wz) -> tilt around the centre (DC and HF gains reciprocal).
    makeInverse() builds the exact inverse, so pre/de-emphasis cancel perfectly. */
struct Shelf1
{
    float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;

    void design (float fz, float fp, float fs, bool inverse) noexcept
    {
        const float c  = 2.0f * fs;
        float wz = c * std::tan (kPi * std::min (fz, 0.45f * fs) / fs);
        float wp = c * std::tan (kPi * std::min (fp, 0.45f * fs) / fs);
        float K  = std::sqrt (wp / wz);
        if (inverse) { std::swap (wz, wp); K = 1.0f / K; }
        const float n = 1.0f / (c + wp);
        b0 = K * (c + wz) * n;
        b1 = K * (wz - c) * n;
        a1 = (wp - c) * n;
    }
    void reset() noexcept { x1 = y1 = 0.0f; }

    inline float process (float x) noexcept
    {
        const float y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x; y1 = y;
        return y;
    }
};

//==============================================================================
// Deterministic RNG + analog drift source
//==============================================================================
struct XorShift
{
    uint32_t s = 0x9E3779B9u;
    explicit XorShift (uint32_t seed = 1) : s (seed ? seed : 1u) {}
    float next() noexcept   // [-1, 1]
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float) (s & 0xFFFFFF) / (float) 0x7FFFFF - 1.0f;
    }
};

/** Very slow, smooth random walk (≈0.05–0.5 Hz). Deterministic, so bounces are repeatable. */
struct DriftSource
{
    XorShift rng { 1 };
    float target = 0, s1 = 0, s2 = 0, coef = 0;
    int countdown = 0, minHold = 1, maxHold = 1;

    void prepare (float controlRate, uint32_t seed) noexcept
    {
        rng = XorShift (seed);
        coef = 1.0f - std::exp (-1.0f / (1.2f * controlRate));  // ~1.2 s smoothing
        minHold = (int) (1.5f * controlRate);
        maxHold = (int) (5.0f * controlRate);
        target = rng.next(); s1 = s2 = target * 0.5f;
        countdown = minHold;
    }
    float tick() noexcept
    {
        if (--countdown <= 0)
        {
            target = rng.next();
            countdown = minHold + (int) ((rng.next() * 0.5f + 0.5f) * (float) (maxHold - minHold));
        }
        s1 += coef * (target - s1);
        s2 += coef * (s1 - s2);
        return s2;
    }
};

/** RBJ peaking EQ, TDF-II. peak(A) and peak(1/A) are exact inverses -> clean pre/de-emphasis. */
struct PeakEQ
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set (float f, float fs, float q, float gainDb, bool inverse) noexcept
    {
        float A = std::pow (10.0f, gainDb / 40.0f);
        if (inverse) A = 1.0f / A;
        const float w = 2.0f * kPi * std::clamp (f, 20.0f, 0.45f * fs) / fs;
        const float alpha = std::sin (w) / (2.0f * q), c = std::cos (w);
        const float a0 = 1.0f / (1.0f + alpha / A);
        b0 = (1.0f + alpha * A) * a0; b1 = -2.0f * c * a0; b2 = (1.0f - alpha * A) * a0;
        a1 = -2.0f * c * a0;          a2 = (1.0f - alpha / A) * a0;
    }
    void reset() noexcept { z1 = z2 = 0.0f; }
    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

//==============================================================================
// Circuits
//==============================================================================
enum class Mode : int { Tube = 0, Transformer, Tape, Console, Acid, NumModes };

/** What makes each mode a different circuit (curves, gain staging, memory, filters) — not an EQ preset. */
struct Voicing
{
    float bA, bB;          // operating-point bias of stage A / B (even-harmonic asymmetry)
    float splitA;          // share of the drive (in dB) hitting stage A vs stage B
    float block;           // coupling-cap blocking (grid conduction memory -> breathing / bloom)
    float fInter;          // interstage treble loss (Hz)
    float wT, wA, wP;      // stage B curve family: tanh / algebraic / hard-knee p4
    float hyst;            // magnetic hysteresis in stage B
    float preFz, preFp;    // tape-style HF pre-emphasis (fz == fp -> flat)
    float tAmt;            // output transformer LF saturation
    float fPost;           // transformer HF resonance (Hz)
    float driveScale;
    float talkBase, biteBase;
};

inline Voicing voicingFor (Mode m) noexcept
{
    switch (m)
    {
        //                          bA     bB     split block fInter  wT  wA  wP  hyst   preFz preFp  tAmt  fPost  drv   talk  bite
        case Mode::Tube:        return { 0.28f,  0.12f, 0.60f, 0.60f,  9500, 1, 0, 0, 0.03f, 1000, 1000, 0.25f, 17500, 1.00f, 0.00f, 0.00f };
        case Mode::Transformer: return { 0.08f,  0.02f, 0.40f, 0.20f,  7500, 0, 1, 0, 0.10f, 1000, 1000, 1.00f, 15000, 1.10f, 0.00f, 0.00f };
        case Mode::Tape:        return { 0.05f,  0.03f, 0.50f, 0.15f, 11000, 1, 0, 0, 0.18f, 2400, 9000, 0.40f, 15500, 1.45f, 0.00f, 0.00f };
        case Mode::Console:     return { 0.06f,  0.00f, 0.30f, 0.10f, 13000, 0, 0, 1, 0.02f, 1000, 1000, 0.15f, 19000, 0.95f, 0.00f, 0.00f };
        case Mode::Acid:        return { 0.32f,  0.14f, 0.55f, 0.85f, 12500, 1, 0, 0, 0.04f, 1000, 1000, 0.10f, 18500, 1.20f, 0.35f, 0.25f };
        default: break;
    }
    return voicingFor (Mode::Tube);
}

struct EngineParams
{
    Mode  mode     = Mode::Tube;
    float drive    = 0.15f;   // 0..1
    float color    = 0.40f;   // 0 Warm .. 1 Excited
    float talk     = 0.30f;   // 0..1  resonance-tracking emphasis
    float bite     = 0.25f;   // 0..1  transient-driven attitude
    float subGuard = 100.0f;  // Hz, everything below stays clean
    float drift    = 0.30f;   // 0..1
};

//==============================================================================
class ColorEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kControlInterval = 16;

    void prepare (double oversampledRate, int numChannels)
    {
        fs = (float) oversampledRate;
        numCh = std::clamp (numChannels, 1, kMaxChannels);
        controlRate = fs / (float) kControlInterval;
        ctrlCoef = 1.0f - std::exp (-1.0f / (0.030f * controlRate));
        talkCoef = 1.0f - std::exp (-1.0f / (0.020f * controlRate));

        trackCoef = 1.0f - std::exp (-1.0f / (0.015f * fs));
        fastAtk = 1.0f - std::exp (-1.0f / (0.0008f * fs));
        fastRel = 1.0f - std::exp (-1.0f / (0.030f * fs));
        slowAtk = 1.0f - std::exp (-1.0f / (0.030f * fs));
        slowRel = 1.0f - std::exp (-1.0f / (0.250f * fs));
        biteSm  = 1.0f - std::exp (-1.0f / (0.002f * fs));
        blockRec = 1.0f - std::exp (-1.0f / (0.180f * fs));   // coupling-cap recovery ~180 ms

        for (int c = 0; c < kMaxChannels; ++c)
        {
            auto& ch = chans[(size_t) c];
            for (int d = 0; d < 3; ++d)
                ch.drift[(size_t) d].prepare (controlRate, 0xDA11u + 7919u * (uint32_t) (c * 3 + d));
            XorShift tol (0xA0D10u + 104729u * (uint32_t) c);
            ch.tolGain = tol.next(); ch.tolBias = tol.next(); ch.tolCut = tol.next();
            ch.interHP.setCutoff (22.0f, fs);
            ch.tLP.setCutoff (160.0f, fs);
            ch.playLP.setCutoff (6000.0f, fs);
            ch.dc.setCutoff (7.0f, fs);
        }
        snapSmoothers();
        reset();
    }

    void reset()
    {
        for (auto& ch : chans)
        {
            ch.s1a.reset(); ch.s1b.reset();
            ch.talkPre.reset(); ch.talkDe.reset(); ch.pre.reset(); ch.de.reset();
            ch.interHP.reset(); ch.interLP.reset(); ch.tLP.reset(); ch.playLP.reset(); ch.dc.reset(); ch.post.reset();
            ch.cc = 0; ch.play = 0; ch.e0 = ch.e1 = 0; ch.prev = 0;
            ch.envF = ch.envS = 0; ch.biteGain = 1;
            ch.logTalkF = std::log (1000.0f);
        }
        ctrlCounter = 0;
        colorPow = inPow = 0;
    }

    void setParams (const EngineParams& p) noexcept { target = p; }

    void snapSmoothers() noexcept
    {
        cur.drive = target.drive; cur.color = target.color; cur.talk = target.talk;
        cur.bite = target.bite; cur.drift = target.drift; cur.logSub = std::log (target.subGuard);
        cur.v = voicingFor (target.mode);
        cur.logPreFz = std::log (cur.v.preFz); cur.logPreFp = std::log (cur.v.preFp);
    }

    /** In place: input = oversampled dry, output = color only. */
    void process (float* const* data, int numChannels, int numSamples) noexcept
    {
        const int nc = std::min (numChannels, numCh);
        for (int start = 0; start < numSamples;)
        {
            if (ctrlCounter <= 0) { updateControl (nc); ctrlCounter = kControlInterval; }
            const int todo = std::min (ctrlCounter, numSamples - start);
            for (int c = 0; c < nc; ++c)
            {
                auto& ch = chans[(size_t) c];
                float* d = data[c] + start;
                for (int i = 0; i < todo; ++i)
                {
                    const float x = d[i];
                    const float col = (colorScale > 0.0f) ? processSample (ch, x) * colorScale : 0.0f;
                    inPow += x * x; colorPow += col * col;
                    d[i] = col;
                }
            }
            ctrlCounter -= todo;
            start += todo;
        }
    }

    float takeColorRatio() noexcept
    {
        const float r = inPow > 1.0e-9f ? std::sqrt (colorPow / inPow) : 0.0f;
        colorPow = inPow = 0;
        return r;
    }
    float getTalkFrequency() const noexcept { return std::exp (chans[0].logTalkF); }
    float getBiteActivity() const noexcept  { return chans[0].biteGain - 1.0f; }

private:
    struct Channel
    {
        SVF s1a, s1b;                      // sub guard LR4 high-pass
        PeakEQ talkPre, talkDe;
        Shelf1 pre, de;
        OnePole interHP, interLP, tLP, playLP, dc;
        SVF post;
        float cc = 0, play = 0;            // coupling-cap charge, hysteresis state
        float e0 = 0, e1 = 0, prev = 0;    // resonance tracker
        float envF = 0, envS = 0, biteGain = 1;
        float logTalkF = 6.9f;
        // control-rate
        float gA = 1, gB = 1, bA = 0, bB = 0, nA = 1, mk = 1, mkP = 1;
        float sBT = 0, nBT = 1, sBA = 0, nBA = 1, sBP = 0, nBP = 1;   // stage B s(b), 1/s'(b)
        float wT = 1, wA = 0, wP = 0, hyst = 0, block = 0, tAmt = 0, kT = 1, bite = 0;
        bool usePre = false, useTalk = false;
        std::array<DriftSource, 3> drift;
        float tolGain = 0, tolBias = 0, tolCut = 0;
    };

    struct Current
    {
        float drive = 0, color = 0, talk = 0, bite = 0, drift = 0, logSub = 0, logPreFz = 0, logPreFp = 0;
        Voicing v {};
    };

    static inline float algS (float v) noexcept { return v / std::sqrt (1.0f + v * v); }
    static inline float p4S  (float v) noexcept { const float v2 = v * v; return v / std::sqrt (std::sqrt (1.0f + v2 * v2)); }

    inline float stageB (const Channel& ch, float v) const noexcept
    {
        const float vb = v + ch.bB;
        float y = 0.0f;
        if (ch.wT > 1.0e-4f) y += ch.wT * (std::tanh (vb) - ch.sBT) * ch.nBT;
        if (ch.wA > 1.0e-4f) y += ch.wA * (algS (vb) - ch.sBA) * ch.nBA;
        if (ch.wP > 1.0e-4f) y += ch.wP * (p4S (vb) - ch.sBP) * ch.nBP;
        return y;
    }

    inline float processSample (Channel& ch, float x) noexcept
    {
        // Sub guard: only content above it is colored (the sub is rebuilt clean by the processor)
        ch.s1a.tick (x); ch.s1b.tick (ch.s1a.hpOut);
        const float s = ch.s1b.hpOut;

        // Resonance tracker: RMS frequency sqrt(E[s'^2]/E[s^2]) follows filter sweeps / formants
        const float ds = s - ch.prev; ch.prev = s;
        ch.e0 += trackCoef * (s * s - ch.e0);
        ch.e1 += trackCoef * (ds * ds - ch.e1);

        // BITE: fast vs slow envelope -> transient detector
        const float a = std::abs (s);
        ch.envF += (a > ch.envF ? fastAtk : fastRel) * (a - ch.envF);
        ch.envS += (a > ch.envS ? slowAtk : slowRel) * (a - ch.envS);
        const float trans = std::clamp (ch.envF / (ch.envS + 1.0e-5f) - 1.15f, 0.0f, 2.0f);
        ch.biteGain += biteSm * ((1.0f + ch.bite * 2.2f * trans) - ch.biteGain);

        // --- pre-emphasis (TALK, tape)
        float u = ch.useTalk ? ch.talkPre.process (s) : s;
        if (ch.usePre) u = ch.pre.process (u);

        // --- Stage A: asymmetric triode with grid-conduction blocking (inverting)
        const float vA = ch.gA * u + ch.cc;
        const float yA = -(std::tanh (vA + ch.bA) - std::tanh (ch.cc + ch.bA)) * ch.nA;
        const float over = vA + ch.bA - 0.55f;               // grid starts conducting
        if (over > 0.0f) ch.cc -= ch.block * 0.02f * over / (1.0f + over);   // coupling cap charges (grid current saturates)
        ch.cc = std::max (ch.cc, -0.9f);                                        // bias can shift, never gate the stage
        ch.cc -= blockRec * ch.cc;                           // ...and recovers: bloom / breathing

        // --- Interstage: coupling HP + treble loss
        float w = ch.interLP.lp (ch.interHP.hp (yA));

        // --- Stage B (inverting back), BITE pushes it harder on attacks
        const float gBd = ch.gB * ch.biteGain;
        float vB = gBd * w;
        if (ch.hyst > 1.0e-4f)
        {
            constexpr float W = 0.25f;
            const float dl = vB - ch.play;
            if (dl > W) ch.play = vB - W; else if (dl < -W) ch.play = vB + W;
            vB += ch.hyst * (vB * vB / (1.0f + vB * vB)) * (ch.playLP.lp (ch.play) - vB);
        }
        // Makeup: full normalisation at low drive, partial at high drive -> sustain/density like a real amp
        float y = -stageB (ch, vB) * ch.mk / (1.0f + ch.mkP * (ch.biteGain - 1.0f));

        // --- Output transformer: LF flux saturation
        if (ch.tAmt > 1.0e-4f)
        {
            const float yl = ch.tLP.lp (y);
            y += ch.tAmt * (algS (yl * ch.kT) / ch.kT - yl);
        }

        if (ch.usePre)  y = ch.de.process (y);
        if (ch.useTalk) y = ch.talkDe.process (y);

        ch.post.tick (y);                                    // transformer HF resonance / anti-harshness
        float col = ch.post.lpOut - s;
        col -= ch.dc.lp (col);
        return col;
    }

    template <typename T> inline void approach (T& c, T t) noexcept { c += (t - c) * ctrlCoef; }

    void updateControl (int nc) noexcept
    {
        approach (cur.drive, target.drive); approach (cur.color, target.color);
        approach (cur.talk, target.talk);   approach (cur.bite, target.bite);
        approach (cur.drift, target.drift);
        approach (cur.logSub, std::log (std::clamp (target.subGuard, 30.0f, 400.0f)));

        const Voicing tv = voicingFor (target.mode);
        auto& v = cur.v;
        approach (v.bA, tv.bA); approach (v.bB, tv.bB); approach (v.splitA, tv.splitA); approach (v.block, tv.block);
        approach (v.fInter, tv.fInter); approach (v.wT, tv.wT); approach (v.wA, tv.wA); approach (v.wP, tv.wP);
        approach (v.hyst, tv.hyst); approach (v.tAmt, tv.tAmt); approach (v.fPost, tv.fPost);
        approach (v.driveScale, tv.driveScale); approach (v.talkBase, tv.talkBase); approach (v.biteBase, tv.biteBase);
        approach (cur.logPreFz, std::log (tv.preFz)); approach (cur.logPreFp, std::log (tv.preFp));

        const float d = std::clamp (cur.drive, 0.0f, 1.0f);
        colorScale = std::min (1.0f, d / 0.10f);                 // Drive 0 -> exactly nothing
        const float driveDb = -4.0f + 40.0f * std::pow (d, 1.2f);   // 5-20 % = color, 30 %+ = drive, 70 %+ = attitude
        const float mkP = 1.0f - 0.55f * d;
        constexpr float kRef = 1.8f;

        // COLOR: Warm -> Rich -> Open -> Excited
        const float c = std::clamp (cur.color, 0.0f, 1.0f);
        const float biasMul  = lerp (1.5f, 0.6f, c);
        const float interMul = 0.6f * std::pow (2.7f, c);
        const float postMul  = lerp (0.85f, 1.1f, c);

        const float talk = std::clamp (cur.talk + v.talkBase * (0.3f + 0.7f * cur.talk), 0.0f, 1.0f);
        const float talkDb = 13.0f * talk;
        const float bite = std::clamp (cur.bite + v.biteBase * (0.3f + 0.7f * cur.bite), 0.0f, 1.0f);

        const float fSub = std::exp (cur.logSub);
        const float fz = std::exp (cur.logPreFz), fp = std::exp (cur.logPreFp);
        const bool usePre = std::abs (cur.logPreFz - cur.logPreFp) > 0.01f;

        for (int ci = 0; ci < nc; ++ci)
        {
            auto& ch = chans[(size_t) ci];
            ch.s1a.set (fSub, fs, 0.70710678f); ch.s1b.set (fSub, fs, 0.70710678f);

            const float da = cur.drift;
            const float dG = ch.drift[0].tick(), dB = ch.drift[1].tick(), dC = ch.drift[2].tick();
            const float gDrift = dbToGain (da * (0.30f * dG + 0.18f * ch.tolGain));
            const float bDrift = da * (0.035f * dB + 0.018f * ch.tolBias);
            const float cDrift = 1.0f + da * (0.06f * dC + 0.03f * ch.tolCut);

            ch.gA = dbToGain (driveDb * v.splitA) * kRef * v.driveScale * gDrift;
            ch.gB = dbToGain (driveDb * (1.0f - v.splitA));
            ch.mkP = mkP;
            ch.mk = std::pow (ch.gA * ch.gB, -mkP);
            ch.bA = std::clamp (v.bA * biasMul + bDrift, -0.8f, 0.8f);
            ch.bB = std::clamp (v.bB * biasMul - 0.5f * bDrift, -0.8f, 0.8f);
            const float tA = std::tanh (ch.bA);
            ch.nA = 1.0f / (1.0f - tA * tA);
            const float bb = ch.bB, tb = std::tanh (bb), b4 = bb * bb * bb * bb;
            ch.sBT = tb;         ch.nBT = 1.0f / (1.0f - tb * tb);
            ch.sBA = algS (bb);  ch.nBA = std::pow (1.0f + bb * bb, 1.5f);
            ch.sBP = p4S (bb);   ch.nBP = std::pow (1.0f + b4, 1.25f);
            ch.wT = v.wT; ch.wA = v.wA; ch.wP = v.wP;
            ch.hyst = v.hyst * (1.0f + 0.25f * da * dB);
            ch.block = v.block;
            ch.tAmt = v.tAmt;
            ch.kT = 1.0f + 5.0f * d;
            ch.bite = bite;

            ch.interLP.setCutoff (std::clamp (v.fInter * interMul * cDrift, 2500.0f, 22000.0f), fs);
            ch.post.set (std::clamp (v.fPost * postMul * cDrift, 9000.0f, 22000.0f), fs, 0.8f);

            ch.usePre = usePre;
            if (usePre) { ch.pre.design (fz, fp, fs, false); ch.de.design (fz, fp, fs, true); }

            // TALK: follow the resonance, emphasise it into the drive, restore after
            ch.useTalk = talkDb > 0.05f;
            if (ch.e0 > 1.0e-10f)
            {
                const float ratio = std::min (4.0f, ch.e1 / ch.e0);
                const float w = 2.0f * std::asin (std::min (1.0f, std::sqrt (ratio) * 0.5f));
                const float f = std::clamp (w * fs / (2.0f * kPi), 180.0f, 5000.0f);
                ch.logTalkF += talkCoef * (std::log (f) - ch.logTalkF);
            }
            if (ch.useTalk)
            {
                const float ft = std::exp (ch.logTalkF);
                ch.talkPre.set (ft, fs, 1.3f, talkDb, false);
                ch.talkDe.set  (ft, fs, 1.3f, talkDb, true);
            }
        }
    }

    float fs = 192000.0f, controlRate = 12000.0f, ctrlCoef = 0.1f, talkCoef = 0.1f;
    float trackCoef = 0, fastAtk = 0, fastRel = 0, slowAtk = 0, slowRel = 0, biteSm = 0, blockRec = 0;
    float colorScale = 0;
    int numCh = 2, ctrlCounter = 0;
    std::array<Channel, kMaxChannels> chans;
    EngineParams target;
    Current cur;
    float colorPow = 0, inPow = 0;
};

} // namespace dali
