#include "BroadcastSender.h"

#if UG6_HAS_LAME
 extern "C" {
   #include <lame/lame.h>
 }
#endif

#if UG6_HAS_SHOUT
 extern "C" {
   #include <shout/shout.h>
 }
#endif

#include <chrono>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
namespace
{
   #if UG6_HAS_SHOUT
    void initShoutOnce()
    {
        static std::once_flag flag;
        std::call_once (flag, [] { shout_init(); });
    }
   #endif
}

bool BroadcastSender::isAvailable()
{
   #if UG6_HAS_LAME && UG6_HAS_SHOUT
    return true;
   #else
    return false;
   #endif
}

BroadcastSender::BroadcastSender()
{
   #if UG6_HAS_LAME && UG6_HAS_SHOUT
    initShoutOnce();
    status.store (Status::Disconnected);
    running.store (true);
    senderThread = std::thread ([this] { senderThreadLoop(); });
   #else
    status.store (Status::Disabled);
   #endif
}

BroadcastSender::~BroadcastSender()
{
   #if UG6_HAS_LAME && UG6_HAS_SHOUT
    running.store (false);
    wantsToBeConnected.store (false);
    if (senderThread.joinable())
        senderThread.join();
   #endif
}

void BroadcastSender::prepare (double sr) { sampleRate = sr; }

// ---------------------------------------------------------------------------
//  Audio-thread producer
// ---------------------------------------------------------------------------
void BroadcastSender::pushAudio (float* const* channels, int numCh, int numSamples)
{
   #if UG6_HAS_LAME && UG6_HAS_SHOUT
    if (numSamples <= 0 || numCh <= 0) return;
    if (status.load (std::memory_order_relaxed) != Status::Connected) return;  // don't buffer when offline

    int s1, sz1, s2, sz2;
    fifo.prepareToWrite (numSamples, s1, sz1, s2, sz2);

    for (int ch = 0; ch < 2; ++ch)
    {
        const int srcCh = juce::jmin (ch, numCh - 1);
        const float* src = channels[srcCh];
        if (src == nullptr) continue;

        if (sz1 > 0)
            ringBuffer.copyFrom (ch, s1, src, sz1);
        if (sz2 > 0)
            ringBuffer.copyFrom (ch, s2, src + sz1, sz2);
    }
    fifo.finishedWrite (sz1 + sz2);
   #else
    juce::ignoreUnused (channels, numCh, numSamples);
   #endif
}

// ---------------------------------------------------------------------------
//  Control plane
// ---------------------------------------------------------------------------
void BroadcastSender::setConfig (const Config& c)
{
    {
        const juce::ScopedLock sl (configLock);
        config = c;
    }
    // Bump the epoch so the sender thread can detect the change and
    // recycle the LAME/Shout objects on the fly.
    configEpoch.fetch_add (1);
}

BroadcastSender::Config BroadcastSender::getConfig() const
{
    const juce::ScopedLock sl (configLock);
    return config;
}

void BroadcastSender::connect()    { wantsToBeConnected.store (true); }
void BroadcastSender::disconnect() { wantsToBeConnected.store (false); }

juce::String BroadcastSender::getStatusText() const
{
    switch (status.load (std::memory_order_relaxed))
    {
        case Status::Disabled:     return "indisponible";
        case Status::Disconnected: return "déconnecté";
        case Status::Connecting:   return "connexion…";
        case Status::Connected:    return "connecté";
        case Status::Error:        return "erreur";
    }
    return "?";
}

juce::String BroadcastSender::getLastError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastError;
}

void BroadcastSender::setError (const juce::String& msg)
{
    const juce::ScopedLock sl (errorLock);
    lastError = msg;
}

// ---------------------------------------------------------------------------
//  Sender thread + connection management
// ---------------------------------------------------------------------------
#if UG6_HAS_LAME && UG6_HAS_SHOUT

bool BroadcastSender::openConnection()
{
    setError ({});
    Config cfg = getConfig();

    auto* lame = lame_init();
    if (lame == nullptr) { setError ("lame_init() a échoué"); return false; }

    lame_set_in_samplerate (lame, static_cast<int> (sampleRate));
    lame_set_num_channels  (lame, 2);
    lame_set_brate         (lame, cfg.bitrateKbps);
    lame_set_quality       (lame, 2);
    lame_set_mode          (lame, JOINT_STEREO);

    if (lame_init_params (lame) < 0)
    {
        setError ("lame_init_params() a échoué");
        lame_close (lame);
        return false;
    }
    lameCtx = lame;

    auto* shout = shout_new();
    if (shout == nullptr)
    {
        setError ("shout_new() a échoué");
        lame_close (lame); lameCtx = nullptr;
        return false;
    }

    shout_set_host     (shout, cfg.host.toRawUTF8());
    shout_set_port     (shout, static_cast<unsigned short> (cfg.port));
    shout_set_user     (shout, cfg.user.toRawUTF8());
    shout_set_password (shout, cfg.password.toRawUTF8());
    shout_set_mount    (shout, cfg.mount.toRawUTF8());
    shout_set_format   (shout, SHOUT_FORMAT_MP3);
    shout_set_protocol (shout, SHOUT_PROTOCOL_HTTP);    // Icecast 2

    // TLS auto-negotiation : libshout tries TLS first, falls back to plain HTTP.
    // Required for Infomaniak streams that mandate HTTPS; harmless on plain Icecast.
    // If libshout was compiled without OpenSSL this is a silent no-op.
   #ifdef SHOUT_TLS_AUTO
    shout_set_tls (shout, SHOUT_TLS_AUTO);
   #endif

    shout_set_name        (shout, cfg.streamName.toRawUTF8());
    shout_set_genre       (shout, cfg.streamGenre.toRawUTF8());
    shout_set_audio_info  (shout, SHOUT_AI_BITRATE,    juce::String (cfg.bitrateKbps).toRawUTF8());
    shout_set_audio_info  (shout, SHOUT_AI_SAMPLERATE, juce::String (static_cast<int> (sampleRate)).toRawUTF8());
    shout_set_audio_info  (shout, SHOUT_AI_CHANNELS,   "2");

    if (shout_open (shout) != SHOUTERR_SUCCESS)
    {
        setError (juce::String ("shout_open : ") + shout_get_error (shout));
        shout_free (shout);
        lame_close (lame);
        shoutCtx = nullptr;
        lameCtx  = nullptr;
        return false;
    }

    shoutCtx = shout;
    return true;
}

void BroadcastSender::closeConnection()
{
    if (shoutCtx != nullptr)
    {
        shout_close (static_cast<shout_t*> (shoutCtx));
        shout_free  (static_cast<shout_t*> (shoutCtx));
        shoutCtx = nullptr;
    }
    if (lameCtx != nullptr)
    {
        unsigned char tail[8192];
        lame_encode_flush (static_cast<lame_t> (lameCtx), tail, sizeof (tail));
        lame_close (static_cast<lame_t> (lameCtx));
        lameCtx = nullptr;
    }
    fifo.reset();
}

void BroadcastSender::senderThreadLoop()
{
    juce::Thread::setCurrentThreadName ("UG6BroadcastSender");

    constexpr int kChunkSamples = 1152; // 1 MP3 frame
    std::vector<unsigned char> mp3Buf (static_cast<size_t> (kChunkSamples) * 5 + 7200);
    std::vector<float> left  (kChunkSamples);
    std::vector<float> right (kChunkSamples);

    int backoffMs = 500;

    while (running.load())
    {
        const bool wantConnect = wantsToBeConnected.load();
        const bool currentlyConnected = (status.load() == Status::Connected);

        // Detect config change and force a reconnect cycle.
        const int epoch = configEpoch.load();
        if (epoch != lastSeenConfigEpoch && currentlyConnected)
        {
            closeConnection();
            status.store (Status::Disconnected);
        }
        lastSeenConfigEpoch = epoch;

        if (wantConnect && status.load() != Status::Connected)
        {
            status.store (Status::Connecting);
            if (openConnection())
            {
                status.store (Status::Connected);
                backoffMs = 500;
            }
            else
            {
                status.store (Status::Error);
                reconnects.fetch_add (1);
                std::this_thread::sleep_for (std::chrono::milliseconds (backoffMs));
                backoffMs = juce::jmin (backoffMs * 2, 30000);
                continue;
            }
        }

        if (! wantConnect && (shoutCtx != nullptr || lameCtx != nullptr))
        {
            closeConnection();
            status.store (Status::Disconnected);
        }

        // Drain → encode → send
        if (status.load() == Status::Connected && fifo.getNumReady() >= kChunkSamples)
        {
            int s1, sz1, s2, sz2;
            fifo.prepareToRead (kChunkSamples, s1, sz1, s2, sz2);

            int filled = 0;
            if (sz1 > 0)
            {
                std::memcpy (left.data()  + filled, ringBuffer.getReadPointer (0) + s1, sz1 * sizeof (float));
                std::memcpy (right.data() + filled, ringBuffer.getReadPointer (1) + s1, sz1 * sizeof (float));
                filled += sz1;
            }
            if (sz2 > 0)
            {
                std::memcpy (left.data()  + filled, ringBuffer.getReadPointer (0) + s2, sz2 * sizeof (float));
                std::memcpy (right.data() + filled, ringBuffer.getReadPointer (1) + s2, sz2 * sizeof (float));
                filled += sz2;
            }
            fifo.finishedRead (sz1 + sz2);

            const int mp3Bytes = lame_encode_buffer_ieee_float (
                static_cast<lame_t> (lameCtx),
                left.data(), right.data(), filled,
                mp3Buf.data(), static_cast<int> (mp3Buf.size()));

            if (mp3Bytes < 0)
            {
                setError ("LAME encode error " + juce::String (mp3Bytes));
                closeConnection();
                status.store (Status::Error);
                reconnects.fetch_add (1);
                continue;
            }

            if (mp3Bytes > 0)
            {
                auto* sh = static_cast<shout_t*> (shoutCtx);
                if (shout_send (sh, mp3Buf.data(), mp3Bytes) != SHOUTERR_SUCCESS)
                {
                    setError (juce::String ("shout_send : ") + shout_get_error (sh));
                    closeConnection();
                    status.store (Status::Error);
                    reconnects.fetch_add (1);
                    std::this_thread::sleep_for (std::chrono::milliseconds (backoffMs));
                    backoffMs = juce::jmin (backoffMs * 2, 30000);
                    continue;
                }
                bytesSent.fetch_add (mp3Bytes);
                shout_sync (sh);   // libshout paces real-time
            }
        }
        else
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }

    closeConnection();
}

#else  // ---------- stubs when LAME / libshout aren't available ----------

bool BroadcastSender::openConnection()  { return false; }
void BroadcastSender::closeConnection() {}
void BroadcastSender::senderThreadLoop() {}

#endif

// ---------------------------------------------------------------------------
//  JSON view
// ---------------------------------------------------------------------------
namespace
{
    juce::String esc (const juce::String& s) { return s.replace ("\"", "\\\""); }
}

juce::String BroadcastSender::toJson() const
{
    const Config c = getConfig();
    juce::String j;
    j << "{"
      << "\"available\":"   << (isAvailable() ? "true" : "false")
      << ",\"status\":\""    << getStatusText() << "\""
      << ",\"reconnects\":"  << reconnects.load()
      << ",\"bytesSent\":"   << bytesSent.load()
      << ",\"lastError\":\"" << esc (getLastError()) << "\""
      << ",\"config\":{"
            << "\"host\":\""     << esc (c.host)        << "\""
            << ",\"port\":"      << c.port
            << ",\"mount\":\""   << esc (c.mount)       << "\""
            << ",\"user\":\""    << esc (c.user)        << "\""
            << ",\"password\":\""<< esc (c.password)    << "\""
            << ",\"name\":\""    << esc (c.streamName)  << "\""
            << ",\"genre\":\""   << esc (c.streamGenre) << "\""
            << ",\"bitrate\":"   << c.bitrateKbps
        << "}"
      << "}";
    return j;
}
