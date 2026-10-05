cmake_minimum_required(VERSION 3.25)

# Shell name. Everything user-visible derives from this: binary names, socket
# path, D-Bus names, config directory. See docs/decisions/ADR-000-shell-name.md.
set(SHELL_NAME "kapah" CACHE STRING "Shell name (user-visible identity of the shell)")

project(${SHELL_NAME}
    VERSION 0.1.0
    DESCRIPTION "Low-resource Qt6 Widgets shell for the niri Wayland compositor"
    HOMEPAGE_URL "https://github.com/kapah/kapah"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single build type unless the packager picks one explicitly.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(KAPAH_RELEASE_SMALL "Optimise for binary size (-Os) instead of speed (-O2)" OFF)
option(KAPAH_ENABLE_LTO "Link time optimization" ON)
option(KAPAH_ENABLE_ASAN "AddressSanitizer + UBSan" OFF)
option(KAPAH_BUILD_GALLERY "Build the shell-gallery primitive widget gallery" ON)
option(KAPAH_WITH_TRAY "Build the StatusNotifierItem system tray" ON)
option(KAPAH_WITH_BLUETOOTH "Build the BlueZ quick-settings module" OFF)
option(KAPAH_WITH_MPRIS "Build the MPRIS bar module" OFF)
option(KAPAH_WITH_CLIPBOARD "Build the ext-data-control clipboard history" OFF)
option(KAPAH_WITH_NIGHTLIGHT "Build the zwlr-gamma-control night light" OFF)
option(KAPAH_WITH_SETTINGS "Build the standalone config editor (kapah-settings)" ON)
option(KAPAH_WITH_PAM "Enable PAM authentication in kapah-lock" ON)
option(KAPAH_STRIP_RELEASE "Strip release binaries" ON)
option(KAPAH_INSTALL "Generate install rules" ON)

include(cmake/CompilerWarnings.cmake)
include(cmake/Optimize.cmake)
include(cmake/GcSections.cmake)
include(cmake/Sanitizers.cmake)

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets DBus WaylandClient)
qt_standard_project_setup()

# qt_generate_wayland_protocol_sources() lives in Qt6WaylandClientTools.
find_package(Qt6 QUIET COMPONENTS WaylandClientTools)

# WaylandClientPrivate provides the QWayland* private client headers that the
# layer-shell glue in src/wl/ is built on (ADR-002).
find_package(Qt6 QUIET COMPONENTS WaylandClientPrivate)

if(NOT Qt6WaylandClientPrivate_FOUND)
    message(FATAL_ERROR
        "Qt6::WaylandClientPrivate not found. kapah implements zwlr_layer_shell_v1 in-tree "
        "(ADR-002) and therefore needs the Qt private wayland client headers.")
endif()

# ---------------------------------------------------------------------------
# Optional system dependencies
# ---------------------------------------------------------------------------

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBPULSE IMPORTED_TARGET libpulse)
pkg_check_modules(LIBUDEV IMPORTED_TARGET libudev)

if(NOT LIBPULSE_FOUND)
    message(FATAL_ERROR "libpulse not found. kapah requires the PulseAudio client library (ADR-003).")
endif()

if(NOT LIBUDEV_FOUND)
    message(FATAL_ERROR "libudev not found. kapah requires libudev for battery/backlight hotplug.")
endif()

if(KAPAH_WITH_PAM)
    pkg_check_modules(PAM IMPORTED_TARGET libpam)
    if(NOT PAM_FOUND)
        message(WARNING "libpam not found: kapah-lock will be built without PAM support.")
        set(KAPAH_WITH_PAM OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------

set(KAPAH_PROTOCOL_XML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/wl/protocols")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/kapah/version.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

add_subdirectory(src/core)
add_subdirectory(src/wl)
add_subdirectory(src/niri)
add_subdirectory(src/services)
add_subdirectory(src/ui)
# Components/app/lock/ctl landed lazily: only enter a subdir once it has a
# CMakeLists.txt, so the tree configures at every stage of the roadmap.
foreach(_subdir components app lock ctl)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_subdir}/CMakeLists.txt")
        add_subdirectory(src/${_subdir})
    endif()
endforeach()

if(KAPAH_BUILD_GALLERY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gallery/CMakeLists.txt")
    add_subdirectory(src/gallery)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

if(KAPAH_INSTALL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/CMakeLists.txt")
    include(GNUInstallDirs)
    add_subdirectory(contrib)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "  ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  ------------------------------------------------")
message(STATUS "  build type ............ ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt .................... ${Qt6_VERSION}")
message(STATUS "  layer-shell bindings .. in-tree (qtwaylandscanner), see ADR-002")
message(STATUS "  audio ................. libpulse ${LIBPULSE_VERSION}, see ADR-003")
message(STATUS "  lock auth ............. ${KAPAH_WITH_PAM}")
message(STATUS "  tray / bluetooth ...... ${KAPAH_WITH_TRAY} / ${KAPAH_WITH_BLUETOOTH}")
message(STATUS "  mpris / clipboard ..... ${KAPAH_WITH_MPRIS} / ${KAPAH_WITH_CLIPBOARD}")
message(STATUS "  night light ........... ${KAPAH_WITH_NIGHTLIGHT}")
message(STATUS "  settings binary ....... ${KAPAH_WITH_SETTINGS}")
message(STATUS "")