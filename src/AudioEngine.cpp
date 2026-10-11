#include "AudioEngine.h"

#include <cmath>
#include <cstring>

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    shutdown();
}

void AudioEngine::initialise()
{
    // Try stereo in + stereo out first. Fall back to output-only if no input device.
    juce::String err = deviceManager.initialiseWithDefaultDevices (2, 2);
    if (err.isNotEmpty())
    {
        DBG ("AudioDeviceManager init (2,2) failed: " << err << " — retrying with output only");
        err = deviceManager.initialiseWithDefaultDevices (0, 2);
        if (err.isNotEmpty())
            DBG ("AudioDeviceManager init (0,2) also failed: " << err);
    }

    deviceManager.addAudioCallback (this);
}

void AudioEngine::shutdown()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::getCurrentInputDevice() const
{
    auto setup = const_cast<juce::AudioDeviceManager&> (deviceManager).getAudioDeviceSetup();
    return setup.inputDeviceName.isEmpty() ? juce::String ("<default>") : setup.inputDeviceName;
}

juce::String AudioEngine::getCurrentOutputDevice() const
{
    auto setup = const_cast<juce::AudioDeviceManager&> (deviceManager).getAudioDeviceSetup();
    return setup.outputDeviceName.isEmpty() ? juce::String ("<default>") : setup.outputDeviceName;
}

juce::StringArray AudioEngine::getInputDeviceNames()
{
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (! type) return {};
    type->scanForDevices();
    return type->getDeviceNames (true /*inputs*/);
}

juce::StringArray AudioEngine::getOutputDeviceNames()
{
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (! type) return {};
    type->scanForDevices();
    return type->getDeviceNames (false /*outputs*/);
}

juce::String AudioEngine::setInputDevice (const juce::String& name)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = name;
    setup.useDefaultInputChannels = true;
    setup.sampleRate = 44100.0;          // comme Oddcast (« 44100Hz/Stereo »)
    return deviceManager.setAudioDeviceSetup (setup, true);
}

juce::String AudioEngine::setOutputDevice (const juce::String& name)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = name;
    setup.useDefaultOutputChannels = true;
    setup.sampleRate = 44100.0;
    return deviceManager.setAudioDeviceSetup (setup, true);
}

juce::String AudioEngine::getCurrentDriverType() const
{
    auto* type = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentDeviceTypeObject();
    return type != nullptr ? type->getTypeName() : juce::String();
}

juce::StringArray AudioEngine::getAvailableDriverTypes()
{
    juce::StringArray names;
    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t != nullptr) names.add (t->getTypeName());
    return names;
}

juce::String AudioEngine::setDriverType (const juce::String& typeName)
{
    // Switching the backend reinitialises the device list and may open a new
    // default device. JUCE handles the close/reopen internally.
    deviceManager.setCurrentAudioDeviceType (typeName, true);

    // Verify the switch took.
    if (getCurrentDriverType() != typeName)
        return "impossible de basculer sur le driver « " + typeName + " »";
    return {};
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    genPhase   = 0.0;
    const int blockSize = device->getCurrentBufferSizeSamples();
    dsp.prepare (sampleRate, blockSize, 2);
    filePlayer.prepare (sampleRate, blockSize);
    for (auto& b : broadcasters) b.prepare (sampleRate);

    silenceBuffer.setSize (2, blockSize, false, true, true);
    silenceBuffer.clear();
}

void AudioEngine::audioDeviceStopped()
{
    dsp.reset();
    filePlayer.releaseResources();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs,
                                                    int numInputChannels,
                                                    float* const* outputs,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    if (numOutputChannels == 0 || numSamples == 0) return;

    const Source src = source.load (std::memory_order_relaxed);

    // ---------- 1. Fill outputs with the source (input or test gen) ----------
    if (src == Source::Generator)
    {
        const float lin = juce::Decibels::decibelsToGain (genGainDb.load (std::memory_order_relaxed));
        const double f  = static_cast<double> (genFreqHz.load (std::memory_order_relaxed));
        const double phaseInc = (juce::MathConstants<double>::twoPi * f) / sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const float s = static_cast<float> (std::sin (genPhase)) * lin;
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputs[ch] != nullptr)
                    outputs[ch][i] = s;
            genPhase += phaseInc;
            if (genPhase >= juce::MathConstants<double>::twoPi)
                genPhase -= juce::MathConstants<double>::twoPi;
        }
    }
    else if (src == Source::AudioInput)
    {
        // Map input channels to outputs: if input is mono and output is stereo,
        // duplicate the mono signal to both output channels. If no inputs at all,
        // output silence.
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputs[ch] == nullptr) continue;

            if (numInputChannels > 0)
            {
                const int srcCh = juce::jmin (ch, numInputChannels - 1);
                if (inputs[srcCh] != nullptr)
                {
                    std::memcpy (outputs[ch], inputs[srcCh],
                                 sizeof (float) * static_cast<size_t> (numSamples));
                    continue;
                }
            }
            std::memset (outputs[ch], 0, sizeof (float) * static_cast<size_t> (numSamples));
        }

        // Secours sur silence : crete de l'entree comparee a -50 dBFS (silence) / -40 dBFS (son revenu)
        if (fallbackOn.load (std::memory_order_relaxed) && filePlayer.hasFile())
        {
            float pk = 0.0f;
            for (int ch = 0; ch < juce::jmin (2, numOutputChannels); ++ch)
                if (outputs[ch] != nullptr)
                    for (int i = 0; i < numSamples; ++i) pk = juce::jmax (pk, std::abs (outputs[ch][i]));
            const bool active = fallbackActive.load (std::memory_order_relaxed);
            if (pk < 0.00316f) { silentSamples += numSamples; soundSamples = 0; }
            else if (pk > 0.01f) { soundSamples += numSamples; silentSamples = 0; }
            if (! active && silentSamples > (int64_t) (fallbackSeconds.load (std::memory_order_relaxed) * sampleRate))
            {
                fallbackActive.store (true, std::memory_order_relaxed);
                filePlayer.play();
            }
            else if (active && soundSamples > (int64_t) (2.0 * sampleRate))
            {
                fallbackActive.store (false, std::memory_order_relaxed);
                filePlayer.pause();
            }
            if (fallbackActive.load (std::memory_order_relaxed))
            {
                juce::AudioBuffer<float> fileBuf (outputs, numOutputChannels, numSamples);
                fileBuf.clear();
                filePlayer.getNextAudioBlock (fileBuf);
            }
        }
        else if (fallbackActive.load (std::memory_order_relaxed))
        {
            fallbackActive.store (false, std::memory_order_relaxed);
            filePlayer.pause();
        }
    }
    else // Source::File
    {
        // Pull audio from the FilePlayer (handles its own decoding + resampling).
        juce::AudioBuffer<float> fileBuf (outputs, numOutputChannels, numSamples);
        fileBuf.clear();
        filePlayer.getNextAudioBlock (fileBuf);
    }

    // ---------- 2. Wrap outputs in an AudioBuffer view and run the DSP chain ----------
    juce::AudioBuffer<float> outBuf (outputs, numOutputChannels, numSamples);
    dsp.process (outBuf);

    // ---------- 3. Push to Icecast encoder (ON AIR gates the stream) ----------
    // ON AIR off => listeners get silence, but local monitoring stays untouched
    // so the operator can preview without going live.
    const bool onAirNow   = onAir.load          (std::memory_order_relaxed);
    const bool monitorNow = monitorEnabled.load (std::memory_order_relaxed);

    if (onAirNow)
    {
        for (auto& b : broadcasters) b.pushAudio (outputs, numOutputChannels, numSamples);
    }
    else
    {
        // Pre-allocated zero buffer — no allocation in audio thread.
        float* silentChans[2] = { silenceBuffer.getWritePointer (0),
                                  silenceBuffer.getWritePointer (1) };
        for (auto& b : broadcasters) b.pushAudio (silentChans, 2, numSamples);
    }

    // ---------- 4. Monitor gate — mute the speakers if monitoring disabled ----------
    // Independent of ON AIR : you can preview off-air, or stream silently.
    if (! monitorNow)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputs[ch] != nullptr)
                std::memset (outputs[ch], 0, sizeof (float) * static_cast<size_t> (numSamples));
    }
}
