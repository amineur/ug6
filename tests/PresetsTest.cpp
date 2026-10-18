// Test du chemin preset : un preset sauve puis recharge sur un etat par defaut doit restituer
// exactement tous les reglages, tableaux compris (eq[], xover[], comp[]).
// Lance par ctest (cmake -DUG6_BUILD_TESTS=ON) ; code de sortie 0 = succes.

#include "DspChain.h"
#include "Presets.h"

#include <iostream>

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  ok    " : "  ECHEC ") << what << "\n";
        if (! ok) ++failures;
    }

    // Valeurs toutes differentes des valeurs par defaut, dans les bornes de setParamByPath
    // et exactes a 4 decimales (format de paramsToJson).
    void setNonDefaults (DspChain& d)
    {
        for (int i = 0; i < DspChain::kNumEqBands; ++i)
        {
            const auto p = "eq." + juce::String (i) + ".";
            d.setParamByPath (p + "on",     i % 2 == 0 ? 0.0f : 1.0f);
            d.setParamByPath (p + "freq",   150.0f + 1000.0f * (float) i);
            d.setParamByPath (p + "gainDb", -4.5f + 2.25f * (float) i);
            d.setParamByPath (p + "q",      0.5f + 0.75f * (float) i);
        }
        for (int i = 0; i < DspChain::kNumXovers; ++i)
            d.setParamByPath ("xover." + juce::String (i), 120.0f + 1500.0f * (float) i);
        for (int i = 0; i < DspChain::kNumBands; ++i)
        {
            const auto p = "comp." + juce::String (i) + ".";
            d.setParamByPath (p + "thresholdDb", -30.0f + 1.5f * (float) i);
            d.setParamByPath (p + "ratio",       3.0f + 0.5f * (float) i);
            d.setParamByPath (p + "attackMs",    5.0f + 2.0f * (float) i);
            d.setParamByPath (p + "releaseMs",   80.0f + 25.0f * (float) i);
            d.setParamByPath (p + "makeupDb",    1.0f + 0.5f * (float) i);
        }
        d.setParamByPath ("inputGainDb", 2.5f);
        d.setParamByPath ("clip.ceilingDb", -1.25f);
        d.setParamByPath ("lim.ceilingDb", -2.0f);
    }

    juce::var section (const DspChain& d, const char* key)
    {
        return juce::JSON::parse (d.paramsToJson()).getProperty (key, {});
    }
}

int main()
{
    DspChain source;
    setNonDefaults (source);
    const auto expected = source.paramsToJson();

    check (expected != DspChain().paramsToJson(), "le jeu d'essai differe bien de l'etat par defaut");

    // 1. Sauver puis recharger un preset (meme chemin que /api/preset/save et /api/preset/load).
    PresetsStore store;
    const juce::String name = "zz-ug6-test-" + juce::String (juce::Time::currentTimeMillis());
    check (store.save (name, source).isEmpty(), "sauvegarde du preset");

    DspChain loaded;
    check (store.load (name, loaded).isEmpty(), "chargement du preset");
    store.remove (name);

    for (auto* key : { "eq", "xover", "comp" })
        check (juce::JSON::toString (section (loaded, key)) == juce::JSON::toString (section (source, key)),
               juce::String ("preset recharge : ") + key + " restitue");
    check (loaded.paramsToJson() == expected, "preset recharge : paramsToJson identique");

    // 2. Restauration au demarrage (PresetsStore::applyJson sur l'etat persiste).
    DspChain restored;
    PresetsStore::applyJson (expected, restored);
    check (restored.paramsToJson() == expected, "restauration au demarrage : paramsToJson identique");

    std::cout << (failures == 0 ? "OK\n" : "ECHECS : " + std::to_string (failures) + "\n");
    return failures == 0 ? 0 : 1;
}
