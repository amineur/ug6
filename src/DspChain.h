#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
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

    // --- Protection du Midroll (Amine, 11/10/2026) ---
    // Le Midroll des webradios est une sinusoide de 18 Hz a ~-1 dBFS que la regie detecte pour lancer la pub
    // streaming. Le coupe-bas l'ecrasait (-12 dB mesure sur Kompa). La bande autour de 18 Hz est donc separee AVANT
    // le traitement et remise telle quelle APRES le limiteur : la musique est traitee comme avant, le Midroll intact.
    std::atomic<bool>  midrollOn     { true };
    std::atomic<float> midrollFreqHz { 18.0f };

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

    // --- AGC 2 bandes (d'apres le Sound4 PULSE de la FM, 12/10/2026) : graves nivelees a part du reste ---
    std::atomic<bool>  agcTwoBand    { false };
    std::atomic<float> agcSplitHz    { 200.0f };
    std::atomic<float> agcLowGrDb    { 0.0f };     // meter readback (bande grave)

    // --- Elargisseur stereo (Stereo FX du Sound4) : canal « cote » renforce au-dessus de 200 Hz, basses mono ---
    std::atomic<bool>  stereoOn      { false };
    std::atomic<float> stereoWidth   { 1.0f };     // 1 = inchange, 1.3 = +30 %

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

    // --- Limiteur par bande (3B Limiter du Sound4) : chaque bande plafonnee a part, une grosse basse ne fait plus
    //     baisser les voix et les aigus ---
    std::atomic<bool>  bandLimOn        { false };
    std::atomic<float> bandLimCeilingDb { -4.0f };
    std::atomic<float> bandLimReleaseMs { 40.0f };

    // --- Ecreteur de basses (Final Limiter « BASS » du Sound4) : les bandes sous bassClipHz sont ecretees en douceur
    //     avant le limiteur final, qui n'a plus a baisser tout le programme a chaque coup de basse ---
    std::atomic<bool>  bassClipOn  { false };
    std::atomic<float> bassClipDb  { -4.0f };
    std::atomic<float> bassClipHz  { 150.0f };

    // --- Ecreteur doux final (Amine, 18/10/2026 : Kompa « plus faible que DAB ») : juste avant le limiteur, il rabote
    //     les cretes tres courtes (attaques) au lieu de faire baisser tout le son ; c'est l'etage qui donne au Sound4 de
    //     DAB son volume. Sureechantillonne x2 (pas de repliement dans l'aigu). Au-dessus du genou (plafond - genou),
    //     courbe tanh douce qui ne depasse jamais le plafond ; le limiteur ne gere plus que le reste.
    std::atomic<bool>  clipOn        { false };
    std::atomic<float> clipCeilingDb { -2.0f };
    std::atomic<float> clipKneeDb    { 3.0f };
    std::atomic<float> clipGrDb      { 0.0f };          // ecretage maximal du dernier bloc (dB, <= 0)

    // --- Mesure de volume (comme la barre « Loudness » du Sound4) : LUFS momentane (400 ms) et moyenne sur 1 min ---
    std::atomic<float> loudMomentary { -70.0f };
    std::atomic<float> loudMinute    { -70.0f };

    // --- Master limiter (brick-wall, soft-knee, ratio ∞ above ceiling) ---
    std::atomic<float> limCeilingDb { -1.0f };
    std::atomic<float> limReleaseMs { 60.0f };
    std::atomic<float> limGrDb      { 0.0f };
    // Limiteur a anticipation (Amine, 11/10/2026 : « plus que ca sature ») : le signal est retarde de quelques ms,
    // le gain descend en douceur AVANT chaque crete ; crete reelle (entre echantillons) estimee par interpolation,
    // puis ecreteur de securite au plafond.
    std::atomic<bool>  limLookahead   { true };
    std::atomic<float> limLookaheadMs { 5.0f };
    std::atomic<bool>  limTruePeak    { true };

    // ============================================================
    //  Meters — audio thread writes, UI reads
    // ============================================================
    std::atomic<float> peakInL  { 0.0f }, peakInR  { 0.0f };
    std::atomic<float> rmsInL   { 0.0f }, rmsInR   { 0.0f };
    std::atomic<float> peakOutL { 0.0f }, peakOutR { 0.0f };
    std::atomic<float> rmsOutL  { 0.0f }, rmsOutR  { 0.0f };
    std::atomic<int>   nanResets { 0 };     // sorties non finies (NaN/inf) purgees : filtres remis a zero

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

    // Passe-bande du Midroll, en double precision (18 Hz a 44,1 kHz : trop serre pour des biquads en float).
    // Deux cellules RBJ en cascade par canal (4e ordre) : tres etroit, la basse musicale (> 30 Hz) n'y passe pas.
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        inline double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void reset() { z1 = z2 = 0; }
    };
    std::array<std::array<Biquad, 2>, 2> midBp;           // [canal][cellule]
    juce::AudioBuffer<float> midBuf;
    float lastMidFreq = 0.0f;
    void updateMidroll();

    // AGC 2 bandes, elargisseur, limiteurs par bande, mesure de volume
    std::array<juce::dsp::LinkwitzRileyFilter<float>, 2> agcSplit;   // [0] passe-bas, [1] passe-haut
    Compressor agcLow;
    std::array<float, kNumBands> bandLimEnv {};
    float bandLimRel = 0.0f;
    std::array<juce::dsp::IIR::Filter<float>, 1> sideHp;
    std::array<std::array<juce::dsp::IIR::Filter<float>, 2>, 2> kw;   // ponderation K [canal][etage]
    double loudBlockSum = 0.0; int loudBlockN = 0, loudBlockLen = 4800;
    std::array<double, 4> loudBlocks {}; int loudBlockIdx = 0;
    double loudMinuteEnergy = 0.0; bool loudMinuteInit = false;
    float lastAgcSplit = 0.0f;

    // Limiteur a anticipation : tampons circulaires (signal + Midroll retardes ensemble), filtre de minimum glissant
    // et moyenne glissante sur la meme longueur (gain toujours <= au gain requis quand la crete sort du retard).
    std::unique_ptr<juce::dsp::Oversampling<float>> clipOs;   // cree dans prepare (jamais dans le fil audio)
    static constexpr int kMaxLook = 2048;                 // ~42 ms a 48 kHz
    std::array<std::array<float, kMaxLook>, 2> laSig {}, laMid {};
    std::array<float, kMaxLook> laReq {};                 // gain requis par echantillon
    std::array<float, kMaxLook> laMin {};                 // apres minimum glissant + relachement
    int laPos = 0, laLen = 0;
    double laSum = 0.0;                                   // somme glissante de laMin (moyenne)
    float laRel = 1.0f, laPrev[2] = {0, 0}, laPrev2[2] = {0, 0}, laPrev3[2] = {0, 0};
    void resetLookahead (int n);

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
