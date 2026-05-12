#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <thread>
#include <vector>

// LAME and libshout are optional. If they are not found at CMake configure time,
// UG6_HAS_LAME / UG6_HAS_SHOUT are 0 and BroadcastSender becomes a no-op stub
// (status reports "indisponible" in the UI).
#ifndef UG6_HAS_LAME
 #define UG6_HAS_LAME 0
#endif
#ifndef UG6_HAS_SHOUT
 #define UG6_HAS_SHOUT 0
#endif

/**
 *  MP3 encoder (LAME) + Icecast 2 streamer (libshout).
 *
 *  The audio thread calls pushAudio() with post-DSP samples. A dedicated sender
 *  thread drains the lock-free ring, encodes to MP3, and pushes to Icecast.
 *  shout_sync() handles real-time pacing. Connection drops trigger an
 *  exponential-backoff reconnect (capped at 30 s).
 */
class BroadcastSender
{
public:
    enum class Status { Disabled, Disconnected, Connecting, Connected, Error };

    struct Config
    {
        juce::String host        { "localhost" };
        int          port        { 8000 };
        juce::String mount       { "/stream" };
        juce::String user        { "source" };
        juce::String password    { "hackme" };
        juce::String streamName  { "UG6 Broadcaster" };
        juce::String streamGenre { "Various" };
        int          bitrateKbps { 128 };
    };

    BroadcastSender();
    ~BroadcastSender();

    /** True if the build was compiled with both LAME and libshout. */
    static bool isAvailable();

    void prepare (double sampleRate);

    /** Audio thread. Non-blocking. Drops samples if the ring is saturated.
        Signature matches juce::AudioIODeviceCallback for easy forwarding from
        the audio callback. We never mutate the buffers. */
    void pushAudio (float* const* channels, int numChannels, int numSamples);

    void   setConfig (const Config& c);
    Config getConfig() const;

    void connect();
    void disconnect();

    Status        getStatus()      const { return status.load (std::memory_order_relaxed); }
    juce::String  getStatusText()  const;
    juce::String  getLastError()   const;
    juce::int64   getBytesSent()   const { return bytesSent.load (std::memory_order_relaxed); }
    int           getReconnects()  const { return reconnects.load (std::memory_order_relaxed); }

    /** Snapshot of state suitable for inclusion in /api/state. */
    juce::String toJson() const;

private:
    void senderThreadLoop();
    bool openConnection();
    void closeConnection();
    void setError (const juce::String& msg);

    Config config;
    mutable juce::CriticalSection configLock;

    std::atomic<Status> status { Status::Disabled };
    juce::String lastError;
    mutable juce::CriticalSection errorLock;

    std::atomic<int>         reconnects { 0 };
    std::atomic<juce::int64> bytesSent  { 0 };
    std::atomic<int>         configEpoch         { 0 };
    int                       lastSeenConfigEpoch { 0 };

    std::atomic<bool> running            { false };
    std::atomic<bool> wantsToBeConnected { false };
    std::thread senderThread;

    juce::AbstractFifo       fifo       { 65536 };
    juce::AudioBuffer<float> ringBuffer { 2, 65536 };

    double sampleRate = 48000.0;

    // Opaque pointers — only touched from the sender thread.
    void* lameCtx  = nullptr;
    void* shoutCtx = nullptr;
};
