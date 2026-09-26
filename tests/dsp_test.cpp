// g++ -O2 -std=c++17 -I../Source dsp_test.cpp -o dsp_test && ./dsp_test [outdir]
// Measures the engine and renders a synthetic 303 line through it (48 kHz WAVs, loudness matched).
#include "DaliDistDSP.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dali;
constexpr double FS = 192000.0;
static const char* modeNames[] = { "TUBE", "TRANSFORMER", "TAPE", "CONSOLE", "ACID" };

static double tone (const std::vector<float>& x, double f)
{
    const size_t n = x.size(); const double w = 2 * M_PI * f / FS, c = 2 * std::cos (w);
    double s1 = 0, s2 = 0, ws = 0;
    for (size_t i = 0; i < n; ++i) { const double h = 0.5 - 0.5 * std::cos (2 * M_PI * i / (n - 1)); ws += h;
        const double s = x[i] * h + c * s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / (ws * 0.5);
}
static double db (double v) { return 20 * std::log10 (std::max (v, 1e-12)); }

static std::vector<float> process (const std::vector<float>& x, EngineParams p, float* talkF = nullptr)
{
    ColorEngine e; e.setParams (p); e.prepare (FS, 1); e.snapSmoothers();
    std::vector<float> y = x; float* ch[1] = { y.data() };
    for (size_t pos = 0; pos < y.size(); pos += 2048)
    {
        float* c1[1] = { y.data() + pos };
        e.process (c1, 1, (int) std::min<size_t> (2048, y.size() - pos));
        if (talkF) talkF[pos / 2048] = e.getTalkFrequency();
    }
    (void) ch;
    SVF l1, l2, h1, h2; for (auto* f : { &l1, &l2, &h1, &h2 }) f->set (p.subGuard, (float) FS, 0.70710678f);
    for (size_t i = 0; i < y.size(); ++i)
    {
        l1.tick (x[i]); l2.tick (l1.lpOut); h1.tick (x[i]); h2.tick (h1.hpOut);
        y[i] += l2.lpOut + h2.hpOut;    // AP(x) + color
    }
    return y;
}

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
    // 1) Null test
    {
        EngineParams p; p.drive = 0; std::vector<float> x (48000); XorShift r (5); for (auto& v : x) v = r.next();
        ColorEngine e; e.setParams (p); e.prepare (FS, 1); e.snapSmoothers(); float* c[1] = { x.data() }; e.process (c, 1, (int) x.size());
        double mx = 0; for (float v : x) mx = std::max (mx, (double) std::abs (v));
        std::printf ("[null] drive 0 -> max color %g  %s\n\n", mx, mx == 0 ? "PASS" : "FAIL");
    }
    // 2) Harmonics, 330 Hz @ -12 dBFS, default COLOR/TALK/BITE
    std::printf ("[harmonics] 330 Hz @ -12 dBFS (dB re fundamental)\n");
    for (int m = 0; m < 5; ++m)
    {
        std::printf ("%s\n", modeNames[m]);
        for (float d : { 0.05f, 0.12f, 0.25f, 0.5f, 1.0f })
        {
            std::vector<float> x ((size_t) FS); for (size_t i = 0; i < x.size(); ++i) x[i] = 0.25f * (float) std::sin (2 * M_PI * 330 * i / FS);
            EngineParams p; p.mode = (Mode) m; p.drive = d; p.drift = 0;
            auto y = process (x, p); y.erase (y.begin(), y.begin() + y.size() / 3);
            const double f = tone (y, 330);
            std::printf ("  drive %3.0f%%  fund %+5.1f  H2 %6.1f H3 %6.1f H4 %6.1f H5 %6.1f H7 %6.1f\n", d * 100, db (f / 0.25),
                db (tone (y, 660) / f), db (tone (y, 990) / f), db (tone (y, 1320) / f), db (tone (y, 1650) / f), db (tone (y, 2310) / f));
        }
    }
    // 3) TALK tracker follows a filter sweep
    {
        std::vector<float> x = acidLine (4.0); std::vector<float> tf (x.size() / 2048 + 1);
        EngineParams p; p.mode = Mode::Acid; p.drive = 0.3f; p.talk = 0.8f; process (x, p, tf.data());
        std::printf ("\n[talk tracker] tracked resonance (Hz) across the sweep: ");
        for (int k = 0; k < 8; ++k) std::printf ("%5.0f ", tf[(size_t) (k * tf.size() / 8 + 20)]);
        std::printf ("\n");
    }
    // 4) Stability
    {
        ColorEngine e; EngineParams p; p.drive = 1; p.drift = 1; p.talk = 1; p.bite = 1; p.color = 1;
        e.setParams (p); e.prepare (FS, 2); e.snapSmoothers(); XorShift r (3); bool ok = true; double mx = 0;
        std::vector<float> L (4096), R (4096);
        for (int b = 0; b < 400; ++b)
        {
            p.mode = (Mode) (b / 20 % 5); p.drive = (b % 7) / 6.0f; p.subGuard = 40 + (b % 5) * 60; e.setParams (p);
            for (size_t i = 0; i < L.size(); ++i) { L[i] = 4 * r.next(); R[i] = (i % 97 == 0) ? 8.0f : 0.0f; }
            float* c[2] = { L.data(), R.data() }; e.process (c, 2, 4096);
            for (size_t i = 0; i < L.size(); ++i) { if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) ok = false;
                mx = std::max ({ mx, (double) std::abs (L[i]), (double) std::abs (R[i]) }); }
        }
        std::printf ("\n[stability] %s (max |color| %.2f)\n", ok ? "PASS" : "FAIL", mx);
    }
    // 5) Renders
    if (argc > 1)
    {
        const std::string dir = argv[1];
        const auto src = acidLine (8.0);
        const auto dry = decimate4 (src); const double ref = rms (dry);
        writeWav (dir + "/00_DRY.wav", dry);
        struct R { const char* name; Mode m; float d, c, t, b; };
        const R rs[] = {
            { "01_TUBE_drive15",          Mode::Tube,        0.15f, 0.40f, 0.30f, 0.25f },
            { "02_ACID_drive25",          Mode::Acid,        0.25f, 0.60f, 0.60f, 0.50f },
            { "03_ACID_drive55_talk90",   Mode::Acid,        0.55f, 0.70f, 0.90f, 0.70f },
            { "04_TRANSFORMER_drive30",   Mode::Transformer, 0.30f, 0.30f, 0.20f, 0.20f },
            { "05_TAPE_drive40",          Mode::Tape,        0.40f, 0.40f, 0.20f, 0.10f },
            { "06_CONSOLE_drive20",       Mode::Console,     0.20f, 0.50f, 0.10f, 0.10f },
            { "07_ACID_drive25_talk0",    Mode::Acid,        0.25f, 0.60f, 0.00f, 0.50f } };
        for (const auto& r : rs)
        {
            EngineParams p; p.mode = r.m; p.drive = r.d; p.color = r.c; p.talk = r.t; p.bite = r.b; p.drift = 0.3f;
            auto y = decimate4 (process (src, p));
            const float g = (float) (ref / rms (y)); for (auto& v : y) v *= g;   // same loudness as dry (auto gain)
            writeWav (dir + "/" + r.name + ".wav", y);
            std::printf ("rendered %-26s (auto gain %+.1f dB)\n", r.name, db (g));
        }
    }
    return 0;
}
