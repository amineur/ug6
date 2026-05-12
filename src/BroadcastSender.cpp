#include "BroadcastSender.h"

#if UG6_HAS_LAME
 extern "C" {
   #include <lame/lame.h>
 }
#endif

#include <chrono>
#include <cstring>

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
bool BroadcastSender::isAvailable()
{
   #if UG6_HAS_LAME
    return true;
   #else
    return false;
   #endif
}

BroadcastSender::BroadcastSender()
{
   #if UG6_HAS_LAME
    status.store (Status::Disconnected);
    running.store (true);
    senderThread = std::thread ([this] { senderThreadLoop(); });
   #else
    status.store (Status::Disabled);
   #endif
}

BroadcastSender::~BroadcastSender()
{
   #if UG6_HAS_LAME
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
   #if UG6_HAS_LAME
    if (numSamples <= 0 || numCh <= 0) return;
    if (status.load (std::memory_order_relaxed) != Status::Connected) return;

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
//  Sender thread + connection management (native Icecast SOURCE protocol)
// ---------------------------------------------------------------------------
#if UG6_HAS_LAME

bool BroadcastSender::openConnection()
{
    setError ({});
    Config cfg = getConfig();

    // ----- 1. Initialise LAME -----
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

    // ----- 2. Connect TCP socket to the Icecast server -----
    auto sock = std::make_unique<juce::StreamingSocket>();
    if (! sock->connect (cfg.host, cfg.port, 5000 /* ms timeout */))
    {
        setError ("connexion TCP impossible vers " + cfg.host + ":" + juce::String (cfg.port));
        lame_close (lame); lameCtx = nullptr;
        return false;
    }

    // ----- 3. Send Icecast SOURCE headers -----
    // Icecast 2.x accepts the classic SOURCE method over HTTP/1.0.
    const auto credsB64 = juce::Base64::toBase64 (cfg.user + ":" + cfg.password);

    juce::String headers;
    headers << "SOURCE " << cfg.mount << " HTTP/1.0\r\n"
            << "Host: "          << cfg.host << ":" << cfg.port << "\r\n"
            << "Authorization: Basic " << credsB64 << "\r\n"
            << "User-Agent: UG6Broadcaster/0.3\r\n"
            << "Content-Type: audio/mpeg\r\n"
            << "ice-name: "      << cfg.streamName  << "\r\n"
            << "ice-genre: "     << cfg.streamGenre << "\r\n"
            << "ice-bitrate: "   << cfg.bitrateKbps << "\r\n"
            << "ice-public: 0\r\n"
            << "ice-audio-info: bitrate=" << cfg.bitrateKbps
                              << ";channels=2"
                              << ";samplerate=" << static_cast<int> (sampleRate) << "\r\n"
            << "\r\n";

    const auto headersUtf8 = headers.toRawUTF8();
    const auto headersLen  = static_cast<int> (headers.getNumBytesAsUTF8());

    if (sock->write (headersUtf8, headersLen) != headersLen)
    {
        setError ("échec écriture des headers SOURCE");
        sock.reset();
        lame_close (lame); lameCtx = nullptr;
        return false;
    }

    // ----- 4. Read the Icecast response (expect HTTP/1.0 200 OK) -----
    if (sock->waitUntilReady (true /*read*/, 5000) <= 0)
    {
        setError ("aucune réponse du serveur Icecast (timeout 5 s)");
        sock.reset();
        lame_close (lame); lameCtx = nullptr;
        return false;
    }

    char respBuf[1024] = {};
    const int n = sock->read (respBuf, sizeof (respBuf) - 1, true);
    if (n <= 0)
    {
        setError ("connexion Icecast fermée pendant la lecture de la réponse");
        sock.reset();
        lame_close (lame); lameCtx = nullptr;
        return false;
    }

    const juce::String response (respBuf, static_cast<size_t> (n));
    if (! response.startsWith ("HTTP/1.0 200") && ! response.startsWith ("HTTP/1.1 200"))
    {
        const auto firstLine = response.upToFirstOccurrenceOf ("\r\n", false, false);
        setError ("serveur Icecast : " + firstLine);
        sock.reset();
        lame_close (lame); lameCtx = nullptr;
        return false;
    }

    socket = std::move (sock);
    return true;
}

void BroadcastSender::closeConnection()
{
    if (socket != nullptr)
    {
        socket->close();
        socket.reset();
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

    constexpr int kChunkSamples = 1152; // 1 MP3 frame at standard rates
    std::vector<unsigned char> mp3Buf (static_cast<size_t> (kChunkSamples) * 5 + 7200);
    std::vector<float> left  (kChunkSamples);
    std::vector<float> right (kChunkSamples);

    int backoffMs = 500;

    while (running.load())
    {
        const bool wantConnect = wantsToBeConnected.load();

        // Detect config change → force a clean reconnect cycle.
        const int epoch = configEpoch.load();
        if (epoch != lastSeenConfigEpoch && status.load() == Status::Connected)
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

        if (! wantConnect && (socket != nullptr || lameCtx != nullptr))
        {
            closeConnection();
            status.store (Status::Disconnected);
        }

        // Drain → encode → write
        if (status.load() == Status::Connected && fifo.getNumReady() >= kChunkSamples)
        {
            int s1, sz1, s2, sz2;
            fifo.prepareToRead (kChunkSamples, s1, sz1, s2, sz2);

            int filled = 0;
            if (sz1 > 0)
            {
                std::memcpy (left.data()  + filled, ringBuffer.getReadPointer (0) + s1, static_cast<size_t> (sz1) * sizeof (float));
                std::memcpy (right.data() + filled, ringBuffer.getReadPointer (1) + s1, static_cast<size_t> (sz1) * sizeof (float));
                filled += sz1;
            }
            if (sz2 > 0)
            {
                std::memcpy (left.data()  + filled, ringBuffer.getReadPointer (0) + s2, static_cast<size_t> (sz2) * sizeof (float));
                std::memcpy (right.data() + filled, ringBuffer.getReadPointer (1) + s2, static_cast<size_t> (sz2) * sizeof (float));
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
                if (socket->write (mp3Buf.data(), mp3Bytes) != mp3Bytes)
                {
                    setError ("connexion Icecast perdue (write a échoué)");
                    closeConnection();
                    status.store (Status::Error);
                    reconnects.fetch_add (1);
                    std::this_thread::sleep_for (std::chrono::milliseconds (backoffMs));
                    backoffMs = juce::jmin (backoffMs * 2, 30000);
                    continue;
                }
                bytesSent.fetch_add (mp3Bytes);
            }
            // Real-time pacing is handled implicitly : we only get audio at real-time rate
            // from the audio callback, so the FIFO refills at the same speed.
        }
        else
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }

    closeConnection();
}

#else  // ---------- stub when LAME isn't available ----------

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
