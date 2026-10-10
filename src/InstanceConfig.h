#pragma once

#include "BroadcastSender.h"

#include <juce_core/juce_core.h>

#include <vector>

/**
 *  Per-instance persistent configuration (une instance = une radio = un port).
 *    macOS : ~/Library/Application Support/UG6Broadcaster/instances/config-port-<N>.json
 *    Win   : %APPDATA%/Roaming/UG6Broadcaster/instances/config-port-<N>.json
 *
 *  Pour remplacer Oddcast sans surveillance, tout est garde : les envois (Encoder Settings + AutoConnect), le pilote
 *  et l'entree son (la Line de la carte virtuelle), la source, ON AIR et le reglage du traitement. Au redemarrage,
 *  l'instance repart toute seule. Ancien format (une seule section « icecast ») relu comme premier envoi.
 */
struct InstanceState
{
    std::vector<BroadcastSender::Config> encoders;
    juce::String driver, input, output;
    juce::String source { "generator" };     // generator | input | file
    bool onAir   { false };
    bool monitor { false };
    juce::String dspJson;                     // DspChain::paramsToJson()
    bool loaded  { false };                   // un fichier existait
};

class InstanceConfig
{
public:
    explicit InstanceConfig (int instancePort);

    juce::File getConfigFile() const { return configFile; }

    InstanceState load (int numEncoders) const;
    bool save (const InstanceState& st) const;

private:
    juce::File configFile;
    int port;
};
