#include "InstanceConfig.h"

namespace
{
    juce::String esc (const juce::String& s) { return s.replace ("\"", "\\\""); }
}

InstanceConfig::InstanceConfig (int instancePort)
    : port (instancePort)
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("UG6Broadcaster")
                    .getChildFile ("instances");
    dir.createDirectory();
    configFile = dir.getChildFile ("config-port-" + juce::String (port) + ".json");
}

BroadcastSender::Config InstanceConfig::loadBroadcastConfig() const
{
    BroadcastSender::Config cfg;   // baseline defaults
    if (! configFile.existsAsFile()) return cfg;

    const auto parsed = juce::JSON::parse (configFile.loadFileAsString());
    if (! parsed.isObject()) return cfg;

    const auto ice = parsed.getProperty ("icecast", juce::var());
    if (! ice.isObject()) return cfg;

    auto get = [&ice] (const char* k, const juce::var& fb) { return ice.getProperty (k, fb); };

    cfg.host         = get ("host",     cfg.host).toString();
    cfg.port         = static_cast<int> (get ("port",     cfg.port));
    cfg.mount        = get ("mount",    cfg.mount).toString();
    cfg.user         = get ("user",     cfg.user).toString();
    cfg.password     = get ("password", cfg.password).toString();
    cfg.streamName   = get ("name",     cfg.streamName).toString();
    cfg.streamGenre  = get ("genre",    cfg.streamGenre).toString();
    cfg.bitrateKbps  = static_cast<int> (get ("bitrate",  cfg.bitrateKbps));
    return cfg;
}

bool InstanceConfig::saveBroadcastConfig (const BroadcastSender::Config& cfg) const
{
    juce::String j;
    j << "{\n"
      << "  \"version\": 1,\n"
      << "  \"port\": " << port << ",\n"
      << "  \"icecast\": {\n"
      << "    \"host\":     \"" << esc (cfg.host)        << "\",\n"
      << "    \"port\":     "    << cfg.port              << ",\n"
      << "    \"mount\":    \"" << esc (cfg.mount)       << "\",\n"
      << "    \"user\":     \"" << esc (cfg.user)        << "\",\n"
      << "    \"password\": \"" << esc (cfg.password)    << "\",\n"
      << "    \"name\":     \"" << esc (cfg.streamName)  << "\",\n"
      << "    \"genre\":    \"" << esc (cfg.streamGenre) << "\",\n"
      << "    \"bitrate\":  "    << cfg.bitrateKbps      << "\n"
      << "  }\n"
      << "}\n";

    return configFile.replaceWithText (j, false, false);
}
