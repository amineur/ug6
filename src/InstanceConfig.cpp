#include "InstanceConfig.h"

#include <iostream>

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
    // Fichier principal vide ou illisible (coupure de courant pendant l'ecriture) : on reprend la copie .bak.
    auto parsed = configFile.existsAsFile() ? juce::JSON::parse (configFile.loadFileAsString()) : juce::var();
    if (! parsed.isObject())
    {
        const auto bak = configFile.getSiblingFile (configFile.getFileName() + ".bak");
        if (! bak.existsAsFile()) return st;
        parsed = juce::JSON::parse (bak.loadFileAsString());
        if (! parsed.isObject()) { std::cerr << "[UG6] config et copie .bak illisibles : " << configFile.getFullPathName() << "\n" << std::flush; return st; }
        std::cerr << "[UG6] config illisible, reprise de la copie .bak : " << bak.getFullPathName() << "\n" << std::flush;
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
    tmp.deleteFile();
    {
        juce::FileOutputStream out (tmp);
        if (out.failedToOpen()) return false;
        out.setPosition (0); out.truncate();
        if (! out.write (text.toRawUTF8(), text.getNumBytesAsUTF8())) return false;
        out.flush();
    }
    if (configFile.existsAsFile() && configFile.getSize() > 0) configFile.copyFileTo (bak);
    return tmp.moveFileTo (configFile);
}
