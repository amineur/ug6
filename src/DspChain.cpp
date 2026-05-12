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

    rmsInLSmooth = rmsInRSmooth = rmsOutLSmooth = rmsOutRSmooth = 0.0f;
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

    // ---------- 3. AGC ----------
    if (agcOn.load (std::memory_order_relaxed))
    {
        agc.updateCoefs (agcAttackMs.load (std::memory_order_relaxed),
                         agcReleaseMs.load (std::memory_order_relaxed));
        const float thr = agcThresholdDb.load (std::memory_order_relaxed);
        const float rat = agcRatio.load       (std::memory_order_relaxed);
        const float mu  = agcMakeupDb.load    (std::memory_order_relaxed);
        const float ceiling = std::numeric_limits<float>::infinity();

        auto* L = buffer.getWritePointer (0);
        auto* R = channels > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float detect = R ? juce::jmax (std::abs (L[i]), std::abs (R[i])) : std::abs (L[i]);
            const float g = agc.computeGain (detect, thr, rat, mu, ceiling);
            L[i] *= g;
            if (R) R[i] *= g;
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
    }

    // Sum bands back into the main buffer.
    buffer.clear();
    for (int b = 0; b < kNumBands; ++b)
        for (int ch = 0; ch < channels; ++ch)
            buffer.addFrom (ch, 0, bandScratch[b], ch, 0, numSamples);

    // ---------- 7. Output gain (before limiter) ----------
    const float outGainLin = juce::Decibels::decibelsToGain (outputGainDb.load (std::memory_order_relaxed));
    buffer.applyGain (outGainLin);

    // ---------- 8. Brick-wall limiter ----------
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
                << ",\"gr\":"         << f (limGrDb.load())
        << "}";

    j << ",\"meters\":{"
        << "\"in\":{\"peakL\":"  << f (peakInL.load())  << ",\"peakR\":" << f (peakInR.load())
                << ",\"rmsL\":"  << f (rmsInL.load())   << ",\"rmsR\":"  << f (rmsInR.load())  << "}"
        << ",\"out\":{\"peakL\":"<< f (peakOutL.load()) << ",\"peakR\":" << f (peakOutR.load())
                << ",\"rmsL\":"  << f (rmsOutL.load())  << ",\"rmsR\":"  << f (rmsOutR.load()) << "}"
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

    const auto root = tokens[0];

    if (root == "bypass")        { bypass.store (value > 0.5f); return true; }
    if (root == "inputGainDb")   { inputGainDb.store (value);   return true; }
    if (root == "outputGainDb")  { outputGainDb.store (value);  return true; }

    if (root == "hpf" && tokens.size() == 2)
    {
        if (tokens[1] == "on")     { hpfOn.store (value > 0.5f); return true; }
        if (tokens[1] == "freqHz") { hpfFreqHz.store (value);    return true; }
    }

    if (root == "agc" && tokens.size() == 2)
    {
        if (tokens[1] == "on")          { agcOn.store (value > 0.5f);     return true; }
        if (tokens[1] == "thresholdDb") { agcThresholdDb.store (value);   return true; }
        if (tokens[1] == "ratio")       { agcRatio.store (value);         return true; }
        if (tokens[1] == "attackMs")    { agcAttackMs.store (value);      return true; }
        if (tokens[1] == "releaseMs")   { agcReleaseMs.store (value);     return true; }
        if (tokens[1] == "makeupDb")    { agcMakeupDb.store (value);      return true; }
    }

    if (root == "eq" && tokens.size() == 3)
    {
        const int idx = tokens[1].getIntValue();
        if (idx < 0 || idx >= kNumEqBands) return false;
        if (tokens[2] == "on")     { eq[idx].on.store (value > 0.5f); return true; }
        if (tokens[2] == "freq")   { eq[idx].freq.store (value);      return true; }
        if (tokens[2] == "gainDb") { eq[idx].gainDb.store (value);    return true; }
        if (tokens[2] == "q")      { eq[idx].q.store (value);         return true; }
    }

    if (root == "xover" && tokens.size() == 2)
    {
        const int idx = tokens[1].getIntValue();
        if (idx < 0 || idx >= kNumXovers) return false;
        xoverFreq[idx].store (value);
        return true;
    }

    if (root == "comp" && tokens.size() == 3)
    {
        const int idx = tokens[1].getIntValue();
        if (idx < 0 || idx >= kNumBands) return false;
        if (tokens[2] == "thresholdDb") { comp[idx].thresholdDb.store (value); return true; }
        if (tokens[2] == "ratio")       { comp[idx].ratio.store       (value); return true; }
        if (tokens[2] == "attackMs")    { comp[idx].attackMs.store    (value); return true; }
        if (tokens[2] == "releaseMs")   { comp[idx].releaseMs.store   (value); return true; }
        if (tokens[2] == "makeupDb")    { comp[idx].makeupDb.store    (value); return true; }
    }

    if (root == "lim" && tokens.size() == 2)
    {
        if (tokens[1] == "ceilingDb") { limCeilingDb.store (value); return true; }
        if (tokens[1] == "releaseMs") { limReleaseMs.store (value); return true; }
    }

    return false;
}
