cmake_minimum_required(VERSION 3.16)

# ── Define the core library (platform-neutral sources) ───────────────────────
add_library(anyar_core STATIC
    src/app.cpp
    src/ipc_router.cpp
    src/command_registry.cpp
    src/event_bus.cpp
    src/window.cpp
    src/plugins/fs_plugin.cpp
    src/plugins/db_plugin.cpp
)

# ── Platform-specific sources ────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(anyar_core PRIVATE
        src/main_thread_linux.cpp
        src/plugins/dialog_linux.cpp
        src/plugins/clipboard_linux.cpp
        src/plugins/shell_linux.cpp
    )
elseif(WIN32)
    # Phase 7: add Windows implementations
    # target_sources(anyar_core PRIVATE
    #     src/main_thread_win32.cpp
    #     src/plugins/dialog_win32.cpp
    #     src/plugins/clipboard_win32.cpp
    #     src/plugins/shell_win32.cpp
    # )
elseif(APPLE)
    # Phase 7: add macOS implementations
    # target_sources(anyar_core PRIVATE
    #     src/main_thread_macos.mm
    #     src/plugins/dialog_macos.mm
    #     src/plugins/clipboard_macos.mm
    #     src/plugins/shell_macos.mm
    # )
endif()

target_include_directories(anyar_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(anyar_core PUBLIC cxx_std_17)

# ── LibAsyik ─────────────────────────────────────────────────────────────────
SET(LIBASYIK_ENABLE_SOCI ${ANYAR_ENABLE_SOCI})

if(LIBASYIK_SOURCE_DIR AND EXISTS "${LIBASYIK_SOURCE_DIR}/CMakeLists.txt")
    # Development mode: use libasyik from its source/build directory
    message(STATUS "Using LibAsyik from source: ${LIBASYIK_SOURCE_DIR}")

    # Assume it's already built — link the static lib and includes
    set(_LIBASYIK_LIB "${LIBASYIK_SOURCE_DIR}/build/src/liblibasyik.a")
    if(NOT EXISTS "${_LIBASYIK_LIB}")
        message(FATAL_ERROR "LibAsyik library not found at ${_LIBASYIK_LIB}. "
            "Build libasyik first: cd ${LIBASYIK_SOURCE_DIR}/build && cmake .. && make")
    endif()

    target_include_directories(anyar_core PUBLIC
        "${LIBASYIK_SOURCE_DIR}/include"
        "${LIBASYIK_SOURCE_DIR}/external/aixlog/include"
        "${LIBASYIK_SOURCE_DIR}/external/cppcodec"
    )
    target_link_libraries(anyar_core PUBLIC "${_LIBASYIK_LIB}")
else()
    # Installed mode: use find_package
    find_package(libasyik REQUIRED)
    if(libasyik_FOUND)
        target_include_directories(anyar_core PUBLIC ${libasyik_INCLUDE_DIR})
        target_link_libraries(anyar_core PUBLIC libasyik)
    endif()
endif()

# ── Boost (required by LibAsyik) ────────────────────────────────────────────
find_package(Boost COMPONENTS context fiber date_time url REQUIRED)
if(Boost_FOUND)
    target_include_directories(anyar_core PUBLIC ${Boost_INCLUDE_DIR})
    target_link_libraries(anyar_core PUBLIC Boost::fiber Boost::context Boost::date_time Boost::url)
endif()

# ── SOCI ─────────────────────────────────────────────────────────────────────
# LibAsyik's installed cmake target links SOCI unconditionally, so we must
# always find SOCI to satisfy imported target references.
find_package(SOCI QUIET)
if(SOCI_FOUND)
    target_include_directories(anyar_core PUBLIC /usr/include/postgresql)
    target_include_directories(anyar_core PUBLIC /usr/local/include/soci)
    target_include_directories(anyar_core PUBLIC /usr/local/include/soci/postgresql)
    target_include_directories(anyar_core PUBLIC /usr/local/include/soci/sqlite3)
    if(ANYAR_ENABLE_SOCI)
        target_link_libraries(anyar_core PUBLIC SOCI::soci_core SOCI::soci_postgresql SOCI::soci_sqlite3)
    endif()
endif()

# ── OpenSSL ──────────────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
target_link_libraries(anyar_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# ── Threads ──────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(anyar_core PUBLIC Threads::Threads)

# ── nlohmann/json (header-only) ─────────────────────────────────────────────
# Look for system-installed nlohmann_json, or use the header directly
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS /usr/local/include /usr/include
)
if(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(anyar_core PUBLIC ${NLOHMANN_JSON_INCLUDE_DIR})
else()
    message(FATAL_ERROR "nlohmann/json.hpp not found. Install via setup script or place in /usr/local/include/nlohmann/")
endif()

# ── WebView (webview/webview) ────────────────────────────────────────────────
# webview/webview — multi-file header-only library in third_party
target_include_directories(anyar_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Find webkit2gtk and GTK3 for Linux webview support
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT2GTK REQUIRED webkit2gtk-4.0)
    target_include_directories(anyar_core PUBLIC ${WEBKIT2GTK_INCLUDE_DIRS})
    target_link_libraries(anyar_core PUBLIC ${WEBKIT2GTK_LIBRARIES})

    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    target_include_directories(anyar_core PUBLIC ${GTK3_INCLUDE_DIRS})
    target_link_libraries(anyar_core PUBLIC ${GTK3_LIBRARIES})
elseif(WIN32)
    # Phase 7: WebView2 — webview/webview auto-detects via macros.h
    # find_package(WebView2 REQUIRED)
elseif(APPLE)
    # Phase 7: WKWebView — webview/webview auto-detects via macros.h
    # target_link_libraries(anyar_core PUBLIC "-framework WebKit")
endif()
