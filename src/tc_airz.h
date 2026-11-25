// ---------------------------------------------------------------------------
// The Airwindows filters: Air Z-Plane, Air X-Series and Air Y-Series
//
// Air Z-Plane, made to sound and act like the E-mu e6400 Ultra's filters:
//   ZLowpass      from ZLowpass2      a lowpass like the e6400 Ultra lowpass
//   ZHighpass     from ZHighpass2     a highpass like the e6400 Ultra highpass
//   ZBandpass     from ZBandpass2     a bandpass like the e6400 Ultra bandpass
//   ZNotch        from ZNotch2        a notch like the e6400 phaser
//   ZRegion       from ZRegion2       Airwindows' Region filter in the e6400 style
//   ZAcidLowpass  from ZAcidLowpass   "like if an e6400 went insane and grew shark teeth"
// Air X-Series, distorted digital EQ inspired by retro sampler DSP:
//   XLowpass, XHighpass, XBandpass, XNotch, XRegion (distorted staggered bandpasses)
// Air Y-Series, soft and smooth to nasty, edgy texture-varying filtering:
//   YLowpass, YHighpass, YBandpass, YNotch (the control-smoothed versions; the YNot plugins
//   are the same filters without smoothing)
//
// The algorithms are Chris Johnson's (Airwindows), used under the MIT licence below; the Y
// series' encode/decode curve is credited by Airwindows to torridgristle, also MIT. What changed
// in moving them here:
//   - they run at the engine's (oversampled) rate: frequencies are worked out in Hz, and the
//     values the originals derive from the sample rate (trims, clip factors, compensation) are
//     taken at 44.1 kHz, the rate they were voiced at, so the sound does not change with the
//     Oversampling setting
//   - coefficients are interpolated across each control block (the X series, which has no
//     smoothing of its own, included)
//   - the output is scaled so the passband sits near unity; nothing inside the filter changes,
//     so the clipping and saturation are the originals'
//   - the 32-bit output dither is left out (the engine works in float)
// Controls in the strip:
//   Air Z-Plane   Cutoff = Freq; Resonance = Input (drive into the e6400-style clipping);
//                 Morph = Poles, one to four stages
//                 ZRegion: Cutoff = First, Resonance = how far Last sits below, Morph = Poles (1-5)
//                 ZAcidLowpass: Morph = Meltdown
//   Air X-Series  Cutoff = Freq; Resonance = Gain (drive into the distortion);
//                 Morph = Nuke, one to five distorted stages
//                 XRegion: Cutoff = First, Resonance = how far Last sits below, Morph = Nuke
//   Air Y-Series  Cutoff = Freq; Resonance = Reson8; Morph = ResEdge (soft to nasty)
// ---------------------------------------------------------------------------
// Airwindows: MIT License
//
// Copyright (c) 2018 Chris Johnson
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ---------------------------------------------------------------------------
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>

namespace airz
{
// One list for all three families. New types go at the end of their family's block; the
// strips store a family (the filter model) and a position in it, so the numbers here are
// internal and can grow.
enum Type
{
    ZLowpass,
    ZHighpass,
    ZBandpass,
    ZNotch,
    ZRegion,
    ZAcidLowpass,
    XLowpass,
    XHighpass,
    XBandpass,
    XNotch,
    XRegion,
    YLowpass,
    YHighpass,
    YBandpass,
    YNotch,
    numTypes
};
enum Family
{
    FamZ,
    FamX,
    FamY,
    numFamilies
};
inline const char *typeName(int t)
{
    static const char *n[numTypes] = {"ZLowpass", "ZHighpass", "ZBandpass", "ZNotch", "ZRegion", "ZAcidLowpass",
                                      "XLowpass", "XHighpass", "XBandpass", "XNotch", "XRegion",
                                      "YLowpass", "YHighpass", "YBandpass", "YNotch"};
    return n[std::clamp(t, 0, (int)numTypes - 1)];
}
inline int familyFirst(int f) { return f == FamX ? XLowpass : f == FamY ? YLowpass : ZLowpass; }
inline int familyCount(int f) { return f == FamX ? 5 : f == FamY ? 4 : 6; }
inline int familyOf(int t) { return t >= YLowpass ? FamY : t >= XLowpass ? FamX : FamZ; }

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kVoiced = 44100.0; // the rate the originals' sample-rate-relative values are taken at

// the Freq knob (0..1) of the four single-type Z filters, and the frequency it gives at 44.1 kHz
struct FreqLaw
{
    double power, scale, offset; // Hz = B^power * scale + offset * 44100
};
inline FreqLaw freqLaw(int t)
{
    switch (t)
    {
    case ZHighpass:
        return {4.0, 9500.0, 0.00076};
    case ZBandpass:
        return {4.0, 14300.0, 0.00079};
    case ZNotch:
        return {3.0, 4700.0, 0.0009963};
    default:
        return {3.0, 18930.0, 0.00162};
    }
}
// the knob position for a frequency, clamped to the knob's range
inline double knobForHz(int t, double hz)
{
    FreqLaw L = freqLaw(t);
    double v = (hz - L.offset * kVoiced) / L.scale;
    return std::clamp(std::pow(std::max(0.0, v), 1.0 / L.power), 0.0, 1.0);
}
inline double hzForKnob(int t, double b)
{
    FreqLaw L = freqLaw(t);
    return std::pow(b, L.power) * L.scale + L.offset * kVoiced;
}

// One strip's Airwindows filter, stereo.
struct Filter
{
    // ----- set up -----
    void setRate(double r)
    {
        rate = r;
        overallscale = r / kVoiced;
        // the fixed ultrasonic lowpasses: the Z series' opamp stage (15.5 kHz), the Y series' (20 kHz)
        fixZ = lowpass(std::min(15500.0 / rate, 0.45), 0.935);
        fixY = lowpass(std::min(20000.0 / rate, 0.45), 0.7071);
        iirAmount = 0.00069 / overallscale;
    }
    void setType(int t)
    {
        type = std::clamp(t, 0, (int)numTypes - 1);
        reset();
    }
    void reset()
    {
        for (auto &s : st)
            s.clear();
        for (auto &d : df)
            d[0].clear(), d[1].clear();
        fixA.clear(), fixB.clear();
        iirL = iirR = 0.0;
        for (auto &v : bez)
            v = 0.0;
        bez[bez_cycle] = 1.0;
        snap = true;
    }

    // ----- once per control block: hz is the cutoff, reso and morph 0..1, steps the block length -----
    void setControls(double hz, double reso, double morph, int steps)
    {
        reso = std::clamp(reso, 0.0, 1.0), morph = std::clamp(morph, 0.0, 1.0);
        steps = std::max(1, steps);
        Target T;
        switch (familyOf(type))
        {
        case FamX:
            xTarget(T, hz, reso, morph);
            break;
        case FamY:
            yTarget(T, hz, reso, morph);
            break;
        default:
            if (type == ZAcidLowpass)
                acidTarget(T, hz, reso, morph);
            else if (type == ZRegion)
                regionTarget(T, hz, reso, morph);
            else
                singleTarget(T, hz, reso, morph);
        }
        if (snap)
        {
            cur = T;
            for (auto &d : dlt.v)
                d = 0.0;
            snap = false;
        }
        else
            for (int i = 0; i < nTarget; ++i)
                dlt.v[i] = (T.v[i] - cur.v[i]) / steps;
        // values the originals keep fixed across a buffer
        hold = T;
    }

    // ----- once per sample -----
    inline void tick(float &l, float &r)
    {
        for (int i = 0; i < nTarget; ++i)
            cur.v[i] += dlt.v[i];
        double L = l, R = r;
        switch (familyOf(type))
        {
        case FamX:
            xTick(L, R);
            break;
        case FamY:
            yTick(L, R);
            break;
        default:
            if (type == ZAcidLowpass)
                acidTick(L, R);
            else if (type == ZRegion)
                regionTick(L, R);
            else
                singleTick(L, R);
        }
        l = (float)std::clamp(L, -8.0, 8.0);
        r = (float)std::clamp(R, -8.0, 8.0);
    }

  private:
    double rate{96000}, overallscale{96000 / kVoiced}, iirAmount{0.00069};
    int type{ZLowpass};
    bool snap{true};

    struct Coef
    {
        double a0{1}, a1{0}, a2{0}, b1{0}, b2{0};
    };
    struct State // transposed direct form II, stereo
    {
        double sL1{0}, sL2{0}, sR1{0}, sR2{0};
        void clear() { sL1 = sL2 = sR1 = sR2 = 0.0; }
    };
    struct DF1 // direct form I, one channel: the X series puts its distortion in the feedback
    {
        double x1{0}, x2{0}, y1{0}, y2{0};
        void clear() { x1 = x2 = y1 = y2 = 0.0; }
    };
    Coef fixZ, fixY;
    State fixA, fixB, st[5];
    DF1 df[5][2];
    double iirL{0}, iirR{0};

    // everything that is interpolated across a block, as one flat list
    enum
    {
        tC0 = 0, // five biquads' coefficients, 5 x (a0 a1 a2 b1 b2)
        tInTrim = 25, tOutTrim, tWet, tGain,
        tCutoff, tOver, tUnder, tMeltdown, // ZAcidLowpass
        tPow,                              // the Y series' ResEdge
        nTarget
    };
    struct Target
    {
        double v[nTarget]{};
        // fixed per block in the originals
        double trim{1}, clip{1}, thr{1}, alpha{1}, clipK[5]{1, 1, 1, 1, 1}, compK[5]{1, 1, 1, 1, 1}, w[5]{1, 0, 0, 0, 0};
    };
    Target cur, dlt, hold;

    static Coef lowpass(double f, double q)
    {
        double K = std::tan(kPi * f), norm = 1.0 / (1.0 + K / q + K * K);
        Coef c;
        c.a0 = K * K * norm, c.a1 = 2.0 * c.a0, c.a2 = c.a0;
        c.b1 = 2.0 * (K * K - 1.0) * norm, c.b2 = (1.0 - K / q + K * K) * norm;
        return c;
    }
    static Coef highpass(double f, double q)
    {
        double K = std::tan(kPi * f), norm = 1.0 / (1.0 + K / q + K * K);
        Coef c;
        c.a0 = norm, c.a1 = -2.0 * c.a0, c.a2 = c.a0;
        c.b1 = 2.0 * (K * K - 1.0) * norm, c.b2 = (1.0 - K / q + K * K) * norm;
        return c;
    }
    static Coef bandpass(double f, double q)
    {
        double K = std::tan(kPi * f), norm = 1.0 / (1.0 + K / q + K * K);
        Coef c;
        c.a0 = K / q * norm, c.a1 = 0.0, c.a2 = -c.a0;
        c.b1 = 2.0 * (K * K - 1.0) * norm, c.b2 = (1.0 - K / q + K * K) * norm;
        return c;
    }
    static Coef notch(double f, double q)
    {
        double K = std::tan(kPi * f), norm = 1.0 / (1.0 + K / q + K * K);
        Coef c;
        c.a0 = (1.0 + K * K) * norm, c.a1 = 2.0 * (K * K - 1.0) * norm, c.a2 = c.a0;
        c.b1 = c.a1, c.b2 = (1.0 - K / q + K * K) * norm;
        return c;
    }
    void putCoef(Target &T, int k, const Coef &c) const
    {
        double *p = T.v + tC0 + 5 * k;
        p[0] = c.a0, p[1] = c.a1, p[2] = c.a2, p[3] = c.b1, p[4] = c.b2;
    }
    double safeF(double hz) const { return std::clamp(hz / rate, 1.0e-6, 0.45); }
    // the four-stage "progressive wet" the originals share: 0..4 -> one stage at a time
    static void stagesFrom(double x, double &a, double &b, double &c, double &d)
    {
        a = 1.0, b = 1.0, c = 1.0, d = x;
        if (d < 1.0)
            a = d, b = 0.0, c = 0.0, d = 0.0;
        else if (d < 2.0)
            b = d - 1.0, c = 0.0, d = 0.0;
        else if (d < 3.0)
            c = d - 2.0, d = 0.0;
        else
            d -= 3.0;
    }

    // The small-signal level of a chain of stages, for setting the output level: each stage is
    // pre x B(z) / (1 + fb (b1 z^-1 + b2 z^-2)) x post, blended with its own input by w; the
    // loudest of the probe frequencies (angles in radians a sample) is returned.
    struct SS
    {
        Coef c;
        double pre{1}, fb{1}, post{1}, w{1};
    };
    static double chainPeak(const SS *s, int n, const double *probe, int np)
    {
        double best = 0.0;
        for (int p = 0; p < np; ++p)
        {
            std::complex<double> z1 = std::polar(1.0, -probe[p]), z2 = z1 * z1, tot(1.0, 0.0);
            for (int k = 0; k < n; ++k)
            {
                if (s[k].w <= 0.0)
                    continue;
                const Coef &c = s[k].c;
                std::complex<double> num = c.a0 + c.a1 * z1 + c.a2 * z2, den = 1.0 + s[k].fb * (c.b1 * z1 + c.b2 * z2);
                if (std::abs(den) < 1e-9)
                    den = 1e-9;
                std::complex<double> h = s[k].pre * s[k].post * num / den;
                tot *= s[k].w * h + (1.0 - s[k].w);
            }
            best = std::max(best, std::abs(tot));
        }
        return best;
    }

    // ----- ZLowpass2, ZHighpass2, ZBandpass2, ZNotch2 -------------------------------------------
    void singleTarget(Target &T, double hz, double reso, double morph) const
    {
        double B = knobForHz(type, hz);
        double fHz = hzForKnob(type, B), f = safeF(fHz), f44 = fHz / kVoiced;
        double clip;
        Coef c;
        switch (type)
        {
        case ZHighpass:
            clip = 1.0; // the highpass leaves its clip factor out
            c = highpass(f, 1.0);
            break;
        case ZBandpass:
            clip = 1.0 - (1.0 - B) * 0.304;
            c = bandpass(f, 0.314);
            break;
        case ZNotch:
            clip = 0.91 - (1.0 - B) * 0.15;
            c = notch(f, 0.618033988749894848204586);
            break;
        default:
            clip = 1.212 - (1.0 - B) * 0.496;
            c = lowpass(f, 0.7071);
            break;
        }
        putCoef(T, 0, c);
        T.trim = 0.1 + 3.712 * f44;
        T.clip = clip;
        // Input: 0.1 is unity; Resonance takes it to 0.18 (about +20 dB into the clipping)
        double A = 0.1 + 0.08 * reso;
        double inTrim = A * 10.0;
        inTrim *= inTrim, inTrim *= inTrim;
        // Poles: one stage at Morph 0, four at Morph 1 (the original's wet^2 * 4 from 1 to 4)
        double wet = (1.0 + 3.0 * morph) / 4.0;
        T.v[tInTrim] = inTrim;
        T.v[tWet] = wet;
        // Output: the originals' Output 1.0 is x10; instead, take back the trim and the clip
        // divisions so the passband sits near unity, and half the Input drive (in dB)
        double stages = 1.0 + 3.0 * morph; // 1..4, the stages in use
        double small = T.trim / std::pow(clip, stages + 1.0);
        T.v[tOutTrim] = 1.0 / (small * std::sqrt(inTrim));
        T.v[tGain] = 1.0;
    }
    inline void stage(const double *c, State &s, double &L, double &R, bool clampOut)
    {
        double y = L * c[0] + s.sL1;
        if (clampOut)
            y = std::clamp(y, -1.0, 1.0);
        s.sL1 = L * c[1] - y * c[3] + s.sL2;
        s.sL2 = L * c[2] - y * c[4];
        L = y;
        y = R * c[0] + s.sR1;
        if (clampOut)
            y = std::clamp(y, -1.0, 1.0);
        s.sR1 = R * c[1] - y * c[3] + s.sR2;
        s.sR2 = R * c[2] - y * c[4];
        R = y;
    }
    static inline void flush(State &s)
    {
        if (std::fabs(s.sL1) < 1e-30)
            s.sL1 = 0;
        if (std::fabs(s.sL2) < 1e-30)
            s.sL2 = 0;
        if (std::fabs(s.sR1) < 1e-30)
            s.sR1 = 0;
        if (std::fabs(s.sR2) < 1e-30)
            s.sR2 = 0;
    }
    inline void opamp(double &L, double &R)
    {
        if (std::fabs(iirL) < 1.18e-37)
            iirL = 0.0;
        iirL = iirL * (1.0 - iirAmount) + L * iirAmount;
        L -= iirL;
        if (std::fabs(iirR) < 1.18e-37)
            iirR = 0.0;
        iirR = iirR * (1.0 - iirAmount) + R * iirAmount;
        R -= iirR;
        const double c[5] = {fixZ.a0, fixZ.a1, fixZ.a2, fixZ.b1, fixZ.b2};
        stage(c, fixA, L, R, false);
        L = std::clamp(L, -1.0, 1.0);
        L -= L * L * L * L * L * 0.1768;
        R = std::clamp(R, -1.0, 1.0);
        R -= R * R * R * R * R * 0.1768;
        stage(c, fixB, L, R, false);
        flush(fixA), flush(fixB);
    }
    inline void singleTick(double &L, double &R)
    {
        const double *c = cur.v + tC0;
        double inTrim = cur.v[tInTrim], outTrim = cur.v[tOutTrim], wet = cur.v[tWet];
        double trim = hold.trim, clip = hold.clip;
        double aWet, bWet, cWet, dWet;
        stagesFrom(wet * 4.0, aWet, bWet, cWet, dWet);
        double dryL = L, dryR = R, allDryL = L, allDryR = R;
        if (inTrim != 1.0)
            L *= inTrim, R *= inTrim;
        L = std::clamp(L, -1.0, 1.0), R = std::clamp(R, -1.0, 1.0);
        L *= trim, R *= trim;
        L /= clip, R /= clip;
        stage(c, st[0], L, R, true);
        dryL = L, dryR = R;
        const double w[3] = {bWet, cWet, dWet};
        for (int k = 0; k < 3; ++k)
        {
            if (w[k] <= 0.0)
                continue;
            L /= clip, R /= clip;
            stage(c, st[k + 1], L, R, true);
            L = L * w[k] + dryL * (1.0 - w[k]);
            R = R * w[k] + dryR * (1.0 - w[k]);
            dryL = L, dryR = R;
        }
        for (auto &s : st)
            flush(s);
        L /= clip, R /= clip;
        opamp(L, R);
        L *= outTrim, R *= outTrim;
        if (aWet != 1.0)
            L = L * aWet + allDryL * (1.0 - aWet), R = R * aWet + allDryR * (1.0 - aWet);
    }

    // ----- ZRegion2 ---------------------------------------------------------------------------
    void regionTarget(Target &T, double hz, double reso, double morph) const
    {
        // First and Last are knobs whose frequency is knob^3 x 20 kHz
        double high = std::clamp(std::cbrt(std::max(0.0, hz) / 20000.0), 0.0, 1.0);
        double low = high * (1.0 - 0.6 * reso); // Last: up to about four octaves below First
        double mid = (high + low) * 0.5;
        double spread = 1.001 - std::fabs(high - low);
        double D = morph; // Poles
        const double knob[5] = {high, (high + mid) * 0.5, mid, (mid + low) * 0.5, low};
        double w[5] = {1, 1, 1, 1, 1}, probe[5];
        stagesFrom(D * 4.0, w[1], w[2], w[3], w[4]);
        SS ss[5];
        for (int k = 0; k < 5; ++k)
        {
            double fHz = std::pow(knob[k], 3) * 20000.0;
            double f44 = std::max(fHz / kVoiced, 0.00009), f = safeF(f44 * kVoiced);
            double comp = std::sqrt(f44) * 6.4 * spread, clip = 0.75 + f44 * D * 37.0;
            Coef c = bandpass(f, 0.7071);
            putCoef(T, k, c);
            T.clipK[k] = clip, T.compK[k] = comp;
            ss[k] = {c, clip, 1.0, 1.0 / comp, w[k]};
            probe[k] = 2.0 * kPi * f;
        }
        double gain = chainPeak(ss, 5, probe, 5); // the loudest point of the whole chain
        T.v[tInTrim] = 1.0;
        T.v[tWet] = D;
        T.v[tOutTrim] = 1.0;
        T.v[tGain] = 1.0 / std::clamp(gain, 0.05, 20.0);
    }
    inline void regionStage(int k, double &L, double &R)
    {
        const double *c = cur.v + tC0 + 5 * k;
        double clip = hold.clipK[k], comp = hold.compK[k];
        L = std::sin(std::clamp(L * clip, -1.57079633, 1.57079633));
        R = std::sin(std::clamp(R * clip, -1.57079633, 1.57079633));
        stage(c, st[k], L, R, false);
        L /= comp, R /= comp;
    }
    inline void regionTick(double &L, double &R)
    {
        double w[4];
        stagesFrom(cur.v[tWet] * 4.0, w[0], w[1], w[2], w[3]);
        regionStage(0, L, R);
        double nL = L, nR = R;
        for (int k = 0; k < 4; ++k)
        {
            if (w[k] <= 0.0)
                continue;
            regionStage(k + 1, L, R);
            L = L * w[k] + nL * (1.0 - w[k]);
            R = R * w[k] + nR * (1.0 - w[k]);
            nL = L, nR = R;
        }
        for (auto &s : st)
            flush(s);
        opamp(L, R);
        L *= cur.v[tGain], R *= cur.v[tGain];
    }

    // ----- ZAcidLowpass -------------------------------------------------------------------------
    enum
    {
        bez_AL, bez_AR, bez_BL, bez_BR, bez_CL, bez_CR, bez_DL, bez_DR, bez_SampL, bez_SampR, bez_cycle, bez_total
    };
    double bez[bez_total]{};
    void acidTarget(Target &T, double hz, double reso, double morph) const
    {
        // Cutoff knob A: the curve is redrawn A^3 / 2 times a sample at 44.1 kHz, which puts the
        // corner near a fifth of that rate
        double A = std::clamp(std::cbrt(std::max(0.0, hz) * 2.0 * 5.0 / kVoiced), 0.0, 1.0);
        double cutoff = std::pow(A, 3.0) / overallscale;
        cutoff /= (2.0 / std::pow(overallscale, 0.5 - ((overallscale - 1.0) * 0.0375)));
        cutoff = std::clamp(cutoff, 0.0001, 1.0);
        double E = 0.1 + 0.08 * reso; // Input
        double inTrim = E * 10.0;
        inTrim *= inTrim, inTrim *= inTrim;
        T.v[tCutoff] = cutoff;
        T.v[tOver] = 0.0;  // Over, at its default
        T.v[tUnder] = 1.0; // Under, at its default
        T.v[tMeltdown] = morph;
        T.v[tInTrim] = inTrim;
        T.v[tOutTrim] = 1.0 / std::sqrt(inTrim); // Output 1.0, less half the drive (in dB)
        T.v[tGain] = 1.0;
    }
    inline void acidTick(double &L, double &R)
    {
        double cutoff = cur.v[tCutoff], over = cur.v[tOver], under = cur.v[tUnder], meltdown = cur.v[tMeltdown];
        double inTrim = cur.v[tInTrim], outTrim = cur.v[tOutTrim];
        double acidTrim = 1.0 - std::pow(cutoff * 0.5, 1.0 / (cutoff * 0.5));
        bez[bez_cycle] += cutoff;
        bez[bez_SampL] += L * cutoff;
        bez[bez_SampR] += R * cutoff;
        if (bez[bez_cycle] > 1.0)
        {
            bez[bez_cycle] -= 1.0;
            bez[bez_DL] = bez[bez_CL], bez[bez_CL] = bez[bez_BL], bez[bez_BL] = bez[bez_AL];
            bez[bez_AL] = bez[bez_SampL] * (1.0 - meltdown) + L * meltdown;
            bez[bez_SampL] = 0.0;
            bez[bez_DR] = bez[bez_CR], bez[bez_CR] = bez[bez_BR], bez[bez_BR] = bez[bez_AR];
            bez[bez_AR] = bez[bez_SampR] * (1.0 - meltdown) + R * meltdown;
            bez[bez_SampR] = 0.0;
        }
        double X = bez[bez_cycle] * acidTrim, iX = 1.0 - X;
        double midL = bez[bez_DL] * iX * iX * iX + bez[bez_CL] * 3.0 * iX * iX * X + bez[bez_BL] * 3.0 * iX * X * X + bez[bez_AL] * X * X * X;
        double midR = bez[bez_DR] * iX * iX * iX + bez[bez_CR] * 3.0 * iX * iX * X + bez[bez_BR] * 3.0 * iX * X * X + bez[bez_AR] * X * X * X;
        L = midL * under + (L - midL) * over;
        R = midR * under + (R - midR) * over;
        if (inTrim != 1.0)
            L *= inTrim, R *= inTrim;
        opamp(L, R);
        if (outTrim != 1.0)
            L *= outTrim, R *= outTrim;
        for (auto &v : bez)
            if (std::fabs(v) < 1e-30)
                v = 0.0;
    }

    // ----- XLowpass, XHighpass, XBandpass, XNotch, XRegion ------------------------------------
    void xTarget(Target &T, double hz, double reso, double morph) const
    {
        const double nuke = morph;
        double w[5] = {1, 0, 0, 0, 0};
        stagesFrom(nuke * 4.0, w[1], w[2], w[3], w[4]);
        for (int k = 0; k < 5; ++k)
            T.w[k] = w[k];
        double total = 1.0, gain = 1.0;
        SS ss[5];
        double probe[7];
        int np = 0;
        if (type == XRegion)
        {
            double high = std::clamp(std::cbrt(std::max(0.0, hz) / 20000.0), 0.0, 1.0);
            double low = high * (1.0 - 0.6 * reso); // Last, as on ZRegion
            double mid = (high + low) * 0.5, spread = 1.001 - std::fabs(high - low);
            const double knob[5] = {high, (high + mid) * 0.5, mid, (mid + low) * 0.5, low};
            for (int k = 0; k < 5; ++k)
            {
                double f44 = std::max(std::pow(knob[k], 3) * 20000.0 / kVoiced, 0.00009);
                double f = safeF(f44 * kVoiced);
                Coef c = bandpass(f, 0.7071);
                putCoef(T, k, c);
                T.compK[k] = std::sqrt(f44) * 6.4 * spread, T.clipK[k] = 0.75 + f44 * nuke * 37.0;
                ss[k] = {c, T.clipK[k], 1.0, 1.0 / T.compK[k], w[k]};
                probe[np++] = 2.0 * kPi * f;
            }
            // low settings divide by a very small compensation, so the chain can gain a lot
            total = std::clamp(chainPeak(ss, 5, probe, np), 0.25, 1000.0);
        }
        else
        {
            // Gain: (A + 0.5)^4 with A at 0.5 is unity; Resonance takes A to 1 (about +14 dB)
            double A = 0.5 + 0.5 * reso;
            gain = std::pow(A + 0.5, 4);
            const double power = type == XHighpass ? 5 : type == XBandpass ? 4 : type == XNotch ? 3 : 2;
            const double fMin = type == XBandpass ? 0.00005 : type == XNotch ? 0.0003 : 0.001;
            double f44 = std::clamp(hz / 20000.0, 0.0, 1.0) * 20000.0 / kVoiced;
            f44 = std::max(f44, fMin);
            double f = safeF(f44 * kVoiced), comp = std::sqrt(f44) * 6.4, wRef = 0.0;
            Coef c;
            double pre = 1, fb = 1, post = 1, pre0 = 1;
            switch (type)
            {
            case XHighpass:
            {
                double B = std::pow(std::clamp(f44 * kVoiced / 20000.0, 0.0, 1.0), 1.0 / power);
                T.thr = std::max(0.1, 1.0 - nuke * std::sqrt(B)); // the original reaches 0 (silence) at the very top
                c = highpass(f, 0.7071), wRef = kPi;
                break;
            }
            case XBandpass:
                T.clip = 0.768 + f44 * nuke * 37.0;
                // the sine in the feedback: scaled so it bends at the level it does at 44.1 kHz
                T.alpha = std::tan(kPi * f) / std::tan(kPi * std::min(f44, 0.45));
                c = bandpass(f, 0.7071), wRef = 2.0 * kPi * f;
                pre = pre0 = T.clip, post = 1.0 / comp;
                break;
            case XNotch:
                T.clip = 1.0 + std::pow(nuke, 3) * 2.0;
                c = notch(f, 0.7071), wRef = 0.0;
                pre = T.clip;
                break;
            default: // XLowpass
                // in the feedback, so it is taken at the running rate, as the original does: the
                // filter's margin from instability then stays the same at any rate
                T.clip = 1.0 + f * f * nuke * 32.0;
                // the sine in the feedback: scaled so it bends at the level it does at 44.1 kHz (the
                // lowpass's margin grows with K squared, so the bend point goes with K)
                T.alpha = std::tan(kPi * f) / std::tan(kPi * std::min(f44, 0.45));
                c = lowpass(f, 0.7071), wRef = 0.0;
                fb = T.clip, post = T.clip / comp;
                break;
            }
            T.compK[0] = comp;
            for (int k = 0; k < 5; ++k)
                putCoef(T, k, c);
            (void)wRef;
            for (int k = 0; k < 5; ++k)
                ss[k] = {c, k ? pre : pre0, fb, post, w[k]};
            double w0 = 2.0 * kPi * f;
            for (double m : {0.0005 / w0, 0.25, 0.5, 1.0, 2.0, 4.0})
                probe[np++] = std::clamp(w0 * m, 1e-4, 0.9 * kPi);
            probe[np++] = 0.9 * kPi;
            // The first stage is levelled in full. The Nuke stages saturate on anything loud (they
            // act like a compressor), so what they add is left in; only what they take away is taken
            // back up.
            SS first[1] = {ss[0]};
            first[0].w = 1.0;
            double g0 = chainPeak(first, 1, probe, np), all = chainPeak(ss, 5, probe, np);
            total = std::clamp(g0 * std::min(1.0, all / std::max(g0, 1e-9)), 0.25, 8.0);
        }
        T.v[tInTrim] = gain;
        T.v[tGain] = 1.0 / (total * std::sqrt(gain)); // near unity, less half the Gain (in dB)
    }
    inline double xStage(int k, double x, DF1 &s)
    {
        const double *c = cur.v + tC0 + 5 * k;
        const double half = 1.57079633;
        double y;
        switch (type)
        {
        case XHighpass:
            y = c[0] * x + c[1] * s.x1 + c[2] * s.x2 - c[3] * s.y1 - c[4] * s.y2;
            s.x2 = s.x1, s.x1 = x;
            y = std::clamp(y, -hold.thr, hold.thr);
            s.y2 = s.y1, s.y1 = y;
            return y;
        case XBandpass:
            x *= hold.clip;
            y = c[0] * x + c[2] * s.x2 - c[3] * s.y1 - c[4] * s.y2;
            s.x2 = s.x1, s.x1 = x;
            y = std::clamp(y, -half / hold.alpha, half / hold.alpha);
            s.y2 = s.y1, s.y1 = std::sin(hold.alpha * y) / hold.alpha;
            return y / hold.compK[0];
        case XNotch:
            if (k > 0)
                x *= hold.clip;
            x = std::sin(std::clamp(x, -half, half));
            y = c[0] * x + c[1] * s.x1 + c[2] * s.x2 - c[3] * s.y1 - c[4] * s.y2;
            s.x2 = s.x1, s.x1 = x;
            s.y2 = s.y1, s.y1 = y;
            return y;
        case XRegion:
            x = std::sin(std::clamp(x * hold.clipK[k], -half, half));
            y = c[0] * x + c[2] * s.x2 - c[3] * s.y1 - c[4] * s.y2;
            s.x2 = s.x1, s.x1 = x;
            s.y2 = s.y1, s.y1 = y;
            return y / hold.compK[k];
        default: // XLowpass: the clip and the sine sit in the feedback
            y = c[0] * x + c[1] * s.x1 + c[2] * s.x2 - c[3] * s.y1 - c[4] * s.y2;
            s.x2 = s.x1, s.x1 = x;
            y = std::clamp(y * hold.clip, -half / hold.alpha, half / hold.alpha);
            s.y2 = s.y1, s.y1 = std::sin(hold.alpha * y) / hold.alpha;
            return y / hold.compK[0];
        }
    }
    static inline void flush(DF1 &s)
    {
        if (std::fabs(s.y1) < 1e-30)
            s.y1 = 0;
        if (std::fabs(s.y2) < 1e-30)
            s.y2 = 0;
        if (std::fabs(s.x1) < 1e-30)
            s.x1 = 0;
        if (std::fabs(s.x2) < 1e-30)
            s.x2 = 0;
    }
    inline void xTick(double &L, double &R)
    {
        double g = cur.v[tInTrim];
        if (g != 1.0)
            L *= g, R *= g;
        for (int k = 0; k < 5; ++k)
        {
            double w = hold.w[k];
            if (w <= 0.0)
                continue;
            double nL = L, nR = R;
            L = xStage(k, L, df[k][0]);
            R = xStage(k, R, df[k][1]);
            if (w < 1.0)
                L = L * w + nL * (1.0 - w), R = R * w + nR * (1.0 - w);
            flush(df[k][0]), flush(df[k][1]);
        }
        L *= cur.v[tGain], R *= cur.v[tGain];
    }

    // ----- YLowpass, YHighpass, YBandpass, YNotch -----------------------------------------------
    void yTarget(Target &T, double hz, double reso, double morph) const
    {
        double f = safeF(std::clamp(hz, 15.0, 20000.0));
        double C = reso, q = C * C * 15.0 + 0.5571;
        switch (type)
        {
        case YHighpass:
            putCoef(T, 0, highpass(f, q));
            break;
        case YBandpass:
            putCoef(T, 0, bandpass(f, q));
            break;
        case YNotch:
            C = 0.1 + 0.9 * reso; // the notch's Q goes to nothing at 0, which empties it
            putCoef(T, 0, notch(f, C * C * 15.0 + 0.0001));
            break;
        default:
            putCoef(T, 0, lowpass(f, q));
            break;
        }
        T.v[tPow] = std::pow(morph + 0.9, 4); // ResEdge: 0.1 is neutral (1.0)
        T.v[tInTrim] = 1.0;  // Gain at its default
        T.v[tOutTrim] = 1.0; // Output at its default
        T.v[tGain] = 1.0;
    }
    static inline double yCurve(double x, double p)
    {
        if (x > 1.0)
            return 1.0;
        if (x > 0.0)
            return 1.0 - std::pow(1.0 - x, p);
        if (x < -1.0)
            return -1.0;
        if (x < 0.0)
            return -1.0 + std::pow(1.0 + x, p);
        return x;
    }
    inline void yTick(double &L, double &R)
    {
        const double fz[5] = {fixY.a0, fixY.a1, fixY.a2, fixY.b1, fixY.b2};
        double p = std::max(cur.v[tPow], 0.01);
        stage(fz, fixA, L, R, false);
        L = yCurve(L, p), R = yCurve(R, p); // encode/decode courtesy of torridgristle (MIT)
        stage(cur.v + tC0, st[0], L, R, false);
        L = yCurve(L, 1.0 / p), R = yCurve(R, 1.0 / p);
        stage(fz, fixB, L, R, false);
        flush(fixA), flush(fixB), flush(st[0]);
    }
};
} // namespace airz
