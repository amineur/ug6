#include "FilePlayer.h"

FilePlayer::FilePlayer()
    : readAheadThread ("UG6FileReadAhead")
{
    formatManager.registerBasicFormats();

   #if JUCE_USE_MP3AUDIOFORMAT
    formatManager.registerFormat (new juce::MP3AudioFormat(), false);
   #endif

    readAheadThread.startThread();
}

FilePlayer::~FilePlayer()
{
    {
        const juce::ScopedLock sl (sourceLock);
        transport.setSource (nullptr);
        readerSource.reset();
    }
    readAheadThread.stopThread (2000);
}

void FilePlayer::prepare (double sampleRate, int blockSize)
{
    transport.prepareToPlay (blockSize, sampleRate);
}

void FilePlayer::releaseResources()
{
    transport.releaseResources();
}

juce::String FilePlayer::loadFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return "Le fichier n'existe pas : " + file.getFullPathName();

    auto* reader = formatManager.createReaderFor (file);
    if (reader == nullptr)
        return "Format de fichier non supporté : " + file.getFileName();

    const double srcSampleRate = reader->sampleRate;
    const int    srcChannels   = juce::jmax (1, static_cast<int> (reader->numChannels));
    const double durSec        = (reader->sampleRate > 0)
                                   ? static_cast<double> (reader->lengthInSamples) / reader->sampleRate
                                   : 0.0;

    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
    newSource->setLooping (looping.load (std::memory_order_relaxed));

    {
        const juce::ScopedLock sl (sourceLock);
        transport.stop();
        transport.setSource (nullptr);
        transport.setSource (newSource.get(), 65536, &readAheadThread, srcSampleRate, srcChannels);
        readerSource = std::move (newSource);
        currentFileName = file.getFileName();
    }

    duration.store (durSec, std::memory_order_relaxed);
    hasLoaded.store (true, std::memory_order_relaxed);
    return {};
}

void FilePlayer::play()  { transport.start(); }
void FilePlayer::pause() { transport.stop(); }
void FilePlayer::stop()
{
    transport.stop();
    transport.setPosition (0.0);
}

void FilePlayer::setLooping (bool b)
{
    looping.store (b, std::memory_order_relaxed);
    const juce::ScopedLock sl (sourceLock);
    if (readerSource != nullptr)
        readerSource->setLooping (b);
}

void FilePlayer::setPositionSeconds (double s) { transport.setPosition (juce::jmax (0.0, s)); }

bool   FilePlayer::isPlaying() const           { return transport.isPlaying(); }
double FilePlayer::getCurrentPositionSeconds() const { return transport.getCurrentPosition(); }

juce::String FilePlayer::getCurrentFileName() const
{
    const juce::ScopedLock sl (sourceLock);
    return currentFileName;
}

void FilePlayer::getNextAudioBlock (juce::AudioBuffer<float>& buffer)
{
    juce::AudioSourceChannelInfo info (buffer);

    if (! hasLoaded.load (std::memory_order_relaxed))
    {
        info.clearActiveBufferRegion();
        return;
    }

    transport.getNextAudioBlock (info);
}
