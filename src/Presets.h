#pragma once

#include <juce_core/juce_core.h>

class DspChain;

/**
 *  User preset store.
 *
 *  Presets are JSON snapshots of the DspChain parameters, written to:
 *    macOS : ~/Library/Application Support/UG6Broadcaster/presets/<name>.json
 *    Win   : %APPDATA%/Roaming/UG6Broadcaster/presets/<name>.json
 *
 *  Saving / loading goes through DspChain::paramsToJson() and
 *  DspChain::setParamByPath() — same surface as the live HTTP API, so the
 *  preset format and the live state format are identical by construction.
 */
class PresetsStore
{
public:
    PresetsStore();

    /** Returns the absolute path of the presets directory (creates it if needed). */
    juce::File getDirectory() const { return presetsDir; }

    /** Save current DspChain state under `name`. Overwrites if exists.
        Returns empty String on success, error message otherwise. */
    juce::String save (const juce::String& name, const DspChain& dsp);

    /** Load a preset by name and apply to `dsp`.
        Returns empty String on success, error message otherwise. */
    juce::String load (const juce::String& name, DspChain& dsp);

    /** Names of all user presets currently on disk (sorted). */
    juce::StringArray listNames() const;

    /** Delete a preset. Returns true on success. */
    bool remove (const juce::String& name);

private:
    juce::File presetsDir;

    static juce::String sanitiseName (const juce::String& raw);
};
