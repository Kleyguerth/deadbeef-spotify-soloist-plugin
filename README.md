# DeaDBeeF Spotify Soloist Plugin

A DeaDBeeF plugin that bridges **Spotify Web API** search, **XSPF** playlist save/restore, and **Spotify Soloist** audio playback via local WebSocket (`ws://127.0.0.1:9090`).

## Architecture

All 3 modules live cleanly in DeaDBeeF's `plugins/` folder with zero core-folder pollution:
- `ddb_spotify_soloist` (`DB_decoder_t`): Plays `spotify://track/...` URIs via Soloist WebSocket (`SimpleWsClient` + `SoloistClient`), auto-spawning `soloist` if not already running.
- `ddb_spotify_xspf` (`DB_playlist_t`): Loads and saves `.xspf` playlists using canonical `spotify%3Atrack%3A...` locations.
- `ddb_spotify_gtkui` (`DB_misc_t` + GTK3): Adds **View → Spotify Search...** and a Design Mode widget (`Spotify Search & Discography`) with Song Search and Artist Discography tabs.

## Quick Start (Windows)

1. Download `deadbeef-spotify-soloist-win64.zip` from [Releases](https://github.com/Kleyguerth/deadbeef-spotify-soloist-plugin/releases).
2. Extract the `plugins/` folder into your DeaDBeeF directory (or copy the `.dll` files into `%APPDATA%\deadbeef\plugins\`).
3. In DeaDBeeF **Edit → Preferences → Plugins**:
   - **Spotify Search UI (GTK3)**: enter your Spotify Web API Client ID and Client Secret.
   - **Spotify Soloist Playback**: set Soloist Executable Path (or place `soloist.exe` on `PATH`) and Soloist API Key.
4. Open **View → Spotify Search...** (or add the widget in Design Mode) to search and play.

## Building from Source

### Linux (openSUSE / Fedora / Arch / Debian)
Dependencies: `cmake`, `ninja`, `gcc-c++`, `gtk3-devel`, `libcurl-devel`, `nlohmann_json-devel`, `pugixml-devel`, `deadbeef-devel`.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

### Windows (MSYS2 UCRT64)
```bash
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,nlohmann-json,pugixml,gtk3}
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```
