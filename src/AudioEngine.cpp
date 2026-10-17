#include "AudioEngine.h"

#include <cmath>
#include <cstring>
#include <iostream>

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
    lastCallbackMs.store (juce::Time::getMillisecondCounter());
    startTimer (500);
}

void AudioEngine::shutdown()
{
    stopTimer();
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::getFallbackFile() const
{
    const juce::ScopedLock sl (fallbackFileLock);
    return fallbackFile;
}

void AudioEngine::setFallbackFile (const juce::String& path)
{
    const juce::ScopedLock sl (fallbackFileLock);
    fallbackFile = path;
}

juce::String AudioEngine::getLastDeviceError() const
{
    const juce::ScopedLock sl (deviceErrorLock);
    return lastDeviceError;
}

void AudioEngine::setDeviceError (const juce::String& what, const juce::String& err)
{
    const juce::ScopedLock sl (deviceErrorLock);
    lastDeviceError = err.isEmpty() ? juce::String() : what + " : " + err;
    if (err.isNotEmpty()) std::cerr << "[Audio] " << lastDeviceError << "\n" << std::flush;
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

// Ouvre le peripherique a 44,1 kHz comme Oddcast (« 44100Hz/Stereo ») ; si cette cadence est refusee, on reessaie a
// la cadence courante du peripherique ; si tout echoue, on remet le reglage precedent pour ne pas rester sans son.
// L'erreur est journalisee et gardee pour /api/state (device.error). Appeler sous deviceLock.
juce::String AudioEngine::applyDeviceSetup (juce::AudioDeviceManager::AudioDeviceSetup setup, const juce::String& what)
{
    const auto previous = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = 44100.0;
    auto err = deviceManager.setAudioDeviceSetup (setup, true);
    if (err.isNotEmpty())
    {
        std::cerr << "[Audio] " << what << " a 44100 Hz : " << err << " — nouvel essai a la cadence du peripherique\n" << std::flush;
        setup.sampleRate = previous.sampleRate;      // 0 = laisser JUCE choisir
        err = deviceManager.setAudioDeviceSetup (setup, true);
    }
    if (err.isNotEmpty())
    {
        const auto back = deviceManager.setAudioDeviceSetup (previous, true);
        if (back.isNotEmpty()) std::cerr << "[Audio] retour au reglage precedent impossible : " << back << "\n" << std::flush;
    }
    setDeviceError (what, err);
    return err;
}

juce::String AudioEngine::setInputDevice (const juce::String& name)
{
    const juce::ScopedLock sl (deviceLock);
    wantedInput = name;
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = name;
    setup.useDefaultInputChannels = true;
    return applyDeviceSetup (setup, "entree « " + name + " »");
}

juce::String AudioEngine::setOutputDevice (const juce::String& name)
{
    const juce::ScopedLock sl (deviceLock);
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = name;
    setup.useDefaultOutputChannels = true;
    return applyDeviceSetup (setup, "sortie « " + name + " »");
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
    const juce::ScopedLock sl (deviceLock);
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
    lastCallbackMs.store (juce::Time::getMillisecondCounter());
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

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    setDeviceError ("erreur du peripherique", errorMessage);
}

// Fil des messages, toutes les 500 ms :
//  - secours : play()/pause() du lecteur hors du rappel audio (le rappel ne fait que lever fallbackActive) ;
//  - rappel audio muet > 3 s alors que le peripherique est ouvert : on le ferme et on le rouvre (1 s, 2, 4… 30 s) ;
//  - entree demandee non ouverte : nouvel essai toutes les 10 s (la carte virtuelle peut arriver apres nous) ;
//  - fichier de secours introuvable au demarrage : nouvel essai toutes les 30 s.
void AudioEngine::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();

    const bool wantPlay = fallbackActive.load (std::memory_order_relaxed);
    if (wantPlay != fallbackPlaying)
    {
        fallbackPlaying = wantPlay;
        if (wantPlay) filePlayer.play(); else filePlayer.pause();
        std::cerr << "[Audio] secours " << (wantPlay ? "ACTIF (entree muette)" : "termine (le son est revenu)") << "\n" << std::flush;
    }

    {
        const juce::ScopedLock sl (deviceLock);
        auto* dev = deviceManager.getCurrentAudioDevice();
        const bool shouldRun = dev != nullptr && dev->isPlaying();
        const bool stalled   = shouldRun && now - lastCallbackMs.load (std::memory_order_relaxed) > 3000;
        if (stalled != audioStalled.load())
        {
            audioStalled.store (stalled);
            if (! stalled) { restartBackoffMs = 1000; std::cerr << "[Audio] le rappel audio est reparti\n" << std::flush; }
        }
        if (stalled && now >= nextRestartMs)
        {
            std::cerr << "[Audio] aucun rappel audio depuis 3 s : reouverture du peripherique (prochain essai dans "
                      << restartBackoffMs / 1000 << " s)\n" << std::flush;
            deviceManager.closeAudioDevice();
            deviceManager.restartLastAudioDevice();
            lastCallbackMs.store (now);
            nextRestartMs    = now + (juce::uint32) restartBackoffMs;
            restartBackoffMs = juce::jmin (restartBackoffMs * 2, 30000);
        }

        if (wantedInput.isNotEmpty() && now - lastInputRetryMs >= 10000 && ! stalled)
        {
            lastInputRetryMs = now;
            if (deviceManager.getAudioDeviceSetup().inputDeviceName != wantedInput)
            {
                std::cerr << "[Audio] entree « " << wantedInput << " » toujours absente : nouvel essai\n" << std::flush;
                auto setup = deviceManager.getAudioDeviceSetup();
                setup.inputDeviceName = wantedInput;
                setup.useDefaultInputChannels = true;
                applyDeviceSetup (setup, "entree « " + wantedInput + " »");
            }
        }
    }

    if (fallbackFileMissing.load() && now - lastFallbackRetryMs >= 30000)
    {
        lastFallbackRetryMs = now;
        const auto path = getFallbackFile();
        if (path.isEmpty()) { fallbackFileMissing.store (false); return; }
        const auto err = filePlayer.loadFile (juce::File (path));
        if (err.isEmpty()) { filePlayer.setLooping (true); fallbackFileMissing.store (false);
                             std::cerr << "[Audio] fichier de secours charge : " << path << "\n" << std::flush; }
        else                 std::cerr << "[Audio] fichier de secours toujours illisible (" << err << ") : " << path << "\n" << std::flush;
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs,
                                                    int numInputChannels,
                                                    float* const* outputs,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    lastCallbackMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
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
            // play()/pause() du lecteur sont faits par le Timer (hors du rappel audio) : ici on ne fait que lever le drapeau.
            if (! active && silentSamples > (int64_t) (fallbackSeconds.load (std::memory_order_relaxed) * sampleRate))
                fallbackActive.store (true, std::memory_order_relaxed);
            else if (active && soundSamples > (int64_t) (2.0 * sampleRate))
                fallbackActive.store (false, std::memory_order_relaxed);
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
