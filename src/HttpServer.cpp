#include "HttpServer.h"
#include "AudioEngine.h"
#include "BroadcastSender.h"

#include <httplib.h>

// JUCE-generated binary data — see juce_add_binary_data() in CMakeLists.txt.
#include "WebAssets.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdio>

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
namespace
{
    std::string jsonEscape (const juce::String& s)
    {
        std::string out;
        out.reserve (static_cast<size_t> (s.length()) + 8);
        for (auto c : s)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<juce::juce_wchar> (c) < 0x20)
                    {
                        char buf[8]; std::snprintf (buf, sizeof (buf), "\\u%04x", static_cast<int> (c));
                        out += buf;
                    }
                    else
                    {
                        out += juce::String::charToString (c).toStdString();
                    }
            }
        }
        return out;
    }

    float parseFloatBody (const std::string& body, float fallback)
    {
        try { return std::stof (body); } catch (...) { return fallback; }
    }

    bool parseBoolBody (const std::string& body)
    {
        return body == "true" || body == "1" || body == "on";
    }

    // ---------------------------------------------------------------------
    //  Hardcoded presets. Keep them gentle and useful for the MVP.
    //  Custom JSON presets with save/load will land in a later commit.
    // ---------------------------------------------------------------------
    void applyPreset (DspChain& d, const juce::String& name)
    {
        // First reset to a safe baseline — flat EQ, gentle comps, HPF on, AGC on.
        d.bypass.store (false);
        d.inputGainDb.store (0.0f);
        d.outputGainDb.store (0.0f);

        d.hpfOn.store (true);  d.hpfFreqHz.store (30.0f);

        d.agcOn.store (true);
        d.agcThresholdDb.store (-24.0f);
        d.agcRatio.store (2.5f);
        d.agcAttackMs.store (500.0f);
        d.agcReleaseMs.store (2000.0f);
        d.agcMakeupDb.store (6.0f);

        for (int i = 0; i < DspChain::kNumEqBands; ++i)
        {
            d.eq[i].on.store (true);
            d.eq[i].gainDb.store (0.0f);
            d.eq[i].q.store (1.0f);
        }
        d.eq[0].freq.store (100.0f);  d.eq[0].q.store (0.7f);
        d.eq[1].freq.store (400.0f);
        d.eq[2].freq.store (2500.0f);
        d.eq[3].freq.store (8000.0f); d.eq[3].q.store (0.7f);

        const float defaultXover[5] = { 100, 300, 800, 2500, 8000 };
        for (int i = 0; i < DspChain::kNumXovers; ++i)
            d.xoverFreq[i].store (defaultXover[i]);

        for (int i = 0; i < DspChain::kNumBands; ++i)
        {
            d.comp[i].thresholdDb.store (-18.0f);
            d.comp[i].ratio.store       (2.0f);
            d.comp[i].attackMs.store    (20.0f);
            d.comp[i].releaseMs.store   (150.0f);
            d.comp[i].makeupDb.store    (0.0f);
        }

        d.limCeilingDb.store (-0.5f);
        d.limReleaseMs.store (60.0f);

        // ---- Style-specific tweaks ----
        if (name == "bypass")
        {
            d.bypass.store (true);
        }
        else if (name == "music_fm")
        {
            // Punchy but musical : modest low/high shelves, moderate per-band comp.
            d.eq[0].gainDb.store (+1.5f);   // sub warmth
            d.eq[3].gainDb.store (+2.0f);   // air

            d.comp[0].thresholdDb.store (-22.0f); d.comp[0].ratio.store (3.0f); d.comp[0].makeupDb.store (1.0f);
            d.comp[1].thresholdDb.store (-20.0f); d.comp[1].ratio.store (3.0f);
            d.comp[2].thresholdDb.store (-18.0f); d.comp[2].ratio.store (2.5f);
            d.comp[3].thresholdDb.store (-18.0f); d.comp[3].ratio.store (2.5f);
            d.comp[4].thresholdDb.store (-20.0f); d.comp[4].ratio.store (3.0f); d.comp[4].makeupDb.store (1.0f);
            d.comp[5].thresholdDb.store (-22.0f); d.comp[5].ratio.store (3.0f); d.comp[5].makeupDb.store (1.5f);
        }
        else if (name == "talk")
        {
            // Speech : aggressive on the 800-2500 range (intelligibility), de-ess-ish on band 5.
            d.hpfFreqHz.store (80.0f);
            d.eq[1].freq.store (250.0f); d.eq[1].gainDb.store (-2.0f);  // tame muddiness
            d.eq[2].freq.store (3000.0f); d.eq[2].gainDb.store (+3.0f); // presence
            d.comp[2].thresholdDb.store (-24.0f); d.comp[2].ratio.store (4.0f); d.comp[2].makeupDb.store (2.0f);
            d.comp[3].thresholdDb.store (-22.0f); d.comp[3].ratio.store (4.0f); d.comp[3].makeupDb.store (1.0f);
            d.comp[4].thresholdDb.store (-26.0f); d.comp[4].ratio.store (5.0f); d.comp[4].releaseMs.store (80.0f); // sibilance
        }
        else if (name == "urban_pop")
        {
            // Loud, hyped, modern. Smile EQ + heavy multibande.
            d.eq[0].gainDb.store (+3.0f);
            d.eq[1].gainDb.store (-1.5f);
            d.eq[2].gainDb.store (+1.0f);
            d.eq[3].gainDb.store (+3.0f);
            for (int i = 0; i < DspChain::kNumBands; ++i)
            {
                d.comp[i].thresholdDb.store (-24.0f);
                d.comp[i].ratio.store       (4.0f);
                d.comp[i].releaseMs.store   (100.0f);
                d.comp[i].makeupDb.store    (2.0f);
            }
            d.limCeilingDb.store (-0.3f);
        }
        else if (name == "leveler")
        {
            // Loudness catch-up: aggressive AGC + medium multibande.
            // Use this when the playlist mixes loud-mastered modern tracks with
            // older / quieter material — UG6 will pull everything to roughly the
            // same perceived volume.
            d.hpfFreqHz.store (40.0f);

            d.agcThresholdDb.store (-32.0f);
            d.agcRatio.store       (5.0f);
            d.agcAttackMs.store    (400.0f);
            d.agcReleaseMs.store   (1200.0f);
            d.agcMakeupDb.store    (14.0f);

            for (int i = 0; i < DspChain::kNumBands; ++i)
            {
                d.comp[i].thresholdDb.store (-22.0f);
                d.comp[i].ratio.store       (3.5f);
                d.comp[i].attackMs.store    (15.0f);
                d.comp[i].releaseMs.store   (180.0f);
                d.comp[i].makeupDb.store    (1.5f);
            }
            d.limCeilingDb.store (-0.5f);
        }
        // unknown name → already at safe defaults
    }
}

// ---------------------------------------------------------------------------
//  HttpServer
// ---------------------------------------------------------------------------
HttpServer::HttpServer (AudioEngine& e, int p)
    : engine (e), port (p), instanceConfig (p)
{
    // Restore Icecast credentials for this port from disk (if any). This makes
    // each radio remember its host/mount/auth/bitrate across restarts.
    engine.getBroadcastSender().setConfig (instanceConfig.loadBroadcastConfig());
}
HttpServer::~HttpServer() { stop(); }

void HttpServer::start()
{
    if (running.exchange (true)) return;

    server = std::make_unique<httplib::Server>();
    // Allow audio file uploads up to ~200 MB (default cpp-httplib limit is small).
    server->set_payload_max_length (200 * 1024 * 1024);
    setupRoutes();

    listenerThread = std::thread ([this]
    {
        if (! server->listen ("127.0.0.1", port))
        {
            DBG ("HttpServer: failed to listen on 127.0.0.1:" << port);
            running = false;
        }
    });
}

void HttpServer::stop()
{
    if (! running.exchange (false)) return;
    if (server) server->stop();
    if (listenerThread.joinable()) listenerThread.join();
    server.reset();
}

// ---------------------------------------------------------------------------
//  JSON builders
// ---------------------------------------------------------------------------
std::string HttpServer::buildStateJson() const
{
    const char* srcName = "generator";
    switch (engine.getSource())
    {
        case AudioEngine::Source::Generator:  srcName = "generator"; break;
        case AudioEngine::Source::AudioInput: srcName = "input";     break;
        case AudioEngine::Source::File:       srcName = "file";      break;
    }

    const auto& fp = engine.getFilePlayer();

    juce::String s;
    s << "{"
      << "\"onAir\":"  << (engine.isOnAir() ? "true" : "false")
      << ",\"source\":\"" << srcName << "\""
      << ",\"gen\":{\"freqHz\":" << engine.getGenFrequency()
            << ",\"gainDb\":"     << engine.getGenGainDb() << "}"
      << ",\"file\":{"
            << "\"loaded\":"   << (fp.hasFile() ? "true" : "false")
            << ",\"playing\":" << (fp.isPlaying() ? "true" : "false")
            << ",\"looping\":" << (fp.isLooping() ? "true" : "false")
            << ",\"name\":\""    << jsonEscape (fp.getCurrentFileName()) << "\""
            << ",\"position\":"  << fp.getCurrentPositionSeconds()
            << ",\"duration\":"  << fp.getDurationSeconds()
        << "}"
      << ",\"device\":{"
            << "\"in\":\""  << jsonEscape (engine.getCurrentInputDevice())  << "\""
            << ",\"out\":\""<< jsonEscape (engine.getCurrentOutputDevice()) << "\""
            << ",\"sampleRate\":" << engine.getCurrentSampleRate() << "}"
      << ",\"dsp\":"  << engine.getDsp().paramsToJson()
      << ",\"broadcast\":" << engine.getBroadcastSender().toJson()
      << "}";
    return s.toStdString();
}

std::string HttpServer::buildDevicesJson() const
{
    auto& e = const_cast<AudioEngine&>(engine);
    const auto ins  = e.getInputDeviceNames();
    const auto outs = e.getOutputDeviceNames();

    juce::String s;
    s << "{\"in\":[";
    for (int i = 0; i < ins.size(); ++i)  { if (i > 0) s << ","; s << "\"" << jsonEscape (ins[i])  << "\""; }
    s << "],\"out\":[";
    for (int i = 0; i < outs.size(); ++i) { if (i > 0) s << ","; s << "\"" << jsonEscape (outs[i]) << "\""; }
    s << "],\"current\":{\"in\":\""  << jsonEscape (e.getCurrentInputDevice())  << "\""
                  << ",\"out\":\""<< jsonEscape (e.getCurrentOutputDevice()) << "\"}}";
    return s.toStdString();
}

// ---------------------------------------------------------------------------
//  Routes
// ---------------------------------------------------------------------------
void HttpServer::setupRoutes()
{
    // --- UI ---
    server->Get ("/", [] (const httplib::Request&, httplib::Response& res)
    {
        res.set_content (std::string (WebAssets::index_html, WebAssets::index_htmlSize),
                         "text/html; charset=utf-8");
    });

    // --- State / devices ---
    server->Get ("/api/state",   [this] (const httplib::Request&, httplib::Response& res)
                                 { res.set_content (buildStateJson(),   "application/json"); });
    server->Get ("/api/devices", [this] (const httplib::Request&, httplib::Response& res)
                                 { res.set_content (buildDevicesJson(), "application/json"); });

    // --- Master ---
    server->Post ("/api/onair", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setOnAir (parseBoolBody (req.body));
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/source", [this] (const httplib::Request& req, httplib::Response& res)
    {
        if      (req.body == "generator") engine.setSource (AudioEngine::Source::Generator);
        else if (req.body == "input")     engine.setSource (AudioEngine::Source::AudioInput);
        else if (req.body == "file")      engine.setSource (AudioEngine::Source::File);
        res.set_content (buildStateJson(), "application/json");
    });

    // --- Test generator ---
    server->Post ("/api/gen/freq", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setGenFrequency (parseFloatBody (req.body, engine.getGenFrequency()));
        res.set_content (buildStateJson(), "application/json");
    });
    server->Post ("/api/gen/gain", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setGenGainDb (parseFloatBody (req.body, engine.getGenGainDb()));
        res.set_content (buildStateJson(), "application/json");
    });

    // --- Device selection ---
    server->Post ("/api/device/in", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto err = engine.setInputDevice (juce::String (req.body));
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json");
    });
    server->Post ("/api/device/out", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto err = engine.setOutputDevice (juce::String (req.body));
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json");
    });

    // --- Generic DSP parameter setter ---
    server->Post ("/api/param", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto parsed = juce::JSON::parse (juce::String (req.body));
        if (auto* obj = parsed.getDynamicObject())
        {
            const auto path  = obj->getProperty ("path").toString();
            const float value = static_cast<float> ((double) obj->getProperty ("value"));
            if (! engine.getDsp().setParamByPath (path, value))
            {
                res.status = 400;
                res.set_content ("unknown path: " + path.toStdString(), "text/plain");
                return;
            }
        }
        else
        {
            res.status = 400;
            res.set_content ("invalid JSON body", "text/plain");
            return;
        }
        res.set_content (buildStateJson(), "application/json");
    });

    // --- Presets (built-in) ---
    server->Post ("/api/preset", [this] (const httplib::Request& req, httplib::Response& res)
    {
        applyPreset (engine.getDsp(), juce::String (req.body).trim());
        res.set_content (buildStateJson(), "application/json");
    });

    // --- User presets (custom JSON in ~/Library/Application Support/UG6Broadcaster/presets) ---
    server->Get ("/api/presets", [this] (const httplib::Request&, httplib::Response& res)
    {
        const auto names = presets.listNames();
        juce::String j = "{\"presets\":[";
        for (int i = 0; i < names.size(); ++i)
        {
            if (i > 0) j << ",";
            j << "\"" << jsonEscape (names[i]) << "\"";
        }
        j << "],\"directory\":\"" << jsonEscape (presets.getDirectory().getFullPathName()) << "\"}";
        res.set_content (j.toStdString(), "application/json");
    });

    server->Post ("/api/preset/save", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto name = juce::String (req.body).trim();
        if (name.isEmpty()) { res.status = 400; res.set_content ("nom vide", "text/plain"); return; }
        const auto err = presets.save (name, engine.getDsp());
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/preset/load", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto name = juce::String (req.body).trim();
        const auto err = presets.load (name, engine.getDsp());
        if (err.isNotEmpty()) { res.status = 404; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/preset/delete", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto name = juce::String (req.body).trim();
        if (! presets.remove (name)) { res.status = 404; res.set_content ("preset introuvable", "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json");
    });

    // --- File player ---
    server->Post ("/api/file/upload", [this] (const httplib::Request& req, httplib::Response& res)
    {
        if (! req.has_file ("audio"))
        {
            res.status = 400;
            res.set_content ("missing 'audio' file field in multipart body", "text/plain");
            return;
        }

        const auto& f = req.get_file_value ("audio");

        // Persist to a temp location so the AudioFormatReader can mmap / stream from disk.
        const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("UG6Broadcaster");
        tempDir.createDirectory();

        const auto ext = juce::File (f.filename).getFileExtension();
        const auto tempFile = tempDir.getChildFile ("upload" + ext);
        tempFile.deleteFile();
        if (! tempFile.replaceWithData (f.content.data(), f.content.size()))
        {
            res.status = 500;
            res.set_content ("could not write temp file " + tempFile.getFullPathName().toStdString(), "text/plain");
            return;
        }

        const auto err = engine.getFilePlayer().loadFile (tempFile);
        if (err.isNotEmpty())
        {
            res.status = 400;
            res.set_content (err.toStdString(), "text/plain");
            return;
        }

        // Switching the source over makes the upload immediately audible.
        engine.setSource (AudioEngine::Source::File);
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/file/play", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getFilePlayer().play();
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/file/pause", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getFilePlayer().pause();
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/file/stop", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getFilePlayer().stop();
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/file/loop", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.getFilePlayer().setLooping (parseBoolBody (req.body));
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/file/seek", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.getFilePlayer().setPositionSeconds (parseFloatBody (req.body, 0.0f));
        res.set_content (buildStateJson(), "application/json");
    });

    // --- Broadcast (Icecast push) ---
    server->Post ("/api/broadcast/config", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto parsed = juce::JSON::parse (juce::String (req.body));
        if (! parsed.isObject()) { res.status = 400; res.set_content ("invalid JSON", "text/plain"); return; }

        BroadcastSender::Config cfg = engine.getBroadcastSender().getConfig();
        const auto get = [&parsed] (const char* k, const juce::var& fallback) { return parsed.getProperty (k, fallback); };

        cfg.host         = get ("host",     cfg.host).toString();
        cfg.port         = static_cast<int> (get ("port",       cfg.port));
        cfg.mount        = get ("mount",    cfg.mount).toString();
        cfg.user         = get ("user",     cfg.user).toString();
        cfg.password     = get ("password", cfg.password).toString();
        cfg.streamName   = get ("name",     cfg.streamName).toString();
        cfg.streamGenre  = get ("genre",    cfg.streamGenre).toString();
        cfg.bitrateKbps  = static_cast<int> (get ("bitrate",    cfg.bitrateKbps));

        engine.getBroadcastSender().setConfig (cfg);
        instanceConfig.saveBroadcastConfig (cfg);   // persist immediately to disk
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/broadcast/connect", [this] (const httplib::Request&, httplib::Response& res)
    {
        if (! BroadcastSender::isAvailable())
        {
            res.status = 503;
            res.set_content ("encodeur indisponible — installe LAME + libshout puis recompile :\n  brew install lame libshout",
                             "text/plain");
            return;
        }
        engine.getBroadcastSender().connect();
        res.set_content (buildStateJson(), "application/json");
    });

    server->Post ("/api/broadcast/disconnect", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getBroadcastSender().disconnect();
        res.set_content (buildStateJson(), "application/json");
    });

    // --- SSE meters ---
    server->Get ("/api/meters", [this] (const httplib::Request&, httplib::Response& res)
    {
        res.set_header ("Cache-Control", "no-cache");
        res.set_chunked_content_provider (
            "text/event-stream",
            [this] (std::size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                if (! running.load()) { sink.done(); return false; }

                const auto data = "data: " + buildStateJson() + "\n\n";
                if (! sink.write (data.c_str(), data.size())) return false;

                std::this_thread::sleep_for (std::chrono::milliseconds (50));
                return true;
            });
    });
}
