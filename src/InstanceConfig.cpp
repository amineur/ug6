#include "InstanceConfig.h"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN 1
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #include <windows.h>
#endif

InstanceConfig::InstanceConfig (int instancePort)
    : port (instancePort)
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("UG6Broadcaster")
                    .getChildFile ("instances");
    dir.createDirectory();
    configFile = dir.getChildFile ("config-port-" + juce::String (port) + ".json");
}

namespace
{
    BroadcastSender::Config readEncoder (const juce::var& e)
    {
        BroadcastSender::Config c;
        auto get = [&e] (const char* k, const juce::var& fb) { return e.getProperty (k, fb); };
        c.host        = get ("host",     c.host).toString();
        c.port        = static_cast<int> (get ("port", c.port));
        c.mount       = get ("mount",    c.mount).toString();
        c.user        = get ("user",     c.user).toString();
        c.password    = get ("password", c.password).toString();
        c.streamName  = get ("name",     c.streamName).toString();
        c.streamGenre = get ("genre",    c.streamGenre).toString();
        c.bitrateKbps = static_cast<int> (get ("bitrate", c.bitrateKbps));
        c.codec       = BroadcastSender::codecFromString (get ("codec", juce::var (BroadcastSender::codecToString (c.codec))).toString());
        c.enabled     = static_cast<bool> (get ("enabled", c.enabled));
        c.label       = get ("label", c.label).toString();
        return c;
    }

    juce::var writeEncoder (const BroadcastSender::Config& c)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("host", c.host);           o->setProperty ("port", c.port);
        o->setProperty ("mount", c.mount);         o->setProperty ("user", c.user);
        o->setProperty ("password", c.password);   o->setProperty ("name", c.streamName);
        o->setProperty ("genre", c.streamGenre);   o->setProperty ("bitrate", c.bitrateKbps);
        o->setProperty ("codec", juce::String (BroadcastSender::codecToString (c.codec)));
        o->setProperty ("enabled", c.enabled);     o->setProperty ("label", c.label);
        return juce::var (o);
    }
}

InstanceState InstanceConfig::load (int numEncoders) const
{
    InstanceState st;
    st.encoders.resize ((size_t) numEncoders);
    // Fichier principal absent, vide ou illisible (coupure de courant pendant l'ecriture) : on reprend la copie .bak,
    // puis le .tmp (coupure entre l'ecriture du .tmp et son renommage, sans .bak precedent).
    const auto parseFile = [] (const juce::File& f) { return f.existsAsFile() ? juce::JSON::parse (f.loadFileAsString()) : juce::var(); };
    auto parsed = parseFile (configFile);
    if (! parsed.isObject())
    {
        bool anyFile = configFile.existsAsFile();
        for (const char* ext : { ".bak", ".tmp" })
        {
            const auto other = configFile.getSiblingFile (configFile.getFileName() + ext);
            if (! other.existsAsFile()) continue;
            anyFile = true;
            parsed = parseFile (other);
            if (parsed.isObject())
            {
                std::cerr << "[UG6] config absente ou illisible, reprise de la copie : " << other.getFullPathName() << "\n" << std::flush;
                break;
            }
        }
        if (! parsed.isObject())
        {
            if (anyFile) std::cerr << "[UG6] config, .bak et .tmp illisibles : " << configFile.getFullPathName() << "\n" << std::flush;
            return st;
        }
    }
    st.loaded = true;

    if (auto* arr = parsed.getProperty ("encoders", juce::var()).getArray())
    {
        for (int i = 0; i < arr->size() && i < numEncoders; ++i)
            st.encoders[(size_t) i] = readEncoder ((*arr)[i]);
    }
    else if (parsed.getProperty ("icecast", juce::var()).isObject())       // ancien format : un seul envoi
    {
        st.encoders[0] = readEncoder (parsed.getProperty ("icecast", juce::var()));
    }

    const auto audio = parsed.getProperty ("audio", juce::var());
    if (audio.isObject())
    {
        st.driver  = audio.getProperty ("driver", "").toString();
        st.input   = audio.getProperty ("input", "").toString();
        st.output  = audio.getProperty ("output", "").toString();
        st.source  = audio.getProperty ("source", st.source).toString();
        st.onAir   = static_cast<bool> (audio.getProperty ("onAir", false));
        st.monitor = static_cast<bool> (audio.getProperty ("monitor", false));
    }
    const auto fb = parsed.getProperty ("fallback", juce::var());
    if (fb.isObject())
    {
        st.fallbackFile    = fb.getProperty ("file", "").toString();
        st.fallbackOn      = static_cast<bool> (fb.getProperty ("on", false));
        st.fallbackSeconds = static_cast<double> (fb.getProperty ("seconds", 10.0));
    }
    const auto dsp = parsed.getProperty ("dsp", juce::var());
    if (dsp.isObject()) st.dspJson = juce::JSON::toString (dsp, true);
    return st;
}

bool InstanceConfig::save (const InstanceState& st) const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 2);
    root->setProperty ("port", port);

    juce::Array<juce::var> encs;
    for (const auto& e : st.encoders) encs.add (writeEncoder (e));
    root->setProperty ("encoders", encs);

    auto* audio = new juce::DynamicObject();
    audio->setProperty ("driver", st.driver);   audio->setProperty ("input", st.input);
    audio->setProperty ("output", st.output);   audio->setProperty ("source", st.source);
    audio->setProperty ("onAir", st.onAir);     audio->setProperty ("monitor", st.monitor);
    root->setProperty ("audio", juce::var (audio));

    auto* fb = new juce::DynamicObject();
    fb->setProperty ("file", st.fallbackFile); fb->setProperty ("on", st.fallbackOn); fb->setProperty ("seconds", st.fallbackSeconds);
    root->setProperty ("fallback", juce::var (fb));

    if (st.dspJson.isNotEmpty())
    {
        const auto dsp = juce::JSON::parse (st.dspJson);
        if (dsp.isObject()) root->setProperty ("dsp", dsp);
    }

    // Ecriture sure : .tmp ecrit et vide (flush) sur le disque, copie .bak de l'ancien fichier, puis renommage.
    const auto tmp = configFile.getSiblingFile (configFile.getFileName() + ".tmp");
    const auto bak = configFile.getSiblingFile (configFile.getFileName() + ".bak");
    const auto text = juce::JSON::toString (juce::var (root));
    const auto* bytes = text.toRawUTF8();
    const auto  numBytes = text.getNumBytesAsUTF8();

   #ifdef _WIN32
    // Windows : juce::File::moveFileTo efface d'abord la cible puis renomme — une coupure entre les deux laissait
    // AUCUN fichier de config. Ici : .tmp ecrit puis FlushFileBuffers (donnees sur le disque), et remplacement en
    // une seule operation par MoveFileExW (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
    {
        const HANDLE h = CreateFileW (tmp.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL okWrite = WriteFile (h, bytes, (DWORD) numBytes, &written, nullptr);
        const BOOL okFlush = FlushFileBuffers (h);
        CloseHandle (h);
        if (! okWrite || written != (DWORD) numBytes || ! okFlush) return false;
    }
    if (configFile.existsAsFile() && configFile.getSize() > 0) configFile.copyFileTo (bak);
    return MoveFileExW (tmp.getFullPathName().toWideCharPointer(), configFile.getFullPathName().toWideCharPointer(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
   #else
    tmp.deleteFile();
    {
        juce::FileOutputStream out (tmp);
        if (out.failedToOpen()) return false;
        out.setPosition (0); out.truncate();
        if (! out.write (bytes, numBytes)) return false;
        out.flush();
    }
    if (configFile.existsAsFile() && configFile.getSize() > 0) configFile.copyFileTo (bak);
    // rename() POSIX remplace la cible en une seule operation (moveFileTo l'effacait d'abord).
    return std::rename (tmp.getFullPathName().toRawUTF8(), configFile.getFullPathName().toRawUTF8()) == 0;
   #endif
}
