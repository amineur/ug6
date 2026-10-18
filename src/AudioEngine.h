#pragma once

#include "DspChain.h"
#include "FilePlayer.h"
#include "BroadcastSender.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>

/**
 *  Phase A audio engine.
 *
 *  - Opens stereo input + stereo output (gracefully falls back to output-only
 *    if no input is available).
 *  - Two source modes: test generator (1 kHz sine) or live audio input.
 *  - All audio is routed through DspChain (HPF + AGC + EQ + multibande + limiter).
 *  - ON AIR == false → output is silent (DSP still runs, but final mute applied).
 */
class AudioEngine : public juce::AudioIODeviceCallback,
                    private juce::Timer
{
public:
    enum class Source { Generator, AudioInput, File };

    AudioEngine();
    ~AudioEngine() override;

    void initialise();
    void shutdown();

    DspChain& getDsp() noexcept { return dsp; }
    const DspChain& getDsp() const noexcept { return dsp; }

    FilePlayer& getFilePlayer() noexcept { return filePlayer; }
    const FilePlayer& getFilePlayer() const noexcept { return filePlayer; }

    // Plusieurs envois par radio, comme la liste « Encoder Settings » d'Oddcast (ex. MP3 256 + AAC 128).
    static constexpr int kNumEncoders = 4;
    BroadcastSender& getBroadcastSender (int i = 0) noexcept { return broadcasters[(size_t) juce::jlimit (0, kNumEncoders - 1, i)]; }
    const BroadcastSender& getBroadcastSender (int i = 0) const noexcept { return broadcasters[(size_t) juce::jlimit (0, kNumEncoders - 1, i)]; }

    // ---- master state ----
    void  setOnAir (bool v)              { onAir.store (v, std::memory_order_relaxed); }
    bool  isOnAir() const                { return onAir.load (std::memory_order_relaxed); }

    /** When false, the output audio device is still open (it drives the audio
        callback / DSP clock) but the final sample buffer is muted before being
        written. Lets 10 UG6 instances share a single output without producing a
        wall of noise — turn Monitor on for the one instance you want to preview. */
    // Source de secours (comme l'entree de secours du Sound4) : si l'entree reste muette plus de fallbackSeconds,
    // le fichier de secours (charge en boucle) prend le relais ; retour a l'entree des que le son revient (2 s).
    std::atomic<bool>  fallbackOn      { false };
    std::atomic<float> fallbackSeconds { 10.0f };
    std::atomic<bool>  fallbackActive  { false };
    std::atomic<bool>  fallbackFileMissing { false };   // fichier introuvable au demarrage : le Timer reessaie toutes les 30 s

    juce::String getFallbackFile() const;         // copie, sous verrou (fils HTTP + Timer)
    void         setFallbackFile (const juce::String& path);

    // Surveillance 24/7 (Timer sur le fil des messages) : rappel audio muet > 3 s = peripherique bloque.
    bool         isAudioStalled() const  { return audioStalled.load (std::memory_order_relaxed); }
    juce::String getLastDeviceError() const;

    void  setMonitorEnabled (bool v)     { monitorEnabled.store (v, std::memory_order_relaxed); }
    bool  isMonitorEnabled() const       { return monitorEnabled.load (std::memory_order_relaxed); }

    void  setSource (Source s)           { source.store (s, std::memory_order_relaxed); }
    Source getSource() const             { return source.load (std::memory_order_relaxed); }

    // ---- test generator ----
    void  setGenFrequency (float hz)     { genFreqHz.store (juce::jlimit (20.0f, 20000.0f, hz), std::memory_order_relaxed); }
    float getGenFrequency() const        { return genFreqHz.load (std::memory_order_relaxed); }

    void  setGenGainDb (float db)        { genGainDb.store (juce::jlimit (-60.0f, 6.0f, db), std::memory_order_relaxed); }
    float getGenGainDb() const           { return genGainDb.load (std::memory_order_relaxed); }

    // ---- device management ----
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    // Noms des peripheriques ouverts : copies mises en cache par le Timer (fil des messages) et apres chaque
    // changement, lues par les fils HTTP sans toucher a l'AudioDeviceManager (qui n'est pas protege contre eux).
    juce::String getCurrentInputDevice() const;
    juce::String getCurrentOutputDevice() const;

    // Entree / sortie VOULUES (demarrage ou API), vides = peripherique par defaut. C'est ce que persist() enregistre :
    // un repli temporaire sur l'entree par defaut ne doit jamais etre grave dans la config.
    juce::String getWantedInput() const;
    juce::String getWantedOutput() const;
    double       getCurrentSampleRate() const { return sampleRate; }

    juce::StringArray getInputDeviceNames();
    juce::StringArray getOutputDeviceNames();

    /** Switch input device by name. Empty = default. Returns error string (empty on success). */
    juce::String setInputDevice  (const juce::String& name);
    juce::String setOutputDevice (const juce::String& name);

    /** Audio backend (CoreAudio on Mac; WASAPI / DirectSound / MME on Windows).
        Switching the driver re-enumerates the input/output device list — useful
        when WASAPI hides Virtual Audio Cable devices that MME would expose. */
    juce::String      getCurrentDriverType() const;
    juce::StringArray getAvailableDriverTypes();
    juce::String      setDriverType (const juce::String& typeName);

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& ctx) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

private:
    void timerCallback() override;
    juce::String applyDeviceSetup (juce::AudioDeviceManager::AudioDeviceSetup setup, const juce::String& what);
    void setDeviceError (const juce::String& what, const juce::String& err);
    void refreshDeviceCache();                    // a appeler sous deviceLock (ou depuis le fil des messages)
    juce::String readDriverTypeName() const;      // nom du pilote lu directement (sous deviceLock)

    juce::AudioDeviceManager deviceManager;
    mutable juce::CriticalSection deviceLock;     // setInputDevice / setOutputDevice / setDriverType / reprise / listes
    juce::String wantedInput;                     // entree demandee (demarrage ou API), retentee toutes les 10 s
    juce::String wantedOutput;                    // sortie demandee (demarrage ou API)
    juce::String cachedInput, cachedOutput, cachedDriver;   // copies lues par les fils HTTP
    mutable juce::CriticalSection deviceCacheLock;
    juce::String lastDeviceError;
    mutable juce::CriticalSection deviceErrorLock;

    juce::String fallbackFile;                    // chemin du fichier de secours
    mutable juce::CriticalSection fallbackFileLock;
    bool fallbackPlaying = false;                 // ce que le Timer a demande au lecteur (play/pause hors du rappel audio)

    std::atomic<juce::uint32> lastCallbackMs { 0 };
    std::atomic<bool>         audioStalled   { false };
    // Compteur ms 32 bits : il reboucle tous les 49,7 jours. On ne compare donc jamais deux instants (now >= echeance),
    // seulement des ecarts non signes (juce::uint32) (now - debut) >= delai, justes a travers le rebouclage.
    int          restartBackoffMs = 1000;
    juce::uint32 lastRestartMs    = 0;            // instant de la derniere reouverture
    juce::uint32 restartWaitMs    = 0;            // attente avant la suivante (0 = tout de suite)
    juce::uint32 lastInputRetryMs = 0;
    juce::uint32 lastFallbackRetryMs = 0;

    DspChain dsp;
    FilePlayer filePlayer;
    int64_t silentSamples = 0, soundSamples = 0;
    std::array<BroadcastSender, kNumEncoders> broadcasters;

    std::atomic<bool>   onAir          { false };
    std::atomic<bool>   monitorEnabled { false };  // off by default for multi-instance setups
    std::atomic<Source> source         { Source::Generator };  // safe default, no feedback risk

    std::atomic<float>  genFreqHz { 1000.0f };
    std::atomic<float>  genGainDb { -18.0f };

    double sampleRate = 48000.0;
    double genPhase   = 0.0;

    // Pre-allocated silent buffer used to push silence to the Icecast encoder
    // when ON AIR is off (avoids audio-thread allocations).
    juce::AudioBuffer<float> silenceBuffer;
};
