#include "Presets.h"
#include "DspChain.h"

namespace
{
    // Walk a parsed JSON tree and call dsp.setParamByPath(path, value) for every
    // numeric / boolean leaf. Unknown paths are silently ignored so we don't
    // crash on legacy preset files that include meters / GR snapshots.
    void applyJsonRecursively (const juce::var& v, const juce::String& prefix, DspChain& dsp)
    {
        if (v.isObject())
        {
            if (auto* obj = v.getDynamicObject())
            {
                for (const auto& prop : obj->getProperties())
                {
                    const juce::String key = prop.name.toString();
                    const juce::String next = prefix.isEmpty() ? key : (prefix + "." + key);
                    applyJsonRecursively (prop.value, next, dsp);
                }
            }
            return;
        }

        if (v.isArray())
        {
            if (auto* arr = v.getArray())
            {
                for (int i = 0; i < arr->size(); ++i)
                {
                    const juce::String next = prefix + "." + juce::String (i);
                    applyJsonRecursively ((*arr)[i], next, dsp);
                }
            }
            return;
        }

        // Leaf — only numerics / bools are meaningful to setParamByPath.
        if (v.isBool())
            dsp.setParamByPath (prefix, static_cast<bool> (v) ? 1.0f : 0.0f);
        else if (v.isInt() || v.isDouble() || v.isInt64())
            dsp.setParamByPath (prefix, static_cast<float> (static_cast<double> (v)));
        // strings / nulls : ignore
    }
}

PresetsStore::PresetsStore()
{
    presetsDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("UG6Broadcaster")
                    .getChildFile ("presets");
    presetsDir.createDirectory();
}

juce::String PresetsStore::sanitiseName (const juce::String& raw)
{
    // Keep ascii letters, digits, dash, underscore, space, and a few punctuation marks.
    juce::String out;
    for (auto c : raw)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c)
            || c == '-' || c == '_' || c == ' ' || c == '.')
            out += juce::String::charToString (c);
    }
    out = out.trim();
    if (out.isEmpty()) out = "preset";
    return out.substring (0, 64);
}

juce::String PresetsStore::save (const juce::String& name, const DspChain& dsp)
{
    const auto safe = sanitiseName (name);
    auto file = presetsDir.getChildFile (safe + ".json");

    // We wrap the params JSON in an envelope so we can add metadata later
    // (version, created-at, etc.) without breaking older clients.
    juce::String body;
    body << "{\"name\":\"" << safe.replace ("\"", "\\\"") << "\""
         << ",\"version\":1"
         << ",\"created\":" << juce::Time::getCurrentTime().toMilliseconds()
         << ",\"dsp\":" << dsp.paramsToJson()
         << "}";

    if (! file.replaceWithText (body, false, false))
        return "Impossible d'écrire le fichier : " + file.getFullPathName();
    return {};
}

juce::String PresetsStore::load (const juce::String& name, DspChain& dsp)
{
    const auto safe = sanitiseName (name);
    auto file = presetsDir.getChildFile (safe + ".json");
    if (! file.existsAsFile())
        return "Preset introuvable : " + safe;

    const auto text = file.loadFileAsString();
    const auto parsed = juce::JSON::parse (text);
    if (! parsed.isObject())
        return "Fichier de preset corrompu : " + safe;

    // Two acceptable shapes :
    //   { "dsp" : { ... } }   — current envelope
    //   { ...params... }       — legacy / plain dump
    juce::var payload = parsed.getProperty ("dsp", parsed);
    applyJsonRecursively (payload, {}, dsp);
    return {};
}

juce::StringArray PresetsStore::listNames() const
{
    juce::StringArray names;
    for (auto& f : presetsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
        names.add (f.getFileNameWithoutExtension());
    names.sort (true);
    return names;
}

bool PresetsStore::remove (const juce::String& name)
{
    const auto safe = sanitiseName (name);
    return presetsDir.getChildFile (safe + ".json").deleteFile();
}
