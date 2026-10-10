#include "InstanceConfig.h"

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
    if (! configFile.existsAsFile()) return st;

    const auto parsed = juce::JSON::parse (configFile.loadFileAsString());
    if (! parsed.isObject()) return st;
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

    if (st.dspJson.isNotEmpty())
    {
        const auto dsp = juce::JSON::parse (st.dspJson);
        if (dsp.isObject()) root->setProperty ("dsp", dsp);
    }

    const auto tmp = configFile.getSiblingFile (configFile.getFileName() + ".tmp");
    if (! tmp.replaceWithText (juce::JSON::toString (juce::var (root)), false, false)) return false;
    return tmp.moveFileTo (configFile);
}
