#pragma once

#include "InstanceConfig.h"
#include "Presets.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

class AudioEngine;
namespace httplib { class Server; }

/**
 *  Phase A HTTP / SSE control plane.
 *
 *  Listens on 127.0.0.1:<port>. Endpoints:
 *
 *    GET  /                    → embedded HTML UI
 *    GET  /api/state           → big JSON snapshot (master + dsp + meters + devices)
 *    GET  /api/devices         → input/output device lists
 *    POST /api/onair           → body "true"|"false"
 *    POST /api/source          → body "generator"|"input"
 *    POST /api/gen/freq        → body Hz
 *    POST /api/gen/gain        → body dB
 *    POST /api/device/in       → body device name (empty = default)
 *    POST /api/device/out      → body device name
 *    POST /api/param           → JSON {"path":"comp.3.ratio","value":4.0}
 *    POST /api/preset          → body preset name (bypass|music_fm|talk|urban_pop)
 *    GET  /api/meters          → SSE stream of state JSON @ ~20 Hz
 */
class HttpServer
{
public:
    HttpServer (AudioEngine& engine, int port);
    ~HttpServer();

    void start();
    void stop();

    int getPort() const noexcept { return port; }

private:
    void setupRoutes();
    std::string buildStateJson() const;
    std::string buildDevicesJson() const;

    AudioEngine& engine;
    int port;
    PresetsStore presets;
    InstanceConfig instanceConfig;

    std::unique_ptr<httplib::Server> server;
    std::thread listenerThread;
    std::atomic<bool> running { false };
};
