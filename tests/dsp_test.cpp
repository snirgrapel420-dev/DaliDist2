// g++ -O2 -std=c++17 -I../Source dsp_test.cpp -o dsp_test && ./dsp_test [outdir]
// DaliDist v3 (multiband) — engine tests + renders. Emulates the processor's dry all-pass path exactly.
#include "DaliDistDSP.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dali;
constexpr double FS = 192000.0;

static const char* bandNames[] = { "LOW", "MID", "HIGH" };

static double tone (const std::vector<float>& x, double f)
{
    const size_t n = x.size(); const double w = 2 * M_PI * f / FS, c = 2 * std::cos (w);
    double s1 = 0, s2 = 0, ws = 0;
    for (size_t i = 0; i < n; ++i) { const double h = 0.5 - 0.5 * std::cos (2 * M_PI * i / (n - 1)); ws += h;
        const double s = x[i] * h + c * s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / (ws * 0.5);
}
static double db (double v) { return 20 * std::log10 (std::max (v, 1e-12)); }

/** Exactly what the processor does: wet = gateLow*AP_h AP_l LP_sub(x) + AP_h AP_l HP_sub(x) + engine(x). */
static std::vector<float> runMB (const std::vector<float>& x, const EngineParams& p)
{
    ColorEngine e; e.setParams (p); e.prepare (FS, 1); e.snapSmoothers();
    std::vector<float> y = x;
    for (size_t pos = 0; pos < y.size(); pos += 2048)
    {
        float* c1[1] = { y.data() + pos };
        e.process (c1, 1, (int) std::min<size_t> (2048, y.size() - pos));
    }
    const float fs = (float) FS, q = 0.70710678f;
    const float xl = clampXLow (p.xLow), xh = clampXHigh (p.xHigh, p.xLow), sub = clampSub (p.subGuard);
    SVF l1, l2, h1, h2, aLl, aHl, aLh, aHh;
    for (auto* f : { &l1, &l2, &h1, &h2 }) f->set (sub, fs, q);
    aLl.set (xl, fs, q); aHl.set (xh, fs, q); aLh.set (xl, fs, q); aHh.set (xh, fs, q);
    const float gLow = p.gateFor (Low);
    for (size_t i = 0; i < y.size(); ++i)
    {
        l1.tick (x[i]); l2.tick (l1.lpOut); h1.tick (x[i]); h2.tick (h1.hpOut);
        aLl.tick (l2.lpOut); aHl.tick (aLl.apOut);
        aLh.tick (h2.hpOut); aHh.tick (aLh.apOut);
        y[i] += gLow * aHl.apOut + aHh.apOut;
    }
    return y;
}
static EngineParams quiet()   // every band clean, nothing coloured
{
    EngineParams p; p.drift = 0; for (auto& b : p.band) b.drive = 0; return p;
}
static std::vector<float> sine (double f, double amp, double sec = 1.0)
{
    std::vector<float> x ((size_t) (sec * FS));
    for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (amp * std::sin (2 * M_PI * f * i / FS));
    return x;
}
static std::vector<float> tail (std::vector<float> y) { y.erase (y.begin(), y.begin() + (long) (y.size() / 3)); return y; }

// ---------- 303-style synth at 192 kHz ----------
static float polyBlep (double t, double dt)
{
    if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
    return 0.0f;
}
static std::vector<float> acidLine (double seconds)
{
    const int notes[16]   = { 33, 33, 45, 33, 36, 33, 43, 45, 33, 48, 33, 40, 33, 45, 31, 33 };
    const bool accent[16] = { 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0 };
    const double step = 60.0 / 140.0 / 4.0;
    const size_t n = (size_t) (seconds * FS);
    std::vector<float> out (n);
    SVF f; double ph = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / FS; const int s = (int) (t / step); const double ts = t - s * step;
        const int k = s % 16; const double hz = 440.0 * std::pow (2.0, (notes[k] - 69) / 12.0);
        const double dt = hz / FS; ph += dt; if (ph >= 1) ph -= 1;
        float saw = (float) (2 * ph - 1) - polyBlep (ph, dt);
        const double sweep = 0.5 - 0.5 * std::cos (2 * M_PI * t / seconds * 1.0);   // cutoff knob moves
        const double envA = accent[k] ? 1.6 : 1.0;
        const double cut = 180 + 2600 * sweep + 2200 * envA * std::exp (-ts / (accent[k] ? 0.09 : 0.16));
        if (i % 16 == 0) f.set ((float) cut, (float) FS, accent[k] ? 9.0f : 6.5f);
        f.tick (saw);
        const double gate = ts < step * 0.8 ? 1.0 : std::exp (-(ts - step * 0.8) / 0.004);
        const double amp = (accent[k] ? 1.0 : 0.72) * gate * (1 - std::exp (-ts / 0.002));
        out[i] = (float) (0.22 * amp * f.lpOut);
    }
    return out;
}

// ---------- 4x decimation (Blackman windowed sinc) + WAV ----------
static std::vector<float> decimate4 (const std::vector<float>& x)
{
    const int N = 255; std::vector<double> h (N); const double fc = 20500.0 / FS; double sum = 0;
    for (int i = 0; i < N; ++i) { const double m = i - (N - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc : std::sin (2 * M_PI * fc * m) / (M_PI * m);
        const double w = 0.42 - 0.5 * std::cos (2 * M_PI * i / (N - 1)) + 0.08 * std::cos (4 * M_PI * i / (N - 1));
        h[i] = sinc * w; sum += h[i]; }
    std::vector<float> y (x.size() / 4);
    for (size_t o = 0; o < y.size(); ++o) { double acc = 0; const long c = (long) o * 4;
        for (int i = 0; i < N; ++i) { const long j = c - i + (N - 1) / 2; if (j >= 0 && j < (long) x.size()) acc += h[i] * x[j]; }
        y[o] = (float) (acc / sum); }
    return y;
}
static double rms (const std::vector<float>& x) { double s = 0; for (float v : x) s += (double) v * v; return std::sqrt (s / x.size()); }
static void writeWav (const std::string& path, const std::vector<float>& x)
{
    FILE* f = std::fopen (path.c_str(), "wb"); if (! f) return;
    const uint32_t sr = 48000, n = (uint32_t) x.size(), bytes = n * 2;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); }; auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + bytes); std::fwrite ("WAVEfmt ", 1, 8, f);
    w32 (16); w16 (1); w16 (1); w32 (sr); w32 (sr * 2); w16 (2); w16 (16);
    std::fwrite ("data", 1, 4, f); w32 (bytes);
    for (float v : x) { const int s = (int) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f); w16 ((uint16_t) (int16_t) s); }
    std::fclose (f);
}

int main (int argc, char** argv)
{
    // 1) Null: all drives 0 -> engine output exactly 0
    {
        EngineParams p = quiet(); std::vector<float> x (48000); XorShift r (5); for (auto& v : x) v = r.next();
        ColorEngine e; e.setParams (p); e.prepare (FS, 1); e.snapSmoothers(); float* c[1] = { x.data() }; e.process (c, 1, (int) x.size());
        double mx = 0; for (float v : x) mx = std::max (mx, (double) std::abs (v));
        std::printf ("[null] all bands drive 0 -> max engine output %g  %s\n", mx, mx == 0 ? "PASS (bit-exact)" : "FAIL");
    }
    // 2) Flat sum: tones across the spectrum, clean -> magnitude must be unchanged
    {
        std::printf ("[flat] clean multiband magnitude (dB):");
        double worst = 0;
        for (double f : { 40.0, 90.0, 180.0, 250.0, 400.0, 1000.0, 3000.0, 6000.0, 15000.0 })
        {
            const double g = db (tone (tail (runMB (sine (f, 0.25), quiet())), f) / 0.25);
            worst = std::max (worst, std::abs (g)); std::printf (" %g:%+.3f", f, g);
        }
        std::printf ("  %s\n\n", worst < 0.05 ? "PASS" : "FAIL");
    }
    // 3) Band isolation: drive ONE band to 60 %, measure H3 of tones in the other bands
    std::printf ("[isolation] H3 (dB re fund) of test tones when only one band is driven 60%%\n");
    std::printf ("               %10s %10s %10s\n", "150 Hz", "1 kHz", "6 kHz");
    for (int b = 0; b < 3; ++b)
    {
        EngineParams p = quiet(); p.band[(size_t) b].drive = 0.6f; p.band[(size_t) b].mode = Mode::Tube;
        std::printf ("  %-5s driven ", bandNames[b]);
        for (double f : { 150.0, 1000.0, 6000.0 })
        {
            auto y = tail (runMB (sine (f, 0.25), p));
            const double h3 = 3 * f < 20000 ? db (tone (y, 3 * f) / tone (y, f)) : db (tone (y, 2 * f) / tone (y, f));
            std::printf (" %9.1f", h3);
        }
        std::printf ("\n");
    }
    // 4) Harmonic character per band at sweet-spot drives, default modes
    std::printf ("\n[per band] default modes (LOW Transformer, MID Tube, HIGH Tape), H2/H3 at 15%% / 40%%\n");
    {
        const double fr[3] = { 150.0, 800.0, 5000.0 };
        for (int b = 0; b < 3; ++b)
            for (float d : { 0.15f, 0.40f })
            {
                EngineParams p = quiet(); p.band[(size_t) b].drive = d;
                auto y = tail (runMB (sine (fr[b], 0.25), p)); const double f = tone (y, fr[b]);
                std::printf ("  %-4s %5.0f Hz  drive %2.0f%%  H2 %6.1f  H3 %6.1f\n", bandNames[b], fr[b], d * 100,
                             db (tone (y, 2 * fr[b]) / f), db (tone (y, 3 * fr[b]) / f));
            }
    }
    // 5) Solo + level
    {
        const double fr[4] = { 50.0, 150.0, 1000.0, 6000.0 };
        std::vector<float> x ((size_t) FS);
        for (size_t i = 0; i < x.size(); ++i) for (double f : fr) x[i] += (float) (0.1 * std::sin (2 * M_PI * f * i / FS));
        std::printf ("\n[solo/level] level of 50 Hz (sub) / 150 / 1k / 6k in dB\n");
        auto show = [&] (const char* name, const EngineParams& p) {
            auto y = tail (runMB (x, p)); std::printf ("  %-22s", name);
            for (double f : fr) { std::printf (" %7.1f", db (tone (y, f) / 0.1)); }
            std::printf ("\n"); };
        EngineParams p = quiet(); show ("clean", p);
        p.band[1].solo = true; show ("solo MID", p);
        p = quiet(); p.band[0].solo = true; show ("solo LOW (+sub)", p);
        p = quiet(); p.band[2].level = 2.0f; show ("HIGH level +6 dB", p);
        p = quiet(); p.band[1].on = false; p.band[1].drive = 1.0f; show ("MID off, drive 100%", p);
    }
    // 6) Stability
    {
        ColorEngine e; EngineParams p; p.master = 2; p.drift = 1; p.talk = 1; p.bite = 1; p.color = 1;
        e.setParams (p); e.prepare (FS, 2); e.snapSmoothers(); XorShift r (3); bool ok = true; double mx = 0;
        std::vector<float> L (4096), R (4096);
        for (int b = 0; b < 600; ++b)
        {
            for (int k = 0; k < 3; ++k) { p.band[(size_t) k].mode = (Mode) ((b / 20 + k) % 5); p.band[(size_t) k].drive = ((b + k) % 7) / 6.0f;
                p.band[(size_t) k].solo = (b / 50) % 4 == k; p.band[(size_t) k].on = (b / 30 + k) % 5 != 0; }
            p.subGuard = 30 + (b % 5) * 60; p.xLow = 80 + (b % 9) * 110; p.xHigh = 900 + (b % 11) * 1000; e.setParams (p);
            for (size_t i = 0; i < L.size(); ++i) { L[i] = 4 * r.next(); R[i] = (i % 97 == 0) ? 8.0f : 0.0f; }
            float* c[2] = { L.data(), R.data() }; e.process (c, 2, 4096);
            for (size_t i = 0; i < L.size(); ++i) { if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) ok = false;
                mx = std::max ({ mx, (double) std::abs (L[i]), (double) std::abs (R[i]) }); }
        }
        std::printf ("\n[stability] +12 dBFS noise/impulses, modes/solo/on/crossovers switching: %s (max %.2f)\n", ok ? "PASS" : "FAIL", mx);
    }
    // 7) Renders (48 kHz, loudness matched like auto gain)
    if (argc > 1)
    {
        const std::string dir = argv[1];
        const auto src = acidLine (8.0);
        const auto dry = decimate4 (src); const double ref = rms (dry);
        writeWav (dir + "/00_DRY.wav", dry);
        struct R { const char* name; Mode lm, mm, hm; float ld, md, hd, talk, bite; };
        const R rs[] = {
            { "01_MB_default",              Mode::Transformer, Mode::Tube, Mode::Tape,    0.12f, 0.20f, 0.12f, 0.30f, 0.25f },
            { "02_MB_cleanLow_acidMid",     Mode::Transformer, Mode::Acid, Mode::Tape,    0.05f, 0.35f, 0.15f, 0.60f, 0.50f },
            { "03_MB_fatLow_tubeMid",       Mode::Transformer, Mode::Tube, Mode::Console, 0.40f, 0.25f, 0.10f, 0.30f, 0.25f },
            { "04_MB_screamHigh",           Mode::Tube,        Mode::Tube, Mode::Acid,    0.05f, 0.15f, 0.55f, 0.50f, 0.60f },
            { "05_MB_allAcid_hot",          Mode::Acid,        Mode::Acid, Mode::Acid,    0.30f, 0.55f, 0.45f, 0.80f, 0.70f } };
        for (const auto& r : rs)
        {
            EngineParams p; p.drift = 0.3f; p.talk = r.talk; p.bite = r.bite;
            p.band[0].mode = r.lm; p.band[1].mode = r.mm; p.band[2].mode = r.hm;
            p.band[0].drive = r.ld; p.band[1].drive = r.md; p.band[2].drive = r.hd;
            auto y = decimate4 (runMB (src, p));
            const float g = (float) (ref / rms (y)); for (auto& v : y) v *= g;
            writeWav (dir + "/" + r.name + ".wav", y);
            std::printf ("rendered %-26s (auto gain %+.1f dB)\n", r.name, db (g));
        }
    }
    return 0;
}
