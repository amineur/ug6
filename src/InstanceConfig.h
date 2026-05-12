#pragma once

#include "BroadcastSender.h"

#include <juce_core/juce_core.h>

/**
 *  Per-instance persistent configuration.
 *
 *  Each UG6 process is associated with a port (8000, 8001, …). The instance
 *  config is keyed on that port and stored under :
 *    macOS : ~/Library/Application Support/UG6Broadcaster/instances/config-port-<N>.json
 *    Win   : %APPDATA%/Roaming/UG6Broadcaster/instances/config-port-<N>.json
 *
 *  Currently saves the Icecast push config (host, port, mount, auth, bitrate, …).
 *  Layout uses a top-level envelope so we can add more sections later without
 *  breaking existing files.
 */
class InstanceConfig
{
public:
    explicit InstanceConfig (int instancePort);

    juce::File getConfigFile() const { return configFile; }

    /** Returns a default-constructed Config if no file exists yet. */
    BroadcastSender::Config loadBroadcastConfig() const;

    /** Writes the icecast section to disk. Atomic-ish via JUCE replaceWithText. */
    bool saveBroadcastConfig (const BroadcastSender::Config& cfg) const;

private:
    juce::File configFile;
    int port;
};
