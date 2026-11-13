#pragma once

#if defined(_WIN32) && defined(SPOTIFY_BRIDGE_SHARED)
  #ifdef SPOTIFY_BRIDGE_EXPORTS
    #define SPOTIFY_BRIDGE_API __declspec(dllexport)
  #else
    #define SPOTIFY_BRIDGE_API __declspec(dllimport)
  #endif
#elif defined(SPOTIFY_BRIDGE_SHARED)
  #define SPOTIFY_BRIDGE_API __attribute__((visibility("default")))
#else
  #define SPOTIFY_BRIDGE_API
#endif
