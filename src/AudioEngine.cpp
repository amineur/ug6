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
    return deviceManager.setAudioDeviceSetup (setup, true);
}

juce::String AudioEngine::setOutputDevice (const juce::String& name)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = name;
    setup.useDefaultOutputChannels = true;
    return deviceManager.setAudioDeviceSetup (setup, true);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    genPhase   = 0.0;
    const int blockSize = device->getCurrentBufferSizeSamples();
    dsp.prepare (sampleRate, blockSize, 2);
    filePlayer.prepare (sampleRate, blockSize);
    broadcaster.prepare (sampleRate);
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

    // ---------- 3. ON AIR gate — mute final output if not live ----------
    const bool onAirNow = onAir.load (std::memory_order_relaxed);
    if (! onAirNow)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputs[ch] != nullptr)
                std::memset (outputs[ch], 0, sizeof (float) * static_cast<size_t> (numSamples));
    }

    // ---------- 4. Push to Icecast encoder (silence when off-air) ----------
    // ON AIR off => the output buffer is now zeros, so listeners hear silence.
    broadcaster.pushAudio (outputs, numOutputChannels, numSamples);
}
