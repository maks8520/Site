#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fetch weather forecast and write to SD card cache using cyclic caching
bool fetch_and_cache_weather(double latitude, double longitude);

// Fetch wind profile from SondeHub and write to SD card
bool fetch_and_cache_wind_profile(double latitude, double longitude);

#ifdef __cplusplus
}
#endif
