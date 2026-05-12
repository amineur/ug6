#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

/**
 *  Internal audio file player.
 *
 *  Lets the user drag-and-drop an audio file (MP3 / WAV / AIFF / FLAC / Ogg)
 *  into the UI to feed the DSP chain directly — bypassing the whole live audio
 *  input plumbing (no BlackHole, no virtual cables, no macOS mic permission).
 *
 *  Threading: the transport source has its own read-ahead thread; the audio
 *  callback only calls getNextAudioBlock(), which is internally thread-safe.
 *  File swapping (loadFile) happens on the HTTP thread under a CriticalSection
 *  that the audio thread never touches.
 */
class FilePlayer
{
public:
    FilePlayer();
    ~FilePlayer();

    void prepare (double sampleRate, int blockSize);
    void releaseResources();

    /** Returns empty String on success, error message otherwise. */
    juce::String loadFile (const juce::File& file);

    void play();
    void pause();
    void stop();
    void setLooping (bool b);
    void setPositionSeconds (double s);

    /** Pulls the next block of file audio into `buffer`. Buffer is cleared if no file
        is loaded or playback is not active. */
    void getNextAudioBlock (juce::AudioBuffer<float>& buffer);

    bool         isPlaying() const;
    bool         isLooping()  const noexcept { return looping.load (std::memory_order_relaxed); }
    bool         hasFile()    const noexcept { return hasLoaded.load (std::memory_order_relaxed); }
    double       getCurrentPositionSeconds() const;
    double       getDurationSeconds()        const noexcept { return duration.load (std::memory_order_relaxed); }
    juce::String getCurrentFileName()        const;

private:
    juce::AudioFormatManager  formatManager;
    juce::TimeSliceThread     readAheadThread;
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    juce::CriticalSection sourceLock;
    juce::String currentFileName;

    std::atomic<bool>   looping   { true };
    std::atomic<bool>   hasLoaded { false };
    std::atomic<double> duration  { 0.0 };
};
