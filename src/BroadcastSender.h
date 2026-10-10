#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <thread>

// External encoder libraries — both optional.
// LAME -> MP3 (always recommended for broad compatibility)
// fdk-aac -> AAC LC (preferred for low-bitrate streaming, e.g. 64 kbps)
#ifndef UG6_HAS_LAME
 #define UG6_HAS_LAME 0
#endif
#ifndef UG6_HAS_FDKAAC
 #define UG6_HAS_FDKAAC 0
#endif

/**
 *  MP3 encoder (LAME) + native Icecast 2 streamer (juce::StreamingSocket).
 *
 *  Pipeline :
 *    audio thread  →  ring buffer (lock-free)
 *    sender thread →  reads ring, encodes to MP3 with LAME, writes to socket
 *
 *  Icecast push protocol used : Icecast 2 SOURCE method (HTTP/1.0 + Basic auth).
 *  Compatible with vanilla Icecast 2.x and most providers (Infomaniak, Centova, …).
 *  Reconnect on socket error with exponential backoff capped at 30 s.
 */
class BroadcastSender
{
public:
    enum class Status { Disabled, Disconnected, Connecting, Connected, Error };
    enum class Codec  { MP3, AAC };

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
        Codec        codec       { Codec::MP3 };
    };

    /** Returns "mp3" or "aac" — used by the JSON layer. */
    static const char* codecToString (Codec c) noexcept { return c == Codec::MP3 ? "mp3" : "aac"; }
    static Codec       codecFromString (const juce::String& s) noexcept { return s == "aac" ? Codec::AAC : Codec::MP3; }

    BroadcastSender();
    ~BroadcastSender();

    /** True if the build was compiled with LAME (required for MP3 encoding). */
    static bool isAvailable();

    void prepare (double sampleRate);

    /** Audio thread. Non-blocking. Drops samples if the ring is saturated. */
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

    // Encoder opaque pointers (touched only on sender thread). Only one of
    // these is non-null at a time, depending on cfg.codec.
    void* lameCtx    = nullptr;   // LAME (MP3)
    void* aacCtx     = nullptr;   // fdk-aac (AAC LC)
    int   aacInputFrameSize = 0;  // samples-per-channel per aacEncEncode call

    // Outgoing TCP socket to the Icecast server.
    std::unique_ptr<juce::StreamingSocket> socket;
};
