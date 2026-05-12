# UG6 Broadcaster

Audio broadcast processor + Icecast/Shoutcast encoder, style Omnia / Stereo Tool, piloté par une UI web locale (`http://127.0.0.1:8000`).

Remplace Oddcast pour la diffusion de 10 webradios (un process par radio, port 8000–8009), avec une chaîne DSP 6 bandes complète avant l'encodage et le push vers Icecast.

## État du projet

**Commit 1 — Hello World** ✅
Sinus 1 kHz, ON AIR, slider gain, VU-mètres temps réel via SSE. Valide le chaînage Audio + HTTP + UI.

**Phase A — DSP chain online** ✅
- Capture audio réelle : sélection device d'entrée + sortie depuis l'UI (CoreAudio/WASAPI).
- Mode source : générateur de test 1 kHz OU entrée audio live.
- Chaîne DSP complète :
  - HPF 30 Hz (rumble filter)
  - AGC auto-leveler (compresseur lent + makeup)
  - EQ 4 bandes (low-shelf, 2× peaking, high-shelf) ajustable
  - Crossover Linkwitz-Riley 6 bandes (parallèle HPF+LPF, 5 fréquences ajustables)
  - Compresseur indépendant par bande avec **GR meters** (threshold, ratio, attack, release, makeup)
  - Brick-wall limiter master (ceiling, release)
- UI broadcast pro dense : ON AIR pulsant, bypass, master gains, EQ 4 bandes, 6 colonnes compresseurs, limiter, presets, gros VU stéréo IN/OUT
- 4 presets hardcodés : `bypass`, `music_fm`, `talk`, `urban_pop`
- API JSON générique : `POST /api/param` avec `{"path":"comp.3.ratio","value":4.0}`

**Phase B — Encodage + Push Icecast** (à venir)
LAME (MP3), fdk-aac (AAC), libopus (Opus), libshout (Icecast 2 / Shoutcast v1+v2), multi-mount, auto-reconnect avec backoff.

**Phase C — Polish + packaging** (à venir)
Presets JSON save/load, spectre FFT 2048 pts bark, LUFS-M, de-esser sidechain, stereo enhancer M/S, soft clipper, installeur Inno Setup Win, .dmg notarized Mac, auto-start utilisateur.

## Stack

| Couche | Choix | Justification |
|---|---|---|
| Framework C++ | **JUCE 7.0.12** | Dernière 7.x compatible Win7 SP1. Code écrit de façon à migrer facilement vers JUCE 8 quand la studio passe sur Win10/11. |
| Build | **CMake 3.22+** | Supporté nativement par JUCE 7+, indispensable pour le CI GitHub Actions cross-platform. Plus moderne que Projucer. |
| HTTP / WS | **cpp-httplib v0.15.3** | Single-header, supporte les Server-Sent Events (utilisés pour pousser les VU à 20 Hz). |
| UI | **Tailwind CSS + Alpine.js** (via CDN pour le dev) | Embarquée dans le binaire via `juce_add_binary_data`. |
| Compilo Win | **MSVC v141 (VS2017 toolset)** | Dernier à produire des binaires Win7 SP1 par défaut, avec `_WIN32_WINNT=0x0601`. |
| Compilo Mac | **Clang/Xcode** | Cible deployment 10.13, universal binary x86_64 + arm64. |

## Build local (macOS)

```bash
# Pré-requis : Xcode CLT + CMake 3.22+
brew install cmake

# Configure (Xcode generator, universal binary)
cmake -B build -G Xcode \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13 \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"

# Premier configure : télécharge JUCE (~500 Mo) + cpp-httplib via FetchContent.
# Build (Release)
cmake --build build --config Release --parallel

# Run
./build/UG6Broadcaster_artefacts/Release/UG6Broadcaster
# → http://127.0.0.1:8000 dans le navigateur
```

Options CLI :

```bash
./UG6Broadcaster --port=8001    # une instance par radio plus tard
```

## Build local (Windows)

```powershell
# Pré-requis : Visual Studio 2017 ou 2019/2022 avec workload "Desktop C++"
#            + v141 toolset installé pour cibler Win7

cmake -B build -G "Visual Studio 16 2019" -A x64 -T v141
cmake --build build --config Release --parallel

# Run
.\build\UG6Broadcaster_artefacts\Release\UG6Broadcaster.exe
```

## Build CI

`.github/workflows/build.yml` produit deux artefacts à chaque push sur `main` :

- `UG6Broadcaster-macOS` — universal binary
- `UG6Broadcaster-Windows` — exe v141 (compatible Win7 SP1)

Récupère-les depuis l'onglet *Actions* de GitHub.

## Architecture

```
src/Main.cpp         entry point, parse --port, signal handlers, message loop
src/AudioEngine.*    juce::AudioDeviceManager + sinus 1 kHz, params atomiques
src/HttpServer.*     cpp-httplib server thread, routes JSON, SSE meters
ui/index.html        UI Tailwind + Alpine, embarquée par juce_add_binary_data
```

Le pipeline audio tourne sur le thread CoreAudio/WASAPI (priorité système). L'HTTP server tourne sur son propre thread. Communication par `std::atomic<float>` / `std::atomic<bool>`. Aucun lock dans la callback audio.

## API HTTP (commit 1)

| Méthode | Route | Body | Retour |
|---|---|---|---|
| GET | `/` | — | HTML UI |
| GET | `/api/state` | — | `{onAir, gainDb, freqHz, peak, rms, device, sampleRate}` |
| GET | `/api/devices` | — | `{current, devices: [...]}` |
| POST | `/api/onair` | `"true"` / `"false"` | state JSON |
| POST | `/api/gain` | `"-12.0"` (dB) | state JSON |
| POST | `/api/freq` | `"1000"` (Hz) | state JSON |
| GET | `/api/meters` | — | `text/event-stream`, push 20 Hz |

Le serveur n'écoute que sur `127.0.0.1` — jamais exposé sur le LAN.

## Multi-instance (cible 10 radios)

Une instance par radio, lancée avec son propre port :

```bash
./UG6Broadcaster --port=8000 &
./UG6Broadcaster --port=8001 &
# ...
./UG6Broadcaster --port=8009 &
```

Si un process crash, les 9 autres tournent. Modèle identique à ton workflow Oddcast actuel.

## Licences à clarifier avant distribution

- **JUCE 7** : licence personnelle gratuite (avec splash + analytics), commerciale payante ($800/an indie). On a désactivé splash + analytics (`JUCE_DISPLAY_SPLASH_SCREEN=0`, `JUCE_REPORT_APP_USAGE=0`) → vérifier que c'est compatible avec l'usage personnel/perso-pro.
- **libfdk-aac** (pour le commit AAC à venir) : licence Fraunhofer, redistribution binaire ambiguë. Pour usage perso pas de souci. Pour distribution, prévoir un fallback `libfaac` ou laisser l'utilisateur builder.
- **LAME** : LGPL — OK si on link en dynamique ou si on documente comment relinker.
- **libopus** : BSD — sans souci.

## Roadmap

- [x] Commit 1 — Hello world : audio + HTTP + UI + CI
- [ ] Commit 2 — capture audio réelle, sélection device d'entrée (UI)
- [ ] Commit 3 — HPF 30 Hz + AGC 2-stage
- [ ] Commit 4 — EQ paramétrique 4 bandes
- [ ] Commit 5 — Crossover Linkwitz-Riley 6 bandes
- [ ] Commit 6 — Compresseurs multibandes + GR meters
- [ ] Commit 7 — De-esser + stereo enhancer
- [ ] Commit 8 — Brick-wall limiter look-ahead + soft clipper
- [ ] Commit 9 — Encoders MP3 (LAME) + AAC (fdk) + Opus
- [ ] Commit 10 — Push Icecast/Shoutcast via libshout + auto-reconnect
- [ ] Commit 11 — Système de presets JSON + import/export
- [ ] Commit 12 — Spectre FFT 2048 pts bark scale + LUFS-M
- [ ] Commit 13 — Packaging : .app + .dmg notarized (Mac) / Inno Setup (Win)
- [ ] Commit 14 — Auto-start utilisateur (LaunchAgent Mac, registry Run Win)
