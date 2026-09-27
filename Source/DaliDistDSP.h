/*
  ==============================================================================
    DaliDist v3 — Multiband Analog Chain Engine
    Dali Audio

    Pure C++ (no JUCE dependency) so the engine can be unit-tested on its own.
    Runs at the OVERSAMPLED rate and outputs ONLY the difference it makes.

    Split (per channel), all Linkwitz-Riley 24 dB/oct:
        s    = HP_sub(x)                      everything below SUB GUARD stays clean
        low  = AP_h( LP_l(s) )                AP_h keeps LOW in phase with MID+HIGH
        mid  = LP_h( HP_l(s) )
        high = HP_h( HP_l(s) )
        low + mid + high == AP_h AP_l s       (flat magnitude, no notches)

    Each band runs its own copy of the v2 analog chain with its own Mode / Drive / Level.
    Output = sum_b( gate_b * band_out_b ) - sum_b( band_b )
    The processor adds this to AP_h AP_l AP_sub(dry), computed on the latency-aligned dry, so
        wet = AP_h AP_l LP_sub(dry) + sum_b gate_b * band_out_b
    Drive 0 on every band (Level 0 dB, no solo) -> output is exactly 0.

    Chain (per band, per channel):
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
    float lpOut = 0, hpOut = 0, apOut = 0;   // apOut: 2nd-order all-pass == LR4 LP + LR4 HP at Q 0.7071

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
        apOut = v0 - 2.0f * k * v1;
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

//==============================================================================
// Parameters
//==============================================================================
constexpr int kNumBands = 3;
enum Band : int { Low = 0, Mid = 1, High = 2 };

struct BandParams
{
    Mode  mode  = Mode::Tube;
    float drive = 0.15f;   // 0..1 (before the master Drive multiplier)
    float level = 1.0f;    // linear band output level
    bool  on    = true;    // false -> band passes clean
    bool  solo  = false;
};

struct EngineParams
{
    std::array<BandParams, kNumBands> band { { { Mode::Transformer, 0.12f, 1.0f, true, false },
                                               { Mode::Tube,        0.20f, 1.0f, true, false },
                                               { Mode::Tape,        0.12f, 1.0f, true, false } } };
    float master   = 1.0f;    // 0..2, multiplies every band's drive
    float color    = 0.40f;   // 0 Warm .. 1 Excited
    float talk     = 0.30f;   // 0..1 resonance-tracking emphasis
    float bite     = 0.25f;   // 0..1 transient-driven attitude
    float subGuard = 90.0f;   // Hz, everything below stays clean
    float xLow     = 250.0f;  // Hz, LOW | MID
    float xHigh    = 3000.0f; // Hz, MID | HIGH
    float drift    = 0.30f;   // 0..1

    bool anySolo() const noexcept { return band[0].solo || band[1].solo || band[2].solo; }
    /** Gate for a band: 1 when audible, 0 when another band is soloed. */
    float gateFor (int b) const noexcept { return (! anySolo() || band[(size_t) b].solo) ? 1.0f : 0.0f; }
};

/** Crossover limits shared by the engine and the processor so both always use the same values. */
inline float clampXLow (float f) noexcept               { return std::clamp (f, 80.0f, 1000.0f); }
inline float clampXHigh (float fHigh, float fLow) noexcept { return std::max (std::clamp (fHigh, 800.0f, 12000.0f), clampXLow (fLow) * 2.0f); }
inline float clampSub (float f) noexcept                { return std::clamp (f, 20.0f, 300.0f); }

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
        blockRec = 1.0f - std::exp (-1.0f / (0.180f * fs));

        for (int c = 0; c < kMaxChannels; ++c)
        {
            for (int b = 0; b < kNumBands; ++b)
            {
                auto& ch = chains[(size_t) c][(size_t) b];
                for (int d = 0; d < 3; ++d)
                    ch.drift[(size_t) d].prepare (controlRate, 0xDA11u + 7919u * (uint32_t) (c * 9 + b * 3 + d));
                XorShift tol (0xA0D10u + 104729u * (uint32_t) (c * 3 + b));   // each band = its own components
                ch.tolGain = tol.next(); ch.tolBias = tol.next(); ch.tolCut = tol.next();
                ch.interHP.setCutoff (22.0f, fs);
                ch.tLP.setCutoff (160.0f, fs);
                ch.playLP.setCutoff (6000.0f, fs);
                ch.dc.setCutoff (7.0f, fs);
            }
        }
        snapSmoothers();
        reset();
    }

    void reset()
    {
        for (auto& sp : splits)
            for (auto* f : { &sp.sub1, &sp.sub2, &sp.l1a, &sp.l1b, &sp.h1a, &sp.h1b, &sp.ap2, &sp.l2a, &sp.l2b, &sp.h2a, &sp.h2b })
                f->reset();

        for (auto& perCh : chains)
            for (auto& ch : perCh)
            {
                ch.talkPre.reset(); ch.talkDe.reset(); ch.pre.reset(); ch.de.reset();
                ch.interHP.reset(); ch.interLP.reset(); ch.tLP.reset(); ch.playLP.reset(); ch.dc.reset(); ch.post.reset();
                ch.cc = 0; ch.play = 0; ch.e0 = ch.e1 = 0; ch.prev = 0;
                ch.envF = ch.envS = 0; ch.biteGain = 1;
                ch.logTalkF = std::log (1000.0f);
            }
        ctrlCounter = 0;
        inPow = 0;
        bandColorPow.fill (0.0f);
    }

    void setParams (const EngineParams& p) noexcept { target = p; }

    void snapSmoothers() noexcept
    {
        cur.color = target.color; cur.talk = target.talk; cur.bite = target.bite; cur.drift = target.drift;
        cur.logSub = std::log (clampSub (target.subGuard));
        cur.logXL  = std::log (clampXLow (target.xLow));
        cur.logXH  = std::log (clampXHigh (target.xHigh, target.xLow));
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& bc = bandCur[(size_t) b];
            const auto& tp = target.band[(size_t) b];
            bc.drive = std::clamp (tp.drive * target.master, 0.0f, 1.0f);
            bc.level = tp.level;
            bc.act   = tp.on ? 1.0f : 0.0f;
            bc.gate  = target.gateFor (b);
            bc.v = voicingFor (tp.mode);
            bc.logPreFz = std::log (bc.v.preFz); bc.logPreFp = std::log (bc.v.preFp);
        }
    }

    /** In place: input = oversampled dry, output = the difference the plugin makes (see header). */
    void process (float* const* data, int numChannels, int numSamples) noexcept
    {
        const int nc = std::min (numChannels, numCh);
        for (int start = 0; start < numSamples;)
        {
            if (ctrlCounter <= 0) { updateControl (nc); ctrlCounter = kControlInterval; }
            const int todo = std::min (ctrlCounter, numSamples - start);

            for (int c = 0; c < nc; ++c)
            {
                auto& sp = splits[(size_t) c];
                auto& chs = chains[(size_t) c];
                float* d = data[c] + start;

                for (int i = 0; i < todo; ++i)
                {
                    const float x = d[i];

                    // --- Split
                    sp.sub1.tick (x);  sp.sub2.tick (sp.sub1.hpOut);
                    const float s = sp.sub2.hpOut;

                    sp.l1a.tick (s);   sp.l1b.tick (sp.l1a.lpOut);
                    sp.h1a.tick (s);   sp.h1b.tick (sp.h1a.hpOut);
                    const float rest = sp.h1b.hpOut;
                    sp.ap2.tick (sp.l1b.lpOut);

                    sp.l2a.tick (rest); sp.l2b.tick (sp.l2a.lpOut);
                    sp.h2a.tick (rest); sp.h2b.tick (sp.h2a.hpOut);

                    const float bands[kNumBands] = { sp.ap2.apOut, sp.l2b.lpOut, sp.h2b.hpOut };

                    // --- Per band: v2 chain, level, on/off, solo gate
                    float out = 0.0f;
                    for (int b = 0; b < kNumBands; ++b)
                    {
                        auto& ch = chs[(size_t) b];
                        const float xb = bands[b];

                        float y = xb;
                        if (ch.colorScale > 0.0f && ch.act > 1.0e-4f)
                            y += processChain (ch, xb) * ch.colorScale;

                        const float fin  = xb + ch.act * (ch.level * y - xb);   // band ON/OFF crossfade
                        const float diff = ch.gate * fin - xb;                  // solo gate
                        bandColorPow[(size_t) b] += (fin - xb) * (fin - xb);
                        out += diff;
                    }
                    inPow += x * x;
                    d[i] = out;
                }
            }
            ctrlCounter -= todo;
            start += todo;
        }
    }

    /** How much each band is changing the sound, relative to the full input (for the UI glows). */
    float takeBandColorRatio (int b) noexcept
    {
        const float r = inPow > 1.0e-9f ? std::sqrt (bandColorPow[(size_t) b] / inPow) : 0.0f;
        bandColorPow[(size_t) b] = 0;
        return r;
    }
    /** Call once per block after reading all bands. */
    void endMeterBlock() noexcept { inPow = 0; }

    float getTalkFrequency (int b = Mid) const noexcept { return std::exp (chains[0][(size_t) b].logTalkF); }
    float getBiteActivity() const noexcept
    {
        float m = 0.0f;
        for (const auto& ch : chains[0]) m = std::max (m, ch.biteGain - 1.0f);
        return m;
    }

private:
    struct Split
    {
        SVF sub1, sub2;            // sub guard LR4 HP
        SVF l1a, l1b, h1a, h1b;    // LOW | MID crossover
        SVF ap2;                   // LOW all-pass at MID | HIGH
        SVF l2a, l2b, h2a, h2b;    // MID | HIGH crossover
    };

    /** One band on one channel: the complete v2 analog chain. */
    struct Chain
    {
        PeakEQ talkPre, talkDe;
        Shelf1 pre, de;
        OnePole interHP, interLP, tLP, playLP, dc;
        SVF post;
        float cc = 0, play = 0;
        float e0 = 0, e1 = 0, prev = 0;
        float envF = 0, envS = 0, biteGain = 1;
        float logTalkF = 6.9f;
        // control-rate
        float gA = 1, gB = 1, bA = 0, bB = 0, nA = 1, mk = 1, mkP = 1;
        float sBT = 0, nBT = 1, sBA = 0, nBA = 1, sBP = 0, nBP = 1;
        float wT = 1, wA = 0, wP = 0, hyst = 0, block = 0, tAmt = 0, kT = 1, bite = 0;
        float colorScale = 0, level = 1, act = 1, gate = 1;
        bool usePre = false, useTalk = false;
        std::array<DriftSource, 3> drift;
        float tolGain = 0, tolBias = 0, tolCut = 0;
    };

    struct BandCurrent
    {
        float drive = 0, level = 1, act = 1, gate = 1, logPreFz = 0, logPreFp = 0;
        Voicing v {};
    };

    struct Current
    {
        float color = 0, talk = 0, bite = 0, drift = 0, logSub = 0, logXL = 0, logXH = 0;
    };

    static inline float algS (float v) noexcept { return v / std::sqrt (1.0f + v * v); }
    static inline float p4S  (float v) noexcept { const float v2 = v * v; return v / std::sqrt (std::sqrt (1.0f + v2 * v2)); }

    inline float stageB (const Chain& ch, float v) const noexcept
    {
        const float vb = v + ch.bB;
        float y = 0.0f;
        if (ch.wT > 1.0e-4f) y += ch.wT * (std::tanh (vb) - ch.sBT) * ch.nBT;
        if (ch.wA > 1.0e-4f) y += ch.wA * (algS (vb) - ch.sBA) * ch.nBA;
        if (ch.wP > 1.0e-4f) y += ch.wP * (p4S (vb) - ch.sBP) * ch.nBP;
        return y;
    }

    /** v2 chain, unchanged apart from the sub guard now living in the split. Returns color of this band. */
    inline float processChain (Chain& ch, float s) noexcept
    {
        const float ds = s - ch.prev; ch.prev = s;
        ch.e0 += trackCoef * (s * s - ch.e0);
        ch.e1 += trackCoef * (ds * ds - ch.e1);

        const float a = std::abs (s);
        ch.envF += (a > ch.envF ? fastAtk : fastRel) * (a - ch.envF);
        ch.envS += (a > ch.envS ? slowAtk : slowRel) * (a - ch.envS);
        const float trans = std::clamp (ch.envF / (ch.envS + 1.0e-5f) - 1.15f, 0.0f, 2.0f);
        ch.biteGain += biteSm * ((1.0f + ch.bite * 2.2f * trans) - ch.biteGain);

        float u = ch.useTalk ? ch.talkPre.process (s) : s;
        if (ch.usePre) u = ch.pre.process (u);

        const float vA = ch.gA * u + ch.cc;
        const float yA = -(std::tanh (vA + ch.bA) - std::tanh (ch.cc + ch.bA)) * ch.nA;
        const float over = vA + ch.bA - 0.55f;
        if (over > 0.0f) ch.cc -= ch.block * 0.02f * over / (1.0f + over);
        ch.cc = std::max (ch.cc, -0.9f);
        ch.cc -= blockRec * ch.cc;

        float w = ch.interLP.lp (ch.interHP.hp (yA));

        const float gBd = ch.gB * ch.biteGain;
        float vB = gBd * w;
        if (ch.hyst > 1.0e-4f)
        {
            constexpr float W = 0.25f;
            const float dl = vB - ch.play;
            if (dl > W) ch.play = vB - W; else if (dl < -W) ch.play = vB + W;
            vB += ch.hyst * (vB * vB / (1.0f + vB * vB)) * (ch.playLP.lp (ch.play) - vB);
        }
        float y = -stageB (ch, vB) * ch.mk / (1.0f + ch.mkP * (ch.biteGain - 1.0f));

        if (ch.tAmt > 1.0e-4f)
        {
            const float yl = ch.tLP.lp (y);
            y += ch.tAmt * (algS (yl * ch.kT) / ch.kT - yl);
        }

        if (ch.usePre)  y = ch.de.process (y);
        if (ch.useTalk) y = ch.talkDe.process (y);

        ch.post.tick (y);
        float col = ch.post.lpOut - s;
        col -= ch.dc.lp (col);
        return col;
    }

    template <typename T> inline void approach (T& c, T t) noexcept { c += (t - c) * ctrlCoef; }

    static void approachVoicing (Voicing& v, const Voicing& tv, float k) noexcept
    {
        auto ap = [k] (float& c, float t) { c += (t - c) * k; };
        ap (v.bA, tv.bA); ap (v.bB, tv.bB); ap (v.splitA, tv.splitA); ap (v.block, tv.block);
        ap (v.fInter, tv.fInter); ap (v.wT, tv.wT); ap (v.wA, tv.wA); ap (v.wP, tv.wP);
        ap (v.hyst, tv.hyst); ap (v.tAmt, tv.tAmt); ap (v.fPost, tv.fPost);
        ap (v.driveScale, tv.driveScale); ap (v.talkBase, tv.talkBase); ap (v.biteBase, tv.biteBase);
    }

    void updateControl (int nc) noexcept
    {
        // --- globals
        approach (cur.color, target.color); approach (cur.talk, target.talk);
        approach (cur.bite, target.bite);   approach (cur.drift, target.drift);
        approach (cur.logSub, std::log (clampSub (target.subGuard)));
        approach (cur.logXL,  std::log (clampXLow (target.xLow)));
        approach (cur.logXH,  std::log (clampXHigh (target.xHigh, target.xLow)));

        const float fSub = std::exp (cur.logSub);
        const float xl = std::exp (cur.logXL);
        const float xh = std::max (std::exp (cur.logXH), xl * 2.0f);
        for (int c = 0; c < nc; ++c)
        {
            auto& sp = splits[(size_t) c];
            sp.sub1.set (fSub, fs, 0.70710678f); sp.sub2.set (fSub, fs, 0.70710678f);
            for (auto* f : { &sp.l1a, &sp.l1b, &sp.h1a, &sp.h1b }) f->set (xl, fs, 0.70710678f);
            for (auto* f : { &sp.ap2, &sp.l2a, &sp.l2b, &sp.h2a, &sp.h2b }) f->set (xh, fs, 0.70710678f);
        }

        const float col = std::clamp (cur.color, 0.0f, 1.0f);
        const float biasMul  = lerp (1.5f, 0.6f, col);
        const float interMul = 0.6f * std::pow (2.7f, col);
        const float postMul  = lerp (0.85f, 1.1f, col);
        constexpr float kRef = 1.8f;

        // --- bands
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& bc = bandCur[(size_t) b];
            const auto& tp = target.band[(size_t) b];
            approach (bc.drive, std::clamp (tp.drive * target.master, 0.0f, 1.0f));
            approach (bc.level, tp.level);
            approach (bc.act,  tp.on ? 1.0f : 0.0f);
            approach (bc.gate, target.gateFor (b));
            const Voicing tv = voicingFor (tp.mode);
            approachVoicing (bc.v, tv, ctrlCoef);
            approach (bc.logPreFz, std::log (tv.preFz)); approach (bc.logPreFp, std::log (tv.preFp));
            const auto& v = bc.v;

            const float d = std::clamp (bc.drive, 0.0f, 1.0f);
            const float colorScale = std::min (1.0f, d / 0.10f);
            const float driveDb = -4.0f + 40.0f * std::pow (d, 1.2f);
            const float mkP = 1.0f - 0.55f * d;

            const float talk = std::clamp (cur.talk + v.talkBase * (0.3f + 0.7f * cur.talk), 0.0f, 1.0f);
            const float talkDb = 13.0f * talk;
            const float bite = std::clamp (cur.bite + v.biteBase * (0.3f + 0.7f * cur.bite), 0.0f, 1.0f);

            const float fz = std::exp (bc.logPreFz), fp = std::exp (bc.logPreFp);
            const bool usePre = std::abs (bc.logPreFz - bc.logPreFp) > 0.01f;

            // TALK tracks inside the band it lives in
            const float talkLo = (b == Low) ? 40.0f : (b == Mid ? xl : xh);
            const float talkHi = (b == Low) ? xl * 1.5f : (b == Mid ? xh : 12000.0f);

            for (int c = 0; c < nc; ++c)
            {
                auto& ch = chains[(size_t) c][(size_t) b];
                ch.colorScale = colorScale;
                ch.level = bc.level; ch.act = bc.act; ch.gate = bc.gate;
                if (colorScale <= 0.0f || bc.act <= 1.0e-4f) continue;   // nothing to compute

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

                ch.useTalk = talkDb > 0.05f;
                if (ch.e0 > 1.0e-10f)
                {
                    const float ratio = std::min (4.0f, ch.e1 / ch.e0);
                    const float w = 2.0f * std::asin (std::min (1.0f, std::sqrt (ratio) * 0.5f));
                    const float f = std::clamp (w * fs / (2.0f * kPi), std::max (40.0f, talkLo), std::max (talkLo + 1.0f, talkHi));
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
    }

    float fs = 192000.0f, controlRate = 12000.0f, ctrlCoef = 0.1f, talkCoef = 0.1f;
    float trackCoef = 0, fastAtk = 0, fastRel = 0, slowAtk = 0, slowRel = 0, biteSm = 0, blockRec = 0;
    int numCh = 2, ctrlCounter = 0;
    std::array<Split, kMaxChannels> splits;
    std::array<std::array<Chain, kNumBands>, kMaxChannels> chains;
    std::array<BandCurrent, kNumBands> bandCur;
    EngineParams target;
    Current cur;
    float inPow = 0;
    std::array<float, kNumBands> bandColorPow {};
};

} // namespace dali
