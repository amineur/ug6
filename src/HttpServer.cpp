#include "HttpServer.h"
#include "AudioEngine.h"
#include "BroadcastSender.h"

#include <httplib.h>

// JUCE-generated binary data — see juce_add_binary_data() in CMakeLists.txt.
#include "WebAssets.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <vector>

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
    restore();
}

// Au demarrage : pilote, entree son (la Line de la carte virtuelle), source, ON AIR, reglage du traitement, puis les
// envois coches « AutoConnect » se connectent seuls — l'instance remplace Oddcast sans qu'on touche a rien.
void HttpServer::restore()
{
    const auto st = instanceConfig.load (AudioEngine::kNumEncoders);
    for (int i = 0; i < AudioEngine::kNumEncoders; ++i)
        engine.getBroadcastSender (i).setConfig (st.encoders[(size_t) i]);
    if (! st.loaded) return;

    if (st.driver.isNotEmpty() && st.driver != engine.getCurrentDriverType()) engine.setDriverType (st.driver);
    if (st.input.isNotEmpty())  engine.setInputDevice (st.input);
    if (st.output.isNotEmpty()) engine.setOutputDevice (st.output);
    engine.setSource (st.source == "input" ? AudioEngine::Source::AudioInput
                    : st.source == "file"  ? AudioEngine::Source::File : AudioEngine::Source::Generator);
    engine.setMonitorEnabled (st.monitor);
    if (st.dspJson.isNotEmpty()) PresetsStore::applyJson (st.dspJson, engine.getDsp());
    engine.setOnAir (st.onAir);
    for (int i = 0; i < AudioEngine::kNumEncoders; ++i)
        if (st.encoders[(size_t) i].enabled) engine.getBroadcastSender (i).connect();
    std::cout << "[UG6] etat restaure : entree « " << st.input << " », source " << st.source
              << (st.onAir ? ", ON AIR" : "") << "\n" << std::flush;
}

void HttpServer::persist()
{
    const juce::ScopedLock sl (persistLock);
    InstanceState st;
    for (int i = 0; i < AudioEngine::kNumEncoders; ++i) st.encoders.push_back (engine.getBroadcastSender (i).getConfig());
    st.driver  = engine.getCurrentDriverType();
    st.input   = engine.getCurrentInputDevice();  if (st.input == "<default>")  st.input = {};
    st.output  = engine.getCurrentOutputDevice(); if (st.output == "<default>") st.output = {};
    st.source  = engine.getSource() == AudioEngine::Source::AudioInput ? "input"
               : engine.getSource() == AudioEngine::Source::File ? "file" : "generator";
    st.onAir   = engine.isOnAir();
    st.monitor = engine.isMonitorEnabled();
    st.dspJson = engine.getDsp().paramsToJson();
    instanceConfig.save (st);
}

// « Importer depuis Oddcast » : lit les fichiers de reglage d'Oddcast (*.cfg, cle=valeur) du dossier donne et
// recopie chaque encodeur (serveur, port, point de montage, mot de passe, format, debit). Les mots de passe
// restent dans UG6 : la reponse n'en contient aucun.
std::string HttpServer::importOddcast (const juce::String& folderPath)
{
    const juce::File folder (folderPath.trim().unquoted());
    if (! folder.isDirectory()) return "{\"error\":\"dossier introuvable\"}";

    auto files = folder.findChildFiles (juce::File::findFiles, true, "*.cfg");
    files.sort();
    struct Found { BroadcastSender::Config cfg; juce::String file, encode; };
    std::vector<Found> found;
    juce::String recDevice;

    for (const auto& f : files)
    {
        juce::StringPairArray kv;
        juce::StringArray lines; lines.addLines (f.loadFileAsString());
        for (const auto& l : lines)
        {
            const int eq = l.indexOfChar ('=');
            if (eq > 0) kv.set (l.substring (0, eq).trim().toLowerCase(), l.substring (eq + 1).trim());
        }
        for (const auto& k : { "advrecdevice", "windowsrecdevice", "recdevice", "recordingdevice" })
            if (kv.containsKey (k) && kv[k].isNotEmpty() && recDevice.isEmpty()) recDevice = kv[k];

        const auto mount = kv["servermountpoint"];
        const auto host  = kv["server"].isNotEmpty() ? kv["server"] : kv["serverip"];
        if (mount.isEmpty() || host.isEmpty()) continue;           // pas un encodeur

        Found e; e.file = f.getFileName();
        auto& c = e.cfg;
        c.host  = host;
        c.port  = kv["port"].isNotEmpty() ? kv["port"].getIntValue() : kv["serverport"].getIntValue();
        c.mount = mount.startsWithChar ('/') ? mount : "/" + mount;
        c.user  = kv["serveruser"].isNotEmpty() ? kv["serveruser"] : juce::String ("source");
        c.password = kv["serverpassword"];
        c.streamName  = kv["servername"].isNotEmpty() ? kv["servername"] : kv["serverdescription"];
        c.streamGenre = kv["servergenre"];
        c.bitrateKbps = kv["bitratenominal"].getIntValue() > 0 ? kv["bitratenominal"].getIntValue() : 128;
        e.encode = kv["encode"].isNotEmpty() ? kv["encode"] : kv["encodertype"];
        const auto enc = e.encode.toLowerCase().removeCharacters (" _-");
        c.codec = enc.contains ("aac") && (enc.contains ("v2") || enc.contains ("ps")) ? BroadcastSender::Codec::AACPlusV2
                : enc.contains ("aac") && (enc.contains ("+") || enc.contains ("plus") || enc.contains ("he")) ? BroadcastSender::Codec::AACPlus
                : enc.contains ("aac") ? BroadcastSender::Codec::AAC : BroadcastSender::Codec::MP3;
        c.enabled = false;                                          // on ne se connecte qu'a la demande (essai d'abord)
        c.label   = juce::String (BroadcastSender::codecToString (c.codec)).toUpperCase() + " " + juce::String (c.bitrateKbps) + " " + c.mount;
        found.push_back (e);
        if ((int) found.size() >= AudioEngine::kNumEncoders) break;
    }

    juce::String j = "{\"input\":\"" + juce::String (jsonEscape (recDevice)) + "\",\"encoders\":[";
    for (size_t i = 0; i < found.size(); ++i)
    {
        auto& s0 = engine.getBroadcastSender ((int) i);
        s0.disconnect();
        s0.setConfig (found[i].cfg);
        const auto& c = found[i].cfg;
        if (i > 0) j << ",";
        j << "{\"file\":\"" << jsonEscape (found[i].file) << "\",\"encode\":\"" << jsonEscape (found[i].encode)
          << "\",\"host\":\"" << jsonEscape (c.host) << "\",\"port\":" << c.port << ",\"mount\":\"" << jsonEscape (c.mount)
          << "\",\"codec\":\"" << BroadcastSender::codecToString (c.codec) << "\",\"bitrate\":" << c.bitrateKbps
          << ",\"passwordSet\":" << (c.password.isNotEmpty() ? "true" : "false") << "}";
    }
    j << "],\"files\":" << files.size() << "}";
    if (! found.empty()) persist();
    return j.toStdString();
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
      << "\"onAir\":"   << (engine.isOnAir() ? "true" : "false")
      << ",\"monitor\":" << (engine.isMonitorEnabled() ? "true" : "false")
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
      << ",\"broadcast\":" << engine.getBroadcastSender (0).toJson();
    s << ",\"encoders\":[";
    for (int i = 0; i < AudioEngine::kNumEncoders; ++i) { if (i > 0) s << ","; s << engine.getBroadcastSender (i).toJson(); }
    s << "]}";
    return s.toStdString();
}

std::string HttpServer::buildDevicesJson() const
{
    auto& e = const_cast<AudioEngine&>(engine);
    const auto ins     = e.getInputDeviceNames();
    const auto outs    = e.getOutputDeviceNames();
    const auto drivers = e.getAvailableDriverTypes();

    juce::String s;
    s << "{\"in\":[";
    for (int i = 0; i < ins.size(); ++i)  { if (i > 0) s << ","; s << "\"" << jsonEscape (ins[i])  << "\""; }
    s << "],\"out\":[";
    for (int i = 0; i < outs.size(); ++i) { if (i > 0) s << ","; s << "\"" << jsonEscape (outs[i]) << "\""; }
    s << "],\"drivers\":[";
    for (int i = 0; i < drivers.size(); ++i) { if (i > 0) s << ","; s << "\"" << jsonEscape (drivers[i]) << "\""; }
    s << "],\"current\":{\"in\":\""  << jsonEscape (e.getCurrentInputDevice())  << "\""
                  << ",\"out\":\""<< jsonEscape (e.getCurrentOutputDevice()) << "\""
                  << ",\"driver\":\""<< jsonEscape (e.getCurrentDriverType()) << "\"}}";
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
                                 { res.set_content (buildStateJson(),   "application/json; charset=utf-8"); });
    server->Get ("/api/devices", [this] (const httplib::Request&, httplib::Response& res)
                                 { res.set_content (buildDevicesJson(), "application/json; charset=utf-8"); });

    // --- Master ---
    server->Post ("/api/onair", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setOnAir (parseBoolBody (req.body));
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/monitor", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setMonitorEnabled (parseBoolBody (req.body));
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/source", [this] (const httplib::Request& req, httplib::Response& res)
    {
        if      (req.body == "generator") engine.setSource (AudioEngine::Source::Generator);
        else if (req.body == "input")     engine.setSource (AudioEngine::Source::AudioInput);
        else if (req.body == "file")      engine.setSource (AudioEngine::Source::File);
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    // --- Test generator ---
    server->Post ("/api/gen/freq", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setGenFrequency (parseFloatBody (req.body, engine.getGenFrequency()));
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });
    server->Post ("/api/gen/gain", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.setGenGainDb (parseFloatBody (req.body, engine.getGenGainDb()));
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    // --- Device selection ---
    server->Post ("/api/device/in", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto err = engine.setInputDevice (juce::String (req.body));
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });
    server->Post ("/api/device/out", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto err = engine.setOutputDevice (juce::String (req.body));
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/device/driver", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto err = engine.setDriverType (juce::String (req.body).trim());
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
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
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    // --- Presets (built-in) ---
    server->Post ("/api/preset", [this] (const httplib::Request& req, httplib::Response& res)
    {
        applyPreset (engine.getDsp(), juce::String (req.body).trim());
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
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
        res.set_content (j.toStdString(), "application/json; charset=utf-8");
    });

    server->Post ("/api/preset/save", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto name = juce::String (req.body).trim();
        if (name.isEmpty()) { res.status = 400; res.set_content ("nom vide", "text/plain"); return; }
        const auto err = presets.save (name, engine.getDsp());
        if (err.isNotEmpty()) { res.status = 500; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/preset/load", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto name = juce::String (req.body).trim();
        const auto err = presets.load (name, engine.getDsp());
        if (err.isNotEmpty()) { res.status = 404; res.set_content (err.toStdString(), "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/preset/delete", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto name = juce::String (req.body).trim();
        if (! presets.remove (name)) { res.status = 404; res.set_content ("preset introuvable", "text/plain"); return; }
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
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
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/file/play", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getFilePlayer().play();
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/file/pause", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getFilePlayer().pause();
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/file/stop", [this] (const httplib::Request&, httplib::Response& res)
    {
        engine.getFilePlayer().stop();
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/file/loop", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.getFilePlayer().setLooping (parseBoolBody (req.body));
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/file/seek", [this] (const httplib::Request& req, httplib::Response& res)
    {
        engine.getFilePlayer().setPositionSeconds (parseFloatBody (req.body, 0.0f));
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    // --- Broadcast (Icecast push) ---
    server->Post ("/api/broadcast/config", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto parsed = juce::JSON::parse (juce::String (req.body));
        if (! parsed.isObject()) { res.status = 400; res.set_content ("invalid JSON", "text/plain"); return; }

        const int idx = juce::jlimit (0, AudioEngine::kNumEncoders - 1,
                                      req.has_param ("i") ? std::stoi (req.get_param_value ("i")) : static_cast<int> (parsed.getProperty ("index", 0)));
        auto& sender = engine.getBroadcastSender (idx);
        BroadcastSender::Config cfg = sender.getConfig();
        const auto get = [&parsed] (const char* k, const juce::var& fallback) { return parsed.getProperty (k, fallback); };

        cfg.host         = get ("host",     cfg.host).toString();
        cfg.port         = static_cast<int> (get ("port",       cfg.port));
        cfg.mount        = get ("mount",    cfg.mount).toString();
        cfg.user         = get ("user",     cfg.user).toString();
        const auto pw    = get ("password", juce::var()).toString();
        if (pw.isNotEmpty()) cfg.password = pw;            // vide = inchange (la page ne recoit jamais le mot de passe)
        cfg.label        = get ("label",    cfg.label).toString();
        cfg.streamName   = get ("name",     cfg.streamName).toString();
        cfg.streamGenre  = get ("genre",    cfg.streamGenre).toString();
        cfg.bitrateKbps  = static_cast<int> (get ("bitrate",    cfg.bitrateKbps));
        cfg.codec        = BroadcastSender::codecFromString (get ("codec", juce::var (BroadcastSender::codecToString (cfg.codec))).toString());

        sender.setConfig (cfg);
        persist();
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/broadcast/connect", [this] (const httplib::Request& req, httplib::Response& res)
    {
        if (! BroadcastSender::isAvailable())
        {
            res.status = 503;
            res.set_content ("encodeur indisponible — installe LAME puis recompile :\n  brew install lame",
                             "text/plain");
            return;
        }
        const int idx = req.has_param ("i") ? juce::jlimit (0, AudioEngine::kNumEncoders - 1, std::stoi (req.get_param_value ("i"))) : 0;
        auto& sender = engine.getBroadcastSender (idx);
        auto c = sender.getConfig(); c.enabled = true; sender.setConfig (c);    // AutoConnect, comme Oddcast
        sender.connect();
        persist();
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    server->Post ("/api/broadcast/disconnect", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const int idx = req.has_param ("i") ? juce::jlimit (0, AudioEngine::kNumEncoders - 1, std::stoi (req.get_param_value ("i"))) : 0;
        auto& sender = engine.getBroadcastSender (idx);
        auto c = sender.getConfig(); c.enabled = false; sender.setConfig (c);
        sender.disconnect();
        persist();
        res.set_content (buildStateJson(), "application/json; charset=utf-8");
    });

    // --- Titrage : le lecteur (UGPLAN Diff Web) envoie ici le titre en cours, pousse vers Icecast par chaque envoi ---
    server->Post ("/api/metadata", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto song = juce::String::fromUTF8 (req.body.c_str(), static_cast<int> (req.body.size())).trim();
        const auto parsed = juce::JSON::parse (song);
        if (parsed.isObject()) song = parsed.getProperty ("song", "").toString().trim();
        for (int i = 0; i < AudioEngine::kNumEncoders; ++i)
            engine.getBroadcastSender (i).setMetadata (song);
        res.set_content ("{\"ok\":true}", "application/json; charset=utf-8");
    });

    // --- Importer les reglages d'Oddcast (dossier d'installation / de config) ---
    server->Post ("/api/oddcast/import", [this] (const httplib::Request& req, httplib::Response& res)
    {
        res.set_content (importOddcast (juce::String::fromUTF8 (req.body.c_str(), static_cast<int> (req.body.size()))), "application/json; charset=utf-8");
    });

    // Tout changement de reglage est garde sur le disque (redemarrage sans surveillance).
    server->set_post_routing_handler ([this] (const httplib::Request& req, httplib::Response&)
    {
        if (req.method != "POST") return;
        const auto& p = req.path;
        if (p == "/api/onair" || p == "/api/monitor" || p == "/api/source" || p.rfind ("/api/device/", 0) == 0
            || p == "/api/param" || p == "/api/preset" || p == "/api/preset/load")
            persist();
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
