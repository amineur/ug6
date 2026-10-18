#include "DspChain.h"

#include <juce_core/juce_core.h>

#include <cstring>
#include <limits>
#include <sstream>

DspChain::DspChain()
{
    // Default crossover frequencies — per spec.
    xoverFreq[0].store (100.0f);
    xoverFreq[1].store (300.0f);
    xoverFreq[2].store (800.0f);
    xoverFreq[3].store (2500.0f);
    xoverFreq[4].store (8000.0f);

    // EQ defaults: low-shelf @ 100 Hz, two peakings, high-shelf @ 8 kHz.
    eq[0].freq.store (100.0f);   eq[0].q.store (0.7f);
    eq[1].freq.store (400.0f);   eq[1].q.store (1.0f);
    eq[2].freq.store (2500.0f);  eq[2].q.store (1.0f);
    eq[3].freq.store (8000.0f);  eq[3].q.store (0.7f);

    // Comp defaults — gentle, mostly transparent until the user dials in.
    for (int i = 0; i < kNumBands; ++i)
    {
        comp[i].thresholdDb.store (-18.0f);
        comp[i].ratio.store       (2.0f);
        comp[i].attackMs.store    (20.0f);
        comp[i].releaseMs.store   (150.0f);
    }
}

void DspChain::prepare (double sr, int blockSize, int channels)
{
    sampleRate   = sr;
    maxBlockSize = blockSize;
    numChannels  = juce::jmax (1, channels);

    juce::dsp::ProcessSpec spec { sr,
                                  static_cast<juce::uint32> (blockSize),
                                  static_cast<juce::uint32> (numChannels) };

    for (auto& f : hpf)
    {
        f.prepare (spec);
        f.reset();
    }

    for (auto& band : eqFilters)
        for (auto& f : band)
        {
            f.prepare (spec);
            f.reset();
        }

    for (auto& bandCh : bandFilters)
        for (auto& bf : bandCh)
        {
            bf.hp.prepare (spec);
            bf.lp.prepare (spec);
            bf.hp.reset();
            bf.lp.reset();
            bf.hp.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
            bf.lp.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        }

    agc.prepare (sr);
    for (auto& c : bandComps) c.prepare (sr);
    limiter.prepare (sr);
    clipOs = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) numChannels, 1,
                 juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    clipOs->initProcessing ((size_t) blockSize);
    resetLookahead (juce::jlimit (16, kMaxLook - 1, (int) std::round (limLookaheadMs.load() * 0.001 * sr)));

    for (auto& f : agcSplit) { f.prepare (spec); f.reset(); }
    agcSplit[0].setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    agcSplit[1].setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    lastAgcSplit = 0.0f;
    agcLow.prepare (sr);
    {
        juce::dsp::ProcessSpec mono { sr, (juce::uint32) blockSize, 1 };
        sideHp[0].prepare (mono);
        *sideHp[0].coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 200.0f);
        sideHp[0].reset();
        for (auto& ch : kw)
        {
            ch[0].prepare (mono); ch[1].prepare (mono);
            *ch[0].coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 1681.97, 0.7071, juce::Decibels::decibelsToGain (4.0f));
            *ch[1].coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 38.13, 0.5003);
            ch[0].reset(); ch[1].reset();
        }
    }
    bandLimEnv.fill (0.0f);
    loudBlockLen = juce::jmax (1, (int) (sr * 0.1)); loudBlockSum = 0.0; loudBlockN = 0; loudBlocks.fill (0.0); loudBlockIdx = 0;
    loudMinuteInit = false;

    midBuf.setSize (numChannels, blockSize, false, true, true);
    midBuf.clear();
    lastMidFreq = 0.0f;
    for (auto& ch : midBp) for (auto& q : ch) q.reset();

    for (auto& b : bandScratch)
    {
        b.setSize (numChannels, blockSize, false, true, true);
        b.clear();
    }

    // RMS smoothing time-constant ≈ 30 ms for VU-style ballistics.
    rmsAlpha = static_cast<float> (1.0 - std::exp (-1.0 / (0.030 * sr)));

    // Force all coefficient updates on first process().
    lastHpfFreq = -1.0f;
    for (auto& v : lastEqFreq) v = -1.0f;
    for (auto& v : lastEqGain) v = -1e9f;
    for (auto& v : lastEqQ)    v = -1.0f;
    for (auto& v : lastXover)  v = -1.0f;

    updateAllCoefficients();
}

void DspChain::reset()
{
    for (auto& f : hpf) f.reset();
    for (auto& band : eqFilters) for (auto& f : band) f.reset();
    for (auto& bandCh : bandFilters)
        for (auto& bf : bandCh) { bf.hp.reset(); bf.lp.reset(); }

    for (auto& ch : midBp) for (auto& q : ch) q.reset();
    rmsInLSmooth = rmsInRSmooth = rmsOutLSmooth = rmsOutRSmooth = 0.0f;
}

void DspChain::resetLookahead (int n)
{
    laLen = n; laPos = 0; laSum = (double) n; laRel = 1.0f;
    for (auto& c : laSig) c.fill (0.0f);
    for (auto& c : laMid) c.fill (0.0f);
    laReq.fill (1.0f); laMin.fill (1.0f);
    for (int ch = 0; ch < 2; ++ch) laPrev[ch] = laPrev2[ch] = laPrev3[ch] = 0.0f;
}

// Passe-bande RBJ (gain 0 dB au centre), Q = 2, deux cellules : -3 dB vers 14 et 23 Hz, < -20 dB a 36 Hz.
void DspChain::updateMidroll()
{
    const float f = midrollFreqHz.load (std::memory_order_relaxed);
    if (f == lastMidFreq) return;
    lastMidFreq = f;
    const double w0 = 2.0 * juce::MathConstants<double>::pi * f / sampleRate, q = 2.0;
    const double alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
    for (auto& ch : midBp)
        for (auto& c : ch)
        {
            c.b0 = alpha / a0; c.b1 = 0.0; c.b2 = -alpha / a0;
            c.a1 = -2.0 * std::cos (w0) / a0; c.a2 = (1.0 - alpha) / a0;
            c.reset();
        }
}

// ---------------------------------------------------------------------------
//  Coefficient updates — cheap polling, only recomputes when atomic changed.
// ---------------------------------------------------------------------------
juce::dsp::IIR::Coefficients<float>::Ptr
DspChain::makeEqCoefs (int idx, double sr, float freq, float gainDb, float q)
{
    const float linearGain = juce::Decibels::decibelsToGain (gainDb);
    if (idx == 0)
        return juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, freq, q, linearGain);
    if (idx == kNumEqBands - 1)
        return juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, freq, q, linearGain);
    return juce::dsp::IIR::Coefficients<float>::makePeakFilter   (sr, freq, q, linearGain);
}

void DspChain::updateHpf()
{
    const float f = hpfFreqHz.load (std::memory_order_relaxed);
    if (f == lastHpfFreq) return;
    lastHpfFreq = f;

    auto coefs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, f);
    for (auto& filt : hpf)
        *filt.coefficients = *coefs;
}

void DspChain::updateEqBand (int idx)
{
    const auto f = eq[idx].freq.load   (std::memory_order_relaxed);
    const auto g = eq[idx].gainDb.load (std::memory_order_relaxed);
    const auto q = eq[idx].q.load      (std::memory_order_relaxed);

    if (f == lastEqFreq[idx] && g == lastEqGain[idx] && q == lastEqQ[idx]) return;
    lastEqFreq[idx] = f; lastEqGain[idx] = g; lastEqQ[idx] = q;

    auto coefs = makeEqCoefs (idx, sampleRate, f, g, juce::jmax (0.1f, q));
    for (auto& filt : eqFilters[idx])
        *filt.coefficients = *coefs;
}

void DspChain::updateCrossover()
{
    for (int i = 0; i < kNumXovers; ++i)
    {
        const float f = xoverFreq[i].load (std::memory_order_relaxed);
        if (f == lastXover[i]) continue;
        lastXover[i] = f;

        // xover[i] is the boundary between band[i] (upper edge) and band[i+1] (lower edge).
        // band[i].lp cutoff = xover[i]
        // band[i+1].hp cutoff = xover[i]
        for (int ch = 0; ch < 2; ++ch)
        {
            bandFilters[i    ][ch].lp.setCutoffFrequency (f);
            bandFilters[i + 1][ch].hp.setCutoffFrequency (f);
        }
    }
}

void DspChain::updateAllCoefficients()
{
    updateHpf();
    for (int i = 0; i < kNumEqBands; ++i) updateEqBand (i);
    updateCrossover();
}

// ---------------------------------------------------------------------------
//  Main process()
// ---------------------------------------------------------------------------
void DspChain::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int channels   = juce::jmin (2, buffer.getNumChannels());

    if (numSamples == 0 || channels == 0) return;

    // ---------- INPUT LEVEL METERS ----------
    {
        float pkL = 0, pkR = 0;
        const auto* L = buffer.getReadPointer (0);
        const auto* R = channels > 1 ? buffer.getReadPointer (1) : L;
        for (int i = 0; i < numSamples; ++i)
        {
            const float aL = std::abs (L[i]);
            const float aR = std::abs (R[i]);
            if (aL > pkL) pkL = aL;
            if (aR > pkR) pkR = aR;
            rmsInLSmooth += rmsAlpha * (aL * aL - rmsInLSmooth);
            rmsInRSmooth += rmsAlpha * (aR * aR - rmsInRSmooth);
        }
        peakInL.store (pkL, std::memory_order_relaxed);
        peakInR.store (pkR, std::memory_order_relaxed);
        rmsInL.store (std::sqrt (rmsInLSmooth), std::memory_order_relaxed);
        rmsInR.store (std::sqrt (rmsInRSmooth), std::memory_order_relaxed);
    }

    // ---------- BYPASS PATH ----------
    if (bypass.load (std::memory_order_relaxed))
    {
        // Just compute output meters (== input meters in this case).
        peakOutL.store (peakInL.load(), std::memory_order_relaxed);
        peakOutR.store (peakInR.load(), std::memory_order_relaxed);
        rmsOutL.store (rmsInL.load(),   std::memory_order_relaxed);
        rmsOutR.store (rmsInR.load(),   std::memory_order_relaxed);
        return;
    }

    // ---------- coefficient refresh (cheap, only updates if atomic changed) ----------
    updateAllCoefficients();

    // ---------- 0. MIDROLL : la bande des 18 Hz est mise de cote, intacte ----------
    const bool midroll = midrollOn.load (std::memory_order_relaxed);
    if (midroll)
    {
        updateMidroll();
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            auto* m = midBuf.getWritePointer (ch);
            auto& c = midBp[(size_t) ch];
            for (int i = 0; i < numSamples; ++i)
            {
                const double x = d[i];
                const double band = c[1].process (c[0].process (x));
                m[i] = (float) band;
                d[i] = (float) (x - band);          // le traitement ne voit plus le Midroll
            }
        }
    }

    // ---------- 1. INPUT GAIN ----------
    const float inGainLin = juce::Decibels::decibelsToGain (inputGainDb.load (std::memory_order_relaxed));
    buffer.applyGain (inGainLin);

    // ---------- 2. HPF ----------
    if (hpfOn.load (std::memory_order_relaxed))
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = hpf[ch].processSample (d[i]);
        }
    }

    // ---------- 3. AGC (une bande, ou deux bandes : graves / reste) ----------
    if (agcOn.load (std::memory_order_relaxed))
    {
        const float aMs = agcAttackMs.load (std::memory_order_relaxed), rMs = agcReleaseMs.load (std::memory_order_relaxed);
        agc.updateCoefs (aMs, rMs);
        const float thr = agcThresholdDb.load (std::memory_order_relaxed);
        const float rat = agcRatio.load       (std::memory_order_relaxed);
        const float mu  = agcMakeupDb.load    (std::memory_order_relaxed);
        const float ceiling = std::numeric_limits<float>::infinity();

        auto* L = buffer.getWritePointer (0);
        auto* R = channels > 1 ? buffer.getWritePointer (1) : nullptr;
        if (! agcTwoBand.load (std::memory_order_relaxed))
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float detect = R ? juce::jmax (std::abs (L[i]), std::abs (R[i])) : std::abs (L[i]);
                const float g = agc.computeGain (detect, thr, rat, mu, ceiling);
                L[i] *= g;
                if (R) R[i] *= g;
            }
        }
        else
        {
            const float fs = agcSplitHz.load (std::memory_order_relaxed);
            if (fs != lastAgcSplit) { lastAgcSplit = fs; for (auto& f : agcSplit) f.setCutoffFrequency (juce::jlimit (60.0f, 600.0f, fs)); }
            agcLow.updateCoefs (aMs, rMs);
            for (int i = 0; i < numSamples; ++i)
            {
                float lo[2], hi[2];
                for (int ch = 0; ch < channels; ++ch)
                {
                    const float x = ch == 0 ? L[i] : R[i];
                    lo[ch] = agcSplit[0].processSample (ch, x);
                    hi[ch] = agcSplit[1].processSample (ch, x);
                }
                const float dLo = channels > 1 ? juce::jmax (std::abs (lo[0]), std::abs (lo[1])) : std::abs (lo[0]);
                const float dHi = channels > 1 ? juce::jmax (std::abs (hi[0]), std::abs (hi[1])) : std::abs (hi[0]);
                const float gLo = agcLow.computeGain (dLo, thr, rat, mu, ceiling);
                const float gHi = agc.computeGain (dHi, thr, rat, mu, ceiling);
                L[i] = lo[0] * gLo + hi[0] * gHi;
                if (R) R[i] = lo[1] * gLo + hi[1] * gHi;
            }
            agcLowGrDb.store (agcLow.currentGrDb(), std::memory_order_relaxed);
        }
        agcGrDb.store (agc.currentGrDb(), std::memory_order_relaxed);
    }

    // ---------- 4. EQ 4 bandes ----------
    for (int b = 0; b < kNumEqBands; ++b)
    {
        if (! eq[b].on.load (std::memory_order_relaxed)) continue;
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = eqFilters[b][ch].processSample (d[i]);
        }
    }

    // ---------- 4b. Elargisseur stereo (cote renforce au-dessus de 200 Hz, basses laissees mono) ----------
    if (channels > 1 && stereoOn.load (std::memory_order_relaxed))
    {
        const float w = juce::jlimit (-1.0f, 1.0f, stereoWidth.load (std::memory_order_relaxed) - 1.0f);
        auto* L = buffer.getWritePointer (0); auto* R = buffer.getWritePointer (1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float side = 0.5f * (L[i] - R[i]);
            const float add = w * sideHp[0].processSample (side);
            L[i] += add; R[i] -= add;
        }
    }

    // ---------- 5. Crossover 6 bandes + comp par bande ----------
    //
    //  Strategy : for each band, copy the signal into a scratch buffer and apply
    //  the appropriate HPF + LPF cascade to isolate that band, then run its
    //  compressor, then sum back into the main buffer.

    // First clear scratch and split.
    for (int b = 0; b < kNumBands; ++b)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dst = bandScratch[b].getWritePointer (ch);
            const auto* src = buffer.getReadPointer (ch);
            std::memcpy (dst, src, sizeof (float) * static_cast<size_t> (numSamples));

            // Highpass (skip for band 0)
            if (b > 0)
            {
                auto& f = bandFilters[b][ch].hp;
                for (int i = 0; i < numSamples; ++i)
                    dst[i] = f.processSample (ch, dst[i]);
            }
            // Lowpass (skip for last band)
            if (b < kNumBands - 1)
            {
                auto& f = bandFilters[b][ch].lp;
                for (int i = 0; i < numSamples; ++i)
                    dst[i] = f.processSample (ch, dst[i]);
            }
        }

        // Per-band compressor
        auto& cb = comp[b];
        bandComps[b].updateCoefs (cb.attackMs.load  (std::memory_order_relaxed),
                                  cb.releaseMs.load (std::memory_order_relaxed));
        const float thr = cb.thresholdDb.load (std::memory_order_relaxed);
        const float rat = cb.ratio.load       (std::memory_order_relaxed);
        const float mu  = cb.makeupDb.load    (std::memory_order_relaxed);
        const float ceiling = std::numeric_limits<float>::infinity();

        auto* L = bandScratch[b].getWritePointer (0);
        auto* R = channels > 1 ? bandScratch[b].getWritePointer (1) : nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float detect = R ? juce::jmax (std::abs (L[i]), std::abs (R[i])) : std::abs (L[i]);
            const float g = bandComps[b].computeGain (detect, thr, rat, mu, ceiling);
            L[i] *= g;
            if (R) R[i] *= g;
        }
        cb.grDb.store (bandComps[b].currentGrDb(), std::memory_order_relaxed);

        // Limiteur de la bande (attaque immediate, relachement court)
        if (bandLimOn.load (std::memory_order_relaxed))
        {
            const float c = juce::Decibels::decibelsToGain (bandLimCeilingDb.load (std::memory_order_relaxed));
            const float rel = std::exp (-1.0f / juce::jmax (1.0f, bandLimReleaseMs.load (std::memory_order_relaxed) * 0.001f * (float) sampleRate));
            float env = bandLimEnv[(size_t) b];       // gain courant (1 = rien)
            for (int i = 0; i < numSamples; ++i)
            {
                const float pk = R ? juce::jmax (std::abs (L[i]), std::abs (R[i])) : std::abs (L[i]);
                const float req = pk > c ? c / pk : 1.0f;
                env = env <= 0.0f ? req : (req < env ? req : req + (env - req) * rel);
                L[i] *= env; if (R) R[i] *= env;
            }
            bandLimEnv[(size_t) b] = env;
        }

        // Ecreteur de basses : bandes entierement sous bassClipHz, ecretage doux
        if (bassClipOn.load (std::memory_order_relaxed)
            && (b < kNumXovers ? xoverFreq[(size_t) b].load (std::memory_order_relaxed) <= bassClipHz.load (std::memory_order_relaxed) : false))
        {
            const float c = juce::Decibels::decibelsToGain (bassClipDb.load (std::memory_order_relaxed)), k = 0.5f * c;
            auto clip = [c, k] (float x) { const float a = std::abs (x); if (a <= k) return x; return std::copysign (k + (c - k) * std::tanh ((a - k) / (c - k)), x); };
            for (int i = 0; i < numSamples; ++i) { L[i] = clip (L[i]); if (R) R[i] = clip (R[i]); }
        }
    }

    // Sum bands back into the main buffer.
    buffer.clear();
    for (int b = 0; b < kNumBands; ++b)
        for (int ch = 0; ch < channels; ++ch)
            buffer.addFrom (ch, 0, bandScratch[b], ch, 0, numSamples);

    // ---------- 7. Output gain (before limiter) ----------
    const float outGainLin = juce::Decibels::decibelsToGain (outputGainDb.load (std::memory_order_relaxed));
    buffer.applyGain (outGainLin);

    // ---------- 7b. Ecreteur doux (x2) ----------
    if (clipOn.load (std::memory_order_relaxed) && clipOs != nullptr && channels == numChannels && numSamples <= maxBlockSize)
    {
        const float c = juce::Decibels::decibelsToGain (clipCeilingDb.load (std::memory_order_relaxed));
        const float k = c * juce::Decibels::decibelsToGain (-juce::jmax (0.1f, clipKneeDb.load (std::memory_order_relaxed)));
        juce::dsp::AudioBlock<float> blk (buffer.getArrayOfWritePointers(), (size_t) channels, (size_t) numSamples);
        auto up = clipOs->processSamplesUp (blk);
        float maxIn = 0.0f;
        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            auto* d = up.getChannelPointer (ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                const float x = d[i], a = std::abs (x);
                maxIn = juce::jmax (maxIn, a);
                if (a > k) d[i] = std::copysign (k + (c - k) * std::tanh ((a - k) / (c - k)), x);
            }
        }
        clipOs->processSamplesDown (blk);
        clipGrDb.store (maxIn > k ? juce::Decibels::gainToDecibels ((k + (c - k) * std::tanh ((maxIn - k) / (c - k))) / maxIn) : 0.0f,
                        std::memory_order_relaxed);
    }
    else clipGrDb.store (0.0f, std::memory_order_relaxed);

    // ---------- 8. Limiteur ----------
    const bool look = limLookahead.load (std::memory_order_relaxed);
    if (! look)
    {
        limiter.updateCoefs (1.0f /* fast attack ≈ 1 ms */,
                             limReleaseMs.load (std::memory_order_relaxed));
        const float ceiling = limCeilingDb.load (std::memory_order_relaxed);
        const float infRatio = std::numeric_limits<float>::infinity();

        auto* L = buffer.getWritePointer (0);
        auto* R = channels > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float detect = R ? juce::jmax (std::abs (L[i]), std::abs (R[i])) : std::abs (L[i]);
            const float g = limiter.computeGain (detect, ceiling, infRatio, 0.0f, ceiling);
            L[i] *= g;
            if (R) R[i] *= g;
        }
        limGrDb.store (limiter.currentGrDb(), std::memory_order_relaxed);
    }
    else
    {
        // Longueur d'anticipation (echantillons), recalee si le reglage change
        const int n = juce::jlimit (16, kMaxLook - 1, (int) std::round (limLookaheadMs.load (std::memory_order_relaxed) * 0.001 * sampleRate));
        if (n != laLen) resetLookahead (n);
        const float ceilLin = juce::Decibels::decibelsToGain (limCeilingDb.load (std::memory_order_relaxed));
        const float relCoef = (float) std::exp (-1.0 / (juce::jmax (1.0f, limReleaseMs.load (std::memory_order_relaxed)) * 0.001 * sampleRate));
        const bool tp = limTruePeak.load (std::memory_order_relaxed);
        auto* L = buffer.getWritePointer (0);
        auto* R = channels > 1 ? buffer.getWritePointer (1) : nullptr;
        float* M[2] = { midroll ? midBuf.getWritePointer (0) : nullptr, midroll && channels > 1 ? midBuf.getWritePointer (1) : nullptr };
        float minGain = 1.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            // crete de l'echantillon entrant (et, en option, crete reelle entre les echantillons precedents)
            float pk = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float x = ch == 0 ? L[i] : R[i];
                float p = std::abs (x);
                if (tp)
                {
                    // interpolation cubique (Catmull-Rom) a 1/4, 1/2, 3/4 entre prev2 et prev : crete inter-echantillons
                    const float y0 = laPrev3[ch], y1 = laPrev2[ch], y2 = laPrev[ch], y3 = x;
                    for (float t : { 0.25f, 0.5f, 0.75f })
                    {
                        const float v = 0.5f * ((2 * y1) + (-y0 + y2) * t + (2 * y0 - 5 * y1 + 4 * y2 - y3) * t * t + (-y0 + 3 * y1 - 3 * y2 + y3) * t * t * t);
                        p = juce::jmax (p, std::abs (v));
                    }
                }
                laPrev3[ch] = laPrev2[ch]; laPrev2[ch] = laPrev[ch]; laPrev[ch] = x;
                pk = juce::jmax (pk, p);
            }
            const float req = pk > ceilLin ? ceilLin / pk : 1.0f;

            // minimum glissant sur n echantillons (n petit : balayage direct, borne et sans allocation)
            laReq[(size_t) laPos] = req;
            float mn = req;
            for (int k = 1; k < laLen; ++k) mn = juce::jmin (mn, laReq[(size_t) ((laPos - k + kMaxLook) % kMaxLook)]);
            // relachement : on remonte doucement, on redescend tout de suite
            laRel = mn < laRel ? mn : mn + (laRel - mn) * relCoef;
            // moyenne glissante de meme longueur : gain lisse, atteint le minimum quand la crete sort du retard
            laSum += laRel - laMin[(size_t) ((laPos - laLen + kMaxLook) % kMaxLook)];
            laMin[(size_t) laPos] = laRel;
            const float g = (float) juce::jmin (1.0, laSum / laLen);

            // signal (et Midroll) retardes de n echantillons, puis gain applique
            const int out = (laPos - laLen + kMaxLook) % kMaxLook;
            for (int ch = 0; ch < channels; ++ch)
            {
                float* d = ch == 0 ? L : R;
                const float delayed = laSig[(size_t) ch][(size_t) out];
                laSig[(size_t) ch][(size_t) laPos] = d[i];
                d[i] = juce::jlimit (-ceilLin, ceilLin, delayed * g);      // ecreteur de securite (ne sert qu'en dernier recours)
                if (M[ch]) { const float md = laMid[(size_t) ch][(size_t) out]; laMid[(size_t) ch][(size_t) laPos] = M[ch][i]; M[ch][i] = md; }
            }
            minGain = juce::jmin (minGain, g);
            laPos = (laPos + 1) % kMaxLook;
        }
        limGrDb.store (juce::Decibels::gainToDecibels (minGain), std::memory_order_relaxed);
    }

    // ---------- 8b. Mesure de volume (ITU-R BS.1770 : ponderation K, blocs de 100 ms) ----------
    {
        const auto* L = buffer.getReadPointer (0);
        const auto* R = channels > 1 ? buffer.getReadPointer (1) : nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float l = kw[0][1].processSample (kw[0][0].processSample (L[i]));
            const float r = R ? kw[1][1].processSample (kw[1][0].processSample (R[i])) : l;
            loudBlockSum += (double) l * l + (double) r * r;
            if (++loudBlockN >= loudBlockLen)
            {
                loudBlocks[(size_t) loudBlockIdx] = loudBlockSum / loudBlockN; loudBlockIdx = (loudBlockIdx + 1) % 4;
                loudBlockSum = 0.0; loudBlockN = 0;
                const double ms = (loudBlocks[0] + loudBlocks[1] + loudBlocks[2] + loudBlocks[3]) / 4.0;
                const float lufs = (float) (-0.691 + 10.0 * std::log10 (ms + 1.0e-12));
                loudMomentary.store (juce::jmax (-70.0f, lufs), std::memory_order_relaxed);
                if (lufs > -70.0f)                    // silence exclu de la moyenne
                {
                    if (! loudMinuteInit) { loudMinuteEnergy = ms; loudMinuteInit = true; }
                    else loudMinuteEnergy += (ms - loudMinuteEnergy) / 600.0;     // ~1 min (600 blocs de 100 ms)
                    loudMinute.store ((float) (-0.691 + 10.0 * std::log10 (loudMinuteEnergy + 1.0e-12)), std::memory_order_relaxed);
                }
            }
        }
    }

    // ---------- 9. MIDROLL remis tel quel, puis securite anti-saturation ----------
    if (midroll)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            const auto* m = midBuf.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = juce::jlimit (-0.999f, 0.999f, d[i] + m[i]);
        }
    }

    // ---------- OUTPUT METERS ----------
    {
        float pkL = 0, pkR = 0;
        const auto* L = buffer.getReadPointer (0);
        const auto* R = channels > 1 ? buffer.getReadPointer (1) : L;
        for (int i = 0; i < numSamples; ++i)
        {
            const float aL = std::abs (L[i]);
            const float aR = std::abs (R[i]);
            if (aL > pkL) pkL = aL;
            if (aR > pkR) pkR = aR;
            rmsOutLSmooth += rmsAlpha * (aL * aL - rmsOutLSmooth);
            rmsOutRSmooth += rmsAlpha * (aR * aR - rmsOutRSmooth);
        }
        peakOutL.store (pkL, std::memory_order_relaxed);
        peakOutR.store (pkR, std::memory_order_relaxed);
        rmsOutL.store (std::sqrt (rmsOutLSmooth), std::memory_order_relaxed);
        rmsOutR.store (std::sqrt (rmsOutRSmooth), std::memory_order_relaxed);

        // Garde-fou 24/7 : un NaN dans un filtre ne doit jamais partir a l'antenne ni s'y installer.
        if (! std::isfinite (pkL + pkR))
        {
            buffer.clear();
            reset();
            rmsOutLSmooth = rmsOutRSmooth = 0.0f;
            peakOutL.store (0.0f, std::memory_order_relaxed); peakOutR.store (0.0f, std::memory_order_relaxed);
            rmsOutL.store (0.0f, std::memory_order_relaxed);  rmsOutR.store (0.0f, std::memory_order_relaxed);
            nanResets.fetch_add (1, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
//  paramsToJson / setParamByPath — generic param plane
// ---------------------------------------------------------------------------
juce::String DspChain::paramsToJson() const
{
    auto b = [] (bool x)   { return x ? "true" : "false"; };
    auto f = [] (float x)  { return juce::String (x, 4); };

    juce::String j;
    j << "{"
      << "\"bypass\":"        << b (bypass.load())
      << ",\"inputGainDb\":"  << f (inputGainDb.load())
      << ",\"outputGainDb\":" << f (outputGainDb.load())
      << ",\"agc2\":{\"on\":" << b (agcTwoBand.load()) << ",\"splitHz\":" << f (agcSplitHz.load()) << ",\"grLow\":" << f (agcLowGrDb.load()) << "}"
      << ",\"stereo\":{\"on\":" << b (stereoOn.load()) << ",\"width\":" << f (stereoWidth.load()) << "}"
      << ",\"bandLim\":{\"on\":" << b (bandLimOn.load()) << ",\"ceilingDb\":" << f (bandLimCeilingDb.load()) << ",\"releaseMs\":" << f (bandLimReleaseMs.load()) << "}"
      << ",\"bassClip\":{\"on\":" << b (bassClipOn.load()) << ",\"ceilingDb\":" << f (bassClipDb.load()) << ",\"freqHz\":" << f (bassClipHz.load()) << "}"
      << ",\"clip\":{\"on\":" << b (clipOn.load()) << ",\"ceilingDb\":" << f (clipCeilingDb.load()) << ",\"kneeDb\":" << f (clipKneeDb.load()) << ",\"gr\":" << f (clipGrDb.load()) << "}"
      << ",\"loud\":{\"momentary\":" << f (loudMomentary.load()) << ",\"minute\":" << f (loudMinute.load()) << "}"
      << ",\"midroll\":{\"on\":" << b (midrollOn.load()) << ",\"freqHz\":" << f (midrollFreqHz.load()) << "}"
      << ",\"hpf\":{\"on\":"  << b (hpfOn.load())     << ",\"freqHz\":" << f (hpfFreqHz.load()) << "}"
      << ",\"agc\":{\"on\":"  << b (agcOn.load())
            << ",\"thresholdDb\":" << f (agcThresholdDb.load())
            << ",\"ratio\":"       << f (agcRatio.load())
            << ",\"attackMs\":"    << f (agcAttackMs.load())
            << ",\"releaseMs\":"   << f (agcReleaseMs.load())
            << ",\"makeupDb\":"    << f (agcMakeupDb.load())
            << ",\"gr\":"          << f (agcGrDb.load())
        << "}";

    j << ",\"eq\":[";
    for (int i = 0; i < kNumEqBands; ++i)
    {
        if (i > 0) j << ",";
        j << "{\"on\":" << b (eq[i].on.load())
          << ",\"freq\":"   << f (eq[i].freq.load())
          << ",\"gainDb\":" << f (eq[i].gainDb.load())
          << ",\"q\":"      << f (eq[i].q.load())
          << "}";
    }
    j << "]";

    j << ",\"xover\":[";
    for (int i = 0; i < kNumXovers; ++i) { if (i > 0) j << ","; j << f (xoverFreq[i].load()); }
    j << "]";

    j << ",\"comp\":[";
    for (int i = 0; i < kNumBands; ++i)
    {
        if (i > 0) j << ",";
        j << "{\"thresholdDb\":" << f (comp[i].thresholdDb.load())
          << ",\"ratio\":"       << f (comp[i].ratio.load())
          << ",\"attackMs\":"    << f (comp[i].attackMs.load())
          << ",\"releaseMs\":"   << f (comp[i].releaseMs.load())
          << ",\"makeupDb\":"    << f (comp[i].makeupDb.load())
          << ",\"gr\":"          << f (comp[i].grDb.load())
          << "}";
    }
    j << "]";

    j << ",\"lim\":{\"ceilingDb\":" << f (limCeilingDb.load())
                << ",\"releaseMs\":"  << f (limReleaseMs.load())
                << ",\"lookahead\":"  << b (limLookahead.load())
                << ",\"lookaheadMs\":" << f (limLookaheadMs.load())
                << ",\"truePeak\":"   << b (limTruePeak.load())
                << ",\"gr\":"         << f (limGrDb.load())
        << "}";

    j << ",\"meters\":{"
        << "\"in\":{\"peakL\":"  << f (peakInL.load())  << ",\"peakR\":" << f (peakInR.load())
                << ",\"rmsL\":"  << f (rmsInL.load())   << ",\"rmsR\":"  << f (rmsInR.load())  << "}"
        << ",\"out\":{\"peakL\":"<< f (peakOutL.load()) << ",\"peakR\":" << f (peakOutR.load())
                << ",\"rmsL\":"  << f (rmsOutL.load())  << ",\"rmsR\":"  << f (rmsOutR.load()) << "}"
        << ",\"nanResets\":" << nanResets.load()
      << "}";

    j << "}";
    return j;
}

namespace
{
    // Small helper: dispatch atomic store of a float-or-bool based on the path tail.
    bool tryStoreBool (const juce::String& tail, std::atomic<bool>& target, const juce::String& wantTail, float value)
    {
        if (tail != wantTail) return false;
        target.store (value > 0.5f);
        return true;
    }
    bool tryStoreFloat (const juce::String& tail, std::atomic<float>& target, const juce::String& wantTail, float value)
    {
        if (tail != wantTail) return false;
        target.store (value);
        return true;
    }
}

bool DspChain::setParamByPath (const juce::String& path, float value)
{
    auto tokens = juce::StringArray::fromTokens (path, ".", "");
    if (tokens.size() < 1) return false;

    // Bornes : une valeur hors plage (page, script, fichier) ne doit jamais faire diverger un filtre a l'antenne.
    if (! std::isfinite (value)) return false;
    const float maxHz = sampleRate > 0.0 ? (float) (0.45 * sampleRate) : 20000.0f;
    auto hz    = [maxHz] (float v) { return juce::jlimit (10.0f, maxHz, v); };
    auto db    = [] (float v)      { return juce::jlimit (-24.0f, 24.0f, v); };
    auto ratio = [] (float v)      { return juce::jlimit (1.0f, 20.0f, v); };
    auto q     = [] (float v)      { return juce::jlimit (0.1f, 10.0f, v); };
    auto ceil  = [] (float v)      { return juce::jlimit (-24.0f, 0.0f, v); };
    auto ms    = [] (float v)      { return juce::jlimit (0.1f, 10000.0f, v); };
    auto thr   = [] (float v)      { return juce::jlimit (-60.0f, 0.0f, v); };

    const auto root = tokens[0];

    if (root == "bypass")        { bypass.store (value > 0.5f); return true; }
    if (root == "inputGainDb")   { inputGainDb.store (db (value));   return true; }
    if (root == "outputGainDb")  { outputGainDb.store (db (value));  return true; }

    if (root == "agc2" && tokens.size() == 2)
    {
        if (tokens[1] == "on")      { agcTwoBand.store (value > 0.5f); return true; }
        if (tokens[1] == "splitHz") { agcSplitHz.store (juce::jlimit (60.0f, 600.0f, value)); return true; }
    }
    if (root == "stereo" && tokens.size() == 2)
    {
        if (tokens[1] == "on")    { stereoOn.store (value > 0.5f); return true; }
        if (tokens[1] == "width") { stereoWidth.store (juce::jlimit (0.5f, 2.0f, value)); return true; }
    }
    if (root == "bandLim" && tokens.size() == 2)
    {
        if (tokens[1] == "on")        { bandLimOn.store (value > 0.5f); return true; }
        if (tokens[1] == "ceilingDb") { bandLimCeilingDb.store (juce::jlimit (-24.0f, 0.0f, value)); return true; }
        if (tokens[1] == "releaseMs") { bandLimReleaseMs.store (juce::jlimit (5.0f, 500.0f, value)); return true; }
    }
    if (root == "clip" && tokens.size() == 2)
    {
        if (tokens[1] == "on")        { clipOn.store (value > 0.5f); return true; }
        if (tokens[1] == "ceilingDb") { clipCeilingDb.store (juce::jlimit (-12.0f, 0.0f, value)); return true; }
        if (tokens[1] == "kneeDb")    { clipKneeDb.store (juce::jlimit (0.5f, 12.0f, value)); return true; }
    }
    if (root == "bassClip" && tokens.size() == 2)
    {
        if (tokens[1] == "on")        { bassClipOn.store (value > 0.5f); return true; }
        if (tokens[1] == "ceilingDb") { bassClipDb.store (juce::jlimit (-24.0f, 0.0f, value)); return true; }
        if (tokens[1] == "freqHz")    { bassClipHz.store (juce::jlimit (50.0f, 400.0f, value)); return true; }
    }

    if (root == "midroll" && tokens.size() == 2)
    {
        if (tokens[1] == "on")     { midrollOn.store (value > 0.5f); return true; }
        if (tokens[1] == "freqHz") { midrollFreqHz.store (juce::jlimit (5.0f, 60.0f, value)); return true; }
    }

    if (root == "hpf" && tokens.size() == 2)
    {
        if (tokens[1] == "on")     { hpfOn.store (value > 0.5f); return true; }
        if (tokens[1] == "freqHz") { hpfFreqHz.store (hz (value)); return true; }
    }

    if (root == "agc" && tokens.size() == 2)
    {
        if (tokens[1] == "on")          { agcOn.store (value > 0.5f);     return true; }
        if (tokens[1] == "thresholdDb") { agcThresholdDb.store (thr (value));   return true; }
        if (tokens[1] == "ratio")       { agcRatio.store (ratio (value));       return true; }
        if (tokens[1] == "attackMs")    { agcAttackMs.store (ms (value));       return true; }
        if (tokens[1] == "releaseMs")   { agcReleaseMs.store (ms (value));      return true; }
        if (tokens[1] == "makeupDb")    { agcMakeupDb.store (db (value));       return true; }
    }

    if (root == "eq" && tokens.size() == 3)
    {
        const int idx = tokens[1].getIntValue();
        if (idx < 0 || idx >= kNumEqBands) return false;
        if (tokens[2] == "on")     { eq[idx].on.store (value > 0.5f); return true; }
        if (tokens[2] == "freq")   { eq[idx].freq.store (hz (value));   return true; }
        if (tokens[2] == "gainDb") { eq[idx].gainDb.store (db (value)); return true; }
        if (tokens[2] == "q")      { eq[idx].q.store (q (value));       return true; }
    }

    if (root == "xover" && tokens.size() == 2)
    {
        const int idx = tokens[1].getIntValue();
        if (idx < 0 || idx >= kNumXovers) return false;
        xoverFreq[idx].store (hz (value));
        return true;
    }

    if (root == "comp" && tokens.size() == 3)
    {
        const int idx = tokens[1].getIntValue();
        if (idx < 0 || idx >= kNumBands) return false;
        if (tokens[2] == "thresholdDb") { comp[idx].thresholdDb.store (thr (value));   return true; }
        if (tokens[2] == "ratio")       { comp[idx].ratio.store       (ratio (value)); return true; }
        if (tokens[2] == "attackMs")    { comp[idx].attackMs.store    (ms (value));    return true; }
        if (tokens[2] == "releaseMs")   { comp[idx].releaseMs.store   (ms (value));    return true; }
        if (tokens[2] == "makeupDb")    { comp[idx].makeupDb.store    (db (value));    return true; }
    }

    if (root == "lim" && tokens.size() == 2)
    {
        if (tokens[1] == "ceilingDb") { limCeilingDb.store (ceil (value)); return true; }
        if (tokens[1] == "releaseMs") { limReleaseMs.store (ms (value));   return true; }
        if (tokens[1] == "lookahead")   { limLookahead.store (value > 0.5f); return true; }
        if (tokens[1] == "lookaheadMs") { limLookaheadMs.store (juce::jlimit (0.0f, 20.0f, value)); return true; }
        if (tokens[1] == "truePeak")    { limTruePeak.store (value > 0.5f); return true; }
    }

    return false;
}
