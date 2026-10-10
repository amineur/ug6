#pragma once

#include "DspChain.h"
#include "FilePlayer.h"
#include "BroadcastSender.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>

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
class AudioEngine : public juce::AudioIODeviceCallback
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

    juce::String getCurrentInputDevice() const;
    juce::String getCurrentOutputDevice() const;
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

private:
    juce::AudioDeviceManager deviceManager;
    DspChain dsp;
    FilePlayer filePlayer;
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
