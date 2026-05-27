cmake_minimum_required(VERSION 3.2)

# Bridges needed by Lego Star Wars: TCS (Android libTTapp.so).
# - openal: replaces the Vita OpenSL ES path; bridges audio output
# - zip:    APK access (if loading libTTapp.so straight from the .apk)
# The game does not link freetype on Android, so we skip it.
set(PORT_BRIDGE_SOURCES
    "bridges/openal_bridge.c"
    "bridges/zip_bridge.c"
)
