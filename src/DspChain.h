#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cmath>

/**
 *  UG6 DSP chain — Phase A.
 *
 *  Pipeline (in order):
 *    Input gain → HPF 30 Hz → AGC (slow leveler) → EQ 4 bandes →
 *    Crossover 6 bandes Linkwitz-Riley (parallel HPF+LPF) →
 *    Compresseur indépendant par bande → Somme →
 *    Brick-wall limiter → Output gain
 *
 *  Skipped for Phase A (coming in Phase B/C): de-esser sidechain, stereo enhancer M/S,
 *  soft clipper, lookahead in the limiter.
 *
 *  All user-facing parameters are std::atomic so the HTTP/WS thread can write
 *  to them without locks. Meters (peak/RMS/GR) are also atomic for the
 *  reverse direction (audio → UI).
 *
 *  process() runs in the audio callback. Never allocates, never locks.
 */
class DspChain
{
public:
    static constexpr int kNumBands = 6;
    static constexpr int kNumEqBands = 4;
    static constexpr int kNumXovers = kNumBands - 1; // 5 crossover points

    // ============================================================
    //  Public atomic parameters — written from UI thread, read from audio thread
    // ============================================================

    // --- master ---
    std::atomic<bool>  bypass        { false };
    std::atomic<float> inputGainDb   { 0.0f };
    std::atomic<float> outputGainDb  { 0.0f };

    // --- HPF (rumble filter) ---
    std::atomic<bool>  hpfOn         { true };
    std::atomic<float> hpfFreqHz     { 30.0f };

    // --- AGC (slow auto-leveler — implemented as gentle compressor + makeup) ---
    std::atomic<bool>  agcOn         { true };
    std::atomic<float> agcThresholdDb{ -24.0f };   // engages above this
    std::atomic<float> agcRatio      { 2.5f };
    std::atomic<float> agcAttackMs   { 500.0f };
    std::atomic<float> agcReleaseMs  { 2000.0f };
    std::atomic<float> agcMakeupDb   { 6.0f };     // brings level back up to ~-18 dB target
    std::atomic<float> agcGrDb       { 0.0f };     // meter readback

    // --- EQ 4 bandes (low-shelf, peak, peak, high-shelf) ---
    struct EqBand
    {
        std::atomic<bool>  on    { true };
        std::atomic<float> freq  { 1000.0f };
        std::atomic<float> gainDb{ 0.0f };
        std::atomic<float> q     { 1.0f };
    };
    std::array<EqBand, kNumEqBands> eq;

    // --- Crossover frequencies (5 points → 6 bands) ---
    std::array<std::atomic<float>, kNumXovers> xoverFreq;

    // --- Per-band compressors ---
    struct CompBand
    {
        std::atomic<float> thresholdDb { -18.0f };
        std::atomic<float> ratio       { 3.0f };
        std::atomic<float> attackMs    { 20.0f };
        std::atomic<float> releaseMs   { 150.0f };
        std::atomic<float> makeupDb    { 0.0f };
        std::atomic<float> grDb        { 0.0f };   // meter readback
    };
    std::array<CompBand, kNumBands> comp;

    // --- Master limiter (brick-wall, soft-knee, ratio ∞ above ceiling) ---
    std::atomic<float> limCeilingDb { -0.5f };
    std::atomic<float> limReleaseMs { 60.0f };
    std::atomic<float> limGrDb      { 0.0f };

    // ============================================================
    //  Meters — audio thread writes, UI reads
    // ============================================================
    std::atomic<float> peakInL  { 0.0f }, peakInR  { 0.0f };
    std::atomic<float> rmsInL   { 0.0f }, rmsInR   { 0.0f };
    std::atomic<float> peakOutL { 0.0f }, peakOutR { 0.0f };
    std::atomic<float> rmsOutL  { 0.0f }, rmsOutR  { 0.0f };

    // ============================================================
    //  Lifecycle (call from prepareToPlay / audio thread)
    // ============================================================
    DspChain();
    void prepare (double sampleRate, int blockSize, int numChannels);
    void reset();

    /** Process buffer in-place. Buffer must be ≤ blockSize given to prepare(). */
    void process (juce::AudioBuffer<float>& buffer);

    /** Snapshot all atomic parameters to a JSON-like text (helper for the HTTP layer). */
    juce::String paramsToJson() const;

    /** Apply a parameter by dotted path. Returns true if the path was recognised. */
    bool setParamByPath (const juce::String& path, float value);

private:
    // -------- Internal simple compressor --------------------------------
    // Feed-forward, peak-detector, hard-knee, exponential envelope follower.
    // Exposed indirectly via the AGC, per-band comps and limiter.
    struct Compressor
    {
        double sr = 48000.0;
        float  envDb     = 0.0f;
        float  attackCoef  = 0.0f;
        float  releaseCoef = 0.0f;

        void prepare (double sampleRate) { sr = sampleRate; envDb = 0.0f; }

        // Update exp() coefficients from atomic params; cheap to call once per block.
        void updateCoefs (float attackMs, float releaseMs)
        {
            attackCoef  = std::exp (-1.0f / juce::jmax (1.0f, attackMs  * 0.001f * (float) sr));
            releaseCoef = std::exp (-1.0f / juce::jmax (1.0f, releaseMs * 0.001f * (float) sr));
        }

        /** Returns linear gain to apply to both channels.
            @param detectAbs   max(|L|, |R|) of the current sample */
        float computeGain (float detectAbs, float thresholdDb, float ratio,
                           float makeupDb, float ceilingDb /* limiter only, +inf otherwise */)
        {
            const float dbIn = juce::Decibels::gainToDecibels (detectAbs + 1.0e-9f);
            const float over = dbIn - thresholdDb;

            float targetGr = 0.0f;
            if (over > 0.0f)
            {
                if (std::isinf (ratio))     // limiter
                    targetGr = juce::jmax (0.0f, dbIn - ceilingDb);
                else
                    targetGr = over * (1.0f - 1.0f / ratio);
            }

            const float coef = (targetGr > envDb) ? attackCoef : releaseCoef;
            envDb = coef * envDb + (1.0f - coef) * targetGr;

            return juce::Decibels::decibelsToGain (-envDb + makeupDb);
        }

        float currentGrDb() const noexcept { return envDb; }
    };

    // -------- Internal state ---------------------------------------------
    double sampleRate     = 48000.0;
    int    maxBlockSize   = 512;
    int    numChannels    = 2;

    // HPF (one per channel)
    std::array<juce::dsp::IIR::Filter<float>, 2> hpf;

    // EQ (4 bands × 2 channels)
    std::array<std::array<juce::dsp::IIR::Filter<float>, 2>, kNumEqBands> eqFilters;

    // Crossover bandpass per band (HPF + LPF stage cascaded) × 2 channels.
    // Band 0 = LPF only, Band kNumBands-1 = HPF only, others = HPF then LPF.
    struct BandFilter
    {
        juce::dsp::LinkwitzRileyFilter<float> hp;  // unused for band 0
        juce::dsp::LinkwitzRileyFilter<float> lp;  // unused for last band
    };
    std::array<std::array<BandFilter, 2>, kNumBands> bandFilters;

    // Compressors
    Compressor agc;
    std::array<Compressor, kNumBands> bandComps;
    Compressor limiter;

    // Scratch buffers (one per band, allocated in prepare)
    std::array<juce::AudioBuffer<float>, kNumBands> bandScratch;

    // RMS smoothing
    float rmsAlpha = 0.0f;
    float rmsInLSmooth = 0.0f, rmsInRSmooth = 0.0f;
    float rmsOutLSmooth = 0.0f, rmsOutRSmooth = 0.0f;

    // Cached coefficient versions (for "did param change?" debounce)
    float lastHpfFreq = 0.0f;
    std::array<float, kNumEqBands>  lastEqFreq  {{}};
    std::array<float, kNumEqBands>  lastEqGain  {{}};
    std::array<float, kNumEqBands>  lastEqQ     {{}};
    std::array<float, kNumXovers>   lastXover   {{}};

    // -------- Helpers --------
    void updateHpf();
    void updateEqBand (int idx);
    void updateCrossover();
    void updateAllCoefficients();

    static juce::dsp::IIR::Coefficients<float>::Ptr makeEqCoefs (int bandIndex,
                                                                  double sr,
                                                                  float freq, float gainDb, float q);
};
