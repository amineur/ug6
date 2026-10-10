#include "BroadcastSender.h"

#if UG6_HAS_LAME
 extern "C" {
   #include <lame/lame.h>
 }
#endif

#if UG6_HAS_FDKAAC
 extern "C" {
   #include <fdk-aac/aacenc_lib.h>
 }
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
bool BroadcastSender::isAvailable()
{
   #if UG6_HAS_LAME || UG6_HAS_FDKAAC
    return true;
   #else
    return false;
   #endif
}

BroadcastSender::BroadcastSender()
{
   #if UG6_HAS_LAME || UG6_HAS_FDKAAC
    status.store (Status::Disconnected);
    running.store (true);
    senderThread = std::thread ([this] { senderThreadLoop(); });
   #else
    status.store (Status::Disabled);
   #endif
}

BroadcastSender::~BroadcastSender()
{
   #if UG6_HAS_LAME || UG6_HAS_FDKAAC
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
#if UG6_HAS_LAME || UG6_HAS_FDKAAC

bool BroadcastSender::openConnection()
{
    setError ({});
    Config cfg = getConfig();

    // ----- 1. Initialise the encoder requested by cfg.codec -----
    if (cfg.codec == Codec::MP3)
    {
       #if UG6_HAS_LAME
        auto* lame = lame_init();
        if (lame == nullptr) { setError ("lame_init() a échoué"); return false; }

        // Output at 44.1 kHz — Infomaniak's mounts (and most FM-style hosts) expect
        // this rate. LAME does the resampling internally if input rate differs.
        constexpr int kOutSampleRate = 44100;

        lame_set_in_samplerate (lame, static_cast<int> (sampleRate));
        lame_set_out_samplerate(lame, kOutSampleRate);
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
        std::cout << "[Broadcast] LAME ready : in=" << static_cast<int>(sampleRate)
                  << "Hz  out=" << kOutSampleRate << "Hz  bitrate=" << cfg.bitrateKbps << "kbps\n" << std::flush;
       #else
        setError ("encoder MP3 non disponible — recompile avec LAME (brew install lame)");
        return false;
       #endif
    }
    else  // Codec::AAC
    {
       #if UG6_HAS_FDKAAC
        HANDLE_AACENCODER aac = nullptr;
        if (aacEncOpen (&aac, 0 /* any encoder modules */, 2 /* max channels */) != AACENC_OK)
        {
            setError ("aacEncOpen() a échoué");
            return false;
        }

        const auto setp = [&] (AACENC_PARAM p, juce::uint32 v) {
            return aacEncoder_SetParam (aac, p, v) == AACENC_OK;
        };

        // AAC-LC profile — universally supported by web/mobile players.
        bool ok = setp (AACENC_AOT,         2 /* AOT_AAC_LC */);
        ok    &= setp (AACENC_SAMPLERATE,   static_cast<juce::uint32> (sampleRate));
        ok    &= setp (AACENC_CHANNELMODE,  2 /* MODE_2 stereo */);
        ok    &= setp (AACENC_CHANNELORDER, 1 /* WAVE order */);
        ok    &= setp (AACENC_BITRATE,      static_cast<juce::uint32> (cfg.bitrateKbps * 1000));
        ok    &= setp (AACENC_TRANSMUX,     2 /* TT_MP4_ADTS — what Icecast expects */);
        ok    &= setp (AACENC_AFTERBURNER,  1 /* better quality, slight CPU bump */);

        if (! ok)
        {
            setError ("aacEncoder_SetParam() a échoué");
            aacEncClose (&aac);
            return false;
        }

        if (aacEncEncode (aac, nullptr, nullptr, nullptr, nullptr) != AACENC_OK)
        {
            setError ("phase d'init de l'aacEncoder a échoué");
            aacEncClose (&aac);
            return false;
        }

        AACENC_InfoStruct info {};
        if (aacEncInfo (aac, &info) == AACENC_OK)
            aacInputFrameSize = static_cast<int> (info.frameLength);
        else
            aacInputFrameSize = 1024;

        aacCtx = aac;
       #else
        setError ("encoder AAC non disponible — recompile avec fdk-aac (brew install fdk-aac)");
        return false;
       #endif
    }

    // ----- 2. Connect TCP socket to the Icecast server -----
    auto sock = std::make_unique<juce::StreamingSocket>();
    if (! sock->connect (cfg.host, cfg.port, 5000 /* ms timeout */))
    {
        setError ("connexion TCP impossible vers " + cfg.host + ":" + juce::String (cfg.port));
       #if UG6_HAS_LAME
        if (lameCtx) { lame_close (static_cast<lame_t> (lameCtx)); lameCtx = nullptr; }
       #endif
       #if UG6_HAS_FDKAAC
        if (aacCtx)  { HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER> (aacCtx); aacEncClose (&h); aacCtx = nullptr; }
       #endif
        return false;
    }

    // ----- 3. Send Icecast SOURCE headers -----
    // Icecast 2.x accepts the classic SOURCE method over HTTP/1.0.
    // Defensive: trim any stray whitespace/newlines that may have crept in
    // from copy-paste in the UI form fields.
    const auto user     = cfg.user.trim();
    const auto password = cfg.password.trim();
    const auto credsB64 = juce::Base64::toBase64 (user + ":" + password);

    const char* mimeType = (cfg.codec == Codec::MP3) ? "audio/mpeg" : "audio/aac";

    // Declare the bitstream sample rate that will reach the listener (44.1 kHz
    // for MP3 since LAME resamples; for AAC we still pass through at the input
    // rate until we add resampling there too).
    const int outSampleRate = (cfg.codec == Codec::MP3) ? 44100 : static_cast<int> (sampleRate);

    juce::String headers;
    headers << "SOURCE " << cfg.mount << " HTTP/1.0\r\n"
            << "Authorization: Basic " << credsB64 << "\r\n"
            << "User-Agent: libshout/2.4.6\r\n"        // mimic libshout — some providers filter by UA
            << "Content-Type: " << mimeType << "\r\n"
            << "ice-public: 0\r\n"
            << "ice-name: "      << cfg.streamName.trim()  << "\r\n"
            << "ice-genre: "     << cfg.streamGenre.trim() << "\r\n"
            << "ice-bitrate: "   << cfg.bitrateKbps << "\r\n"
            << "ice-audio-info: bitrate=" << cfg.bitrateKbps
                              << ";channels=2"
                              << ";samplerate=" << outSampleRate << "\r\n"
            << "\r\n";

    const auto headersUtf8 = headers.toRawUTF8();
    const auto headersLen  = static_cast<int> (headers.getNumBytesAsUTF8());

    // ---- DEBUG : print exactly what we're sending to the console ----
    std::cout << "\n========== UG6 Broadcast HANDSHAKE OUT ==========\n"
              << "Endpoint : " << cfg.host << ":" << cfg.port << cfg.mount << "\n"
              << "User     : " << user << "\n"
              << "Password : " << password.length() << " chars\n"
              << "B64 auth : " << credsB64 << "\n"
              << "Bytes    : " << headersLen << "\n"
              << "----- raw request -----\n"
              << headers.toStdString()
              << "----------------------\n" << std::flush;

    if (sock->write (headersUtf8, headersLen) != headersLen)
    {
        setError ("échec écriture des headers SOURCE");
        sock.reset();
       #if UG6_HAS_LAME
        if (lameCtx) { lame_close (static_cast<lame_t> (lameCtx)); lameCtx = nullptr; }
       #endif
       #if UG6_HAS_FDKAAC
        if (aacCtx)  { HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER> (aacCtx); aacEncClose (&h); aacCtx = nullptr; }
       #endif
        return false;
    }

    // ----- 4. Read the Icecast response (expect HTTP/1.0 200 OK) -----
    if (sock->waitUntilReady (true /*read*/, 5000) <= 0)
    {
        setError ("aucune réponse du serveur Icecast (timeout 5 s)");
        std::cout << "[Broadcast] no response within 5 s\n" << std::flush;
        sock.reset();
       #if UG6_HAS_LAME
        if (lameCtx) { lame_close (static_cast<lame_t> (lameCtx)); lameCtx = nullptr; }
       #endif
       #if UG6_HAS_FDKAAC
        if (aacCtx)  { HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER> (aacCtx); aacEncClose (&h); aacCtx = nullptr; }
       #endif
        return false;
    }

    char respBuf[1024] = {};
    const int n = sock->read (respBuf, sizeof (respBuf) - 1, true);

    std::cout << "========== UG6 Broadcast RESPONSE IN ==========\n"
              << "Bytes : " << n << "\n"
              << "----- raw response -----\n"
              << (n > 0 ? std::string (respBuf, static_cast<size_t> (n)) : std::string ("(empty)"))
              << "\n========================\n" << std::flush;

    if (n <= 0)
    {
        setError ("connexion Icecast fermée pendant la lecture de la réponse");
        sock.reset();
       #if UG6_HAS_LAME
        if (lameCtx) { lame_close (static_cast<lame_t> (lameCtx)); lameCtx = nullptr; }
       #endif
       #if UG6_HAS_FDKAAC
        if (aacCtx)  { HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER> (aacCtx); aacEncClose (&h); aacCtx = nullptr; }
       #endif
        return false;
    }

    const juce::String response (respBuf, static_cast<size_t> (n));
    if (! response.startsWith ("HTTP/1.0 200") && ! response.startsWith ("HTTP/1.1 200"))
    {
        const auto firstLine = response.upToFirstOccurrenceOf ("\r\n", false, false);
        setError ("serveur Icecast : " + firstLine);
        sock.reset();
       #if UG6_HAS_LAME
        if (lameCtx) { lame_close (static_cast<lame_t> (lameCtx)); lameCtx = nullptr; }
       #endif
       #if UG6_HAS_FDKAAC
        if (aacCtx)  { HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER> (aacCtx); aacEncClose (&h); aacCtx = nullptr; }
       #endif
        return false;
    }

    socket = std::move (sock);
    std::cout << "[Broadcast] >>> CONNECTED — waiting for audio chunks to encode and send...\n" << std::flush;
    return true;
}

void BroadcastSender::closeConnection()
{
    if (socket != nullptr)
    {
        socket->close();
        socket.reset();
    }
   #if UG6_HAS_LAME
    if (lameCtx != nullptr)
    {
        unsigned char tail[8192];
        lame_encode_flush (static_cast<lame_t> (lameCtx), tail, sizeof (tail));
        lame_close (static_cast<lame_t> (lameCtx));
        lameCtx = nullptr;
    }
   #endif
   #if UG6_HAS_FDKAAC
    if (aacCtx != nullptr)
    {
        HANDLE_AACENCODER h = static_cast<HANDLE_AACENCODER> (aacCtx);
        aacEncClose (&h);
        aacCtx = nullptr;
    }
   #endif
    aacInputFrameSize = 0;
    fifo.reset();
}

void BroadcastSender::senderThreadLoop()
{
    juce::Thread::setCurrentThreadName ("UG6BroadcastSender");

    // 1024 samples works for both codecs : it's the AAC-LC frame size, and
    // LAME is fine with any input length.
    constexpr int kChunkSamples = 1024;
    std::vector<unsigned char> outBuf (static_cast<size_t> (kChunkSamples) * 5 + 7200);
    std::vector<float>   left  (kChunkSamples);
    std::vector<float>   right (kChunkSamples);
    std::vector<int16_t> pcmInterleaved (static_cast<size_t> (kChunkSamples) * 2);  // for AAC

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

        if (! wantConnect && (socket != nullptr || lameCtx != nullptr || aacCtx != nullptr))
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

            int encodedBytes = 0;
            bool encodeError = false;

           #if UG6_HAS_LAME
            if (lameCtx != nullptr)
            {
                const int n = lame_encode_buffer_ieee_float (
                    static_cast<lame_t> (lameCtx),
                    left.data(), right.data(), filled,
                    outBuf.data(), static_cast<int> (outBuf.size()));
                if (n < 0) { setError ("LAME encode error " + juce::String (n)); encodeError = true; }
                else       { encodedBytes = n; }
            }
           #endif
           #if UG6_HAS_FDKAAC
            if (aacCtx != nullptr && ! encodeError)
            {
                // Interleave + convert float [-1,1] → int16 (fdk-aac's expected format).
                for (int i = 0; i < filled; ++i)
                {
                    const float l = juce::jlimit (-1.0f, 1.0f, left [i]);
                    const float r = juce::jlimit (-1.0f, 1.0f, right[i]);
                    pcmInterleaved [2 * i    ] = static_cast<int16_t> (l * 32767.0f);
                    pcmInterleaved [2 * i + 1] = static_cast<int16_t> (r * 32767.0f);
                }

                void* in_ptr        = pcmInterleaved.data();
                int   in_size       = static_cast<int> (filled * 2 * sizeof (int16_t));
                int   in_id         = 1 /* IN_AUDIO_DATA */;
                int   in_elsize     = sizeof (int16_t);

                AACENC_BufDesc in_buf = {};
                in_buf.numBufs           = 1;
                in_buf.bufs              = &in_ptr;
                in_buf.bufferIdentifiers = &in_id;
                in_buf.bufSizes          = &in_size;
                in_buf.bufElSizes        = &in_elsize;

                void* out_ptr       = outBuf.data();
                int   out_size      = static_cast<int> (outBuf.size());
                int   out_id        = 257 /* OUT_BITSTREAM_DATA = 0x0101 */;
                int   out_elsize    = 1;

                AACENC_BufDesc out_buf_desc = {};
                out_buf_desc.numBufs           = 1;
                out_buf_desc.bufs              = &out_ptr;
                out_buf_desc.bufferIdentifiers = &out_id;
                out_buf_desc.bufSizes          = &out_size;
                out_buf_desc.bufElSizes        = &out_elsize;

                AACENC_InArgs  in_args  = {};
                in_args.numInSamples    = filled * 2;  // total interleaved samples
                AACENC_OutArgs out_args = {};

                const AACENC_ERROR err = aacEncEncode (
                    static_cast<HANDLE_AACENCODER> (aacCtx),
                    &in_buf, &out_buf_desc, &in_args, &out_args);

                if (err != AACENC_OK && err != AACENC_ENCODE_EOF)
                {
                    setError ("AAC encode error " + juce::String (static_cast<int> (err)));
                    encodeError = true;
                }
                else
                {
                    encodedBytes = out_args.numOutBytes;
                }
            }
           #endif

            if (encodeError)
            {
                closeConnection();
                status.store (Status::Error);
                reconnects.fetch_add (1);
                continue;
            }

            if (encodedBytes > 0)
            {
                const int written = socket->write (outBuf.data(), encodedBytes);
                static int writeCallCount = 0;
                ++writeCallCount;

                // Log the first 5 writes verbosely, then every 50th to avoid spam.
                if (writeCallCount <= 5 || (writeCallCount % 50) == 0)
                    std::cout << "[Broadcast] write #" << writeCallCount
                              << " asked=" << encodedBytes
                              << " wrote=" << written
                              << " total=" << (bytesSent.load() + (written > 0 ? written : 0)) << " B\n" << std::flush;

                if (written != encodedBytes)
                {
                    std::cout << "[Broadcast] !!! write FAILED at call #" << writeCallCount
                              << " : asked " << encodedBytes
                              << " bytes, wrote " << written
                              << " (total sent so far : " << bytesSent.load() << " B)\n" << std::flush;
                    writeCallCount = 0;  // reset for next connection
                    setError ("connexion Icecast perdue (write retourne " + juce::String (written) + ")");
                    closeConnection();
                    status.store (Status::Error);
                    reconnects.fetch_add (1);
                    std::this_thread::sleep_for (std::chrono::milliseconds (backoffMs));
                    backoffMs = juce::jmin (backoffMs * 2, 30000);
                    continue;
                }
                bytesSent.fetch_add (encodedBytes);
            }
            else if (status.load() == Status::Connected)
            {
                // Encoder produced 0 bytes (still buffering internally). Normal at start.
                static int zeroEncodeCount = 0;
                ++zeroEncodeCount;
                if (zeroEncodeCount <= 3)
                    std::cout << "[Broadcast] encoder produced 0 bytes (call #" << zeroEncodeCount
                              << ", normal during warmup)\n" << std::flush;
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

#else  // ---------- stub when neither LAME nor fdk-aac is available ----------

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
            << ",\"codec\":\""   << codecToString (c.codec) << "\""
        << "}"
      << "}";
    return j;
}
