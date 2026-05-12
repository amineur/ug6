// UG6 Broadcaster — entry point.
//
// Commit #1 ("Hello World") wires together:
//   - JUCE audio device manager (CoreAudio on macOS, WASAPI on Windows)
//   - 1 kHz sine generator with atomic gain + ON AIR gate
//   - cpp-httplib HTTP server on 127.0.0.1:<port> with SSE meters
//   - Embedded HTML/Tailwind/Alpine.js UI
//
// CLI: UG6Broadcaster [--port=8000]
//
// The 10-instance multi-radio model is per-process: launch one binary per
// radio with --port=8000..8009, each with its own config file (later commit).

#include "AudioEngine.h"
#include "HttpServer.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

namespace
{
    std::atomic<bool> shouldQuit { false };

    void signalHandler (int) { shouldQuit.store (true); }

    int parsePort (int argc, char** argv, int defaultPort)
    {
        for (int i = 1; i < argc; ++i)
        {
            const juce::String arg (argv[i]);
            if (arg.startsWith ("--port="))
                return arg.fromFirstOccurrenceOf ("=", false, false).getIntValue();
            if (arg == "--port" && i + 1 < argc)
                return juce::String (argv[i + 1]).getIntValue();
        }
        return defaultPort;
    }
}

int main (int argc, char** argv)
{
    const int port = parsePort (argc, argv, 8000);

    // Initialises the JUCE message manager (required for CoreAudio / WASAPI
    // device-change notifications and for any async juce::Timer).
    juce::ScopedJuceInitialiser_GUI juceInit;

    AudioEngine engine;
    engine.initialise();

    HttpServer server (engine, port);
    server.start();

    std::cout << "UG6 Broadcaster started\n"
              << "  UI:     http://127.0.0.1:" << port << "\n"
              << "  Device: " << engine.getCurrentDeviceName() << "\n"
              << "  SR:     " << engine.getCurrentSampleRate() << " Hz\n"
              << "Press Ctrl+C to quit.\n" << std::flush;

    std::signal (SIGINT,  signalHandler);
    std::signal (SIGTERM, signalHandler);

    // Pump the message manager so JUCE async callbacks fire while we wait for quit.
    auto* mm = juce::MessageManager::getInstance();
    while (! shouldQuit.load())
        mm->runDispatchLoopUntil (100);

    std::cout << "Shutting down...\n";
    server.stop();
    engine.shutdown();
    return 0;
}
