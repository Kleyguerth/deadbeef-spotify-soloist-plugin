cmake_minimum_required(VERSION 3.20)
project(deadbeef-spotify-soloist-plugin
    VERSION 0.1.0
    DESCRIPTION "DeaDBeeF plugin bridging Spotify Web API search, XSPF playlists, and Spotify Soloist"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_DEADBEEF_PLUGIN "Build DeaDBeeF plugin modules (requires GTK3)" ON)
option(BUILD_TESTS "Build standalone unit tests" ON)

find_package(nlohmann_json 3.9 REQUIRED)
if(NOT WIN32)
    find_package(pugixml REQUIRED)
endif()

# -----------------------------------------------------------------------------
# Core library (pure C++17: URI helpers, Soloist JSON protocol, MetadataCache, XSPF)
# -----------------------------------------------------------------------------
set(SPOTIFY_CORE_SOURCES
    src/core/track_metadata.cpp
    src/core/soloist_protocol.cpp
    src/core/metadata_cache.cpp
    src/core/xspf_codec.cpp
)
if(WIN32)
    list(APPEND SPOTIFY_CORE_SOURCES third_party/pugixml/pugixml.cpp)
endif()

add_library(spotify_core STATIC ${SPOTIFY_CORE_SOURCES})

target_include_directories(spotify_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32)
    target_include_directories(spotify_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml
    )
    target_link_libraries(spotify_core PUBLIC nlohmann_json::nlohmann_json)
else()
    target_link_libraries(spotify_core PUBLIC
        nlohmann_json::nlohmann_json
        pugixml::pugixml
    )
endif()

set_target_properties(spotify_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# -----------------------------------------------------------------------------
# DeaDBeeF Plugin Targets
# -----------------------------------------------------------------------------
if(BUILD_DEADBEEF_PLUGIN)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    if(NOT WIN32)
        find_package(CURL REQUIRED)
    endif()

    find_path(DEADBEEF_INCLUDE_DIR deadbeef/deadbeef.h
        PATHS /usr/include /usr/local/include
    )
    if(NOT DEADBEEF_INCLUDE_DIR)
        set(DEADBEEF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    endif()

    include(GNUInstallDirs)
    set(DEADBEEF_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/deadbeef")

    # 1. Decoder Plugin (handles spotify://track/... playback via Soloist)
    add_library(ddb_spotify_soloist MODULE
        src/plugin/ddb_spotify_soloist.cpp
        src/core/simple_ws_client.cpp
        src/core/soloist_client.cpp
    )
    set_target_properties(ddb_spotify_soloist PROPERTIES PREFIX "")
    target_include_directories(ddb_spotify_soloist PRIVATE ${DEADBEEF_INCLUDE_DIR})
    target_link_libraries(ddb_spotify_soloist PRIVATE spotify_core)
    if(WIN32)
        target_link_libraries(ddb_spotify_soloist PRIVATE ws2_32)
        target_link_options(ddb_spotify_soloist PRIVATE -static-libgcc -static-libstdc++)
    endif()
    install(TARGETS ddb_spotify_soloist
        LIBRARY DESTINATION "${DEADBEEF_PLUGIN_DIR}"
        RUNTIME DESTINATION "${DEADBEEF_PLUGIN_DIR}"
    )

    # 2. Playlist Plugin (XSPF load & save with spotify%3Atrack%3A...)
    add_library(ddb_spotify_xspf MODULE
        src/plugin/ddb_spotify_xspf.cpp
    )
    set_target_properties(ddb_spotify_xspf PROPERTIES PREFIX "")
    target_include_directories(ddb_spotify_xspf PRIVATE ${DEADBEEF_INCLUDE_DIR})
    target_link_libraries(ddb_spotify_xspf PRIVATE spotify_core)
    if(WIN32)
        target_link_options(ddb_spotify_xspf PRIVATE -static-libgcc -static-libstdc++)
    endif()
    install(TARGETS ddb_spotify_xspf
        LIBRARY DESTINATION "${DEADBEEF_PLUGIN_DIR}"
        RUNTIME DESTINATION "${DEADBEEF_PLUGIN_DIR}"
    )

    # 3. GTK3 Search & Discography UI Plugin
    add_library(ddb_spotify_gtkui MODULE
        src/plugin/ddb_spotify_gtkui.cpp
        src/core/spotify_api_client.cpp
    )
    set_target_properties(ddb_spotify_gtkui PROPERTIES PREFIX "")
    target_include_directories(ddb_spotify_gtkui PRIVATE ${DEADBEEF_INCLUDE_DIR})
    target_link_libraries(ddb_spotify_gtkui PRIVATE
        spotify_core
        PkgConfig::GTK3
    )
    if(WIN32)
        target_link_libraries(ddb_spotify_gtkui PRIVATE winhttp)
        target_link_options(ddb_spotify_gtkui PRIVATE -static-libgcc -static-libstdc++)
    else()
        target_link_libraries(ddb_spotify_gtkui PRIVATE CURL::libcurl)
    endif()
    install(TARGETS ddb_spotify_gtkui
        LIBRARY DESTINATION "${DEADBEEF_PLUGIN_DIR}"
        RUNTIME DESTINATION "${DEADBEEF_PLUGIN_DIR}"
    )
endif()

# -----------------------------------------------------------------------------
# Unit Tests
# -----------------------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_spotify_core
        tests/test_main.cpp
        tests/test_xspf_codec.cpp
        tests/test_soloist_protocol.cpp
    )
    target_link_libraries(test_spotify_core PRIVATE spotify_core)
    target_compile_definitions(test_spotify_core PRIVATE
        EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
    )
    add_test(NAME test_spotify_core COMMAND test_spotify_core)
endif()
