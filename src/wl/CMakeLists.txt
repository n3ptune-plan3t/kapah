# Wayland glue. ADR-002: the layer-shell, session-lock, idle-notify, data-control
# and gamma-control protocols are generated in-tree from vendored XML, using
# Qt's bundled wayland-scanner (qt6-wayland-scanner, part of qt6-wayland-dev).
#
# Nothing here links Qt Widgets; the module is Gui-only so kapah-lock can use the
# lock protocol without dragging in the shell.

# ---------------------------------------------------------------------------
# Protocol generation
# ---------------------------------------------------------------------------

set(KAPAH_PROTOCOL_FILES
    "${KAPAH_PROTOCOL_XML_DIR}/wlr-layer-shell-unstable-v1.xml"
    "${KAPAH_PROTOCOL_XML_DIR}/ext-session-lock-v1.xml"
    "${KAPAH_PROTOCOL_XML_DIR}/ext-idle-notify-v1.xml"
    "${KAPAH_PROTOCOL_XML_DIR}/wlr-output-power-management-unstable-v1.xml"
)

if(KAPAH_WITH_CLIPBOARD)
    list(APPEND KAPAH_PROTOCOL_FILES
        "${KAPAH_PROTOCOL_XML_DIR}/ext-data-control-v1.xml")
endif()

if(KAPAH_WITH_NIGHTLIGHT)
    list(APPEND KAPAH_PROTOCOL_FILES
        "${KAPAH_PROTOCOL_XML_DIR}/wlr-gamma-control-unstable-v1.xml")
endif()

foreach(_xml ${KAPAH_PROTOCOL_FILES})
    if(NOT EXISTS "${_xml}")
        message(FATAL_ERROR
            "vendored Wayland protocol XML missing: ${_xml}\n"
            "Regenerate with tools/update-protocols.sh if you are re-syncing them.")
    endif()
endforeach()

add_library(kapah_wl_protos STATIC
    # The generated sources are produced at build time; listing the XML keeps
    # CMake aware of the dependency.
    ${KAPAH_PROTOCOL_FILES}
)

target_include_directories(kapah_wl_protos
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
)

# Produces <basename>_client_protocol.{h,cpp} for each input, in this dir.
qt_generate_wayland_protocol_sources(
    TARGET kapah_wl_protos
    FILES ${KAPAH_PROTOCOL_FILES}
)

target_link_libraries(kapah_wl_protos
    PUBLIC
        Qt6::Gui
        Qt6::WaylandClient
    PRIVATE
        Qt6::WaylandClientPrivate
        kapah::warnings
        kapah::optimize
)

set_target_properties(kapah_wl_protos PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)

# ---------------------------------------------------------------------------
# The shell-facing wrapper
# ---------------------------------------------------------------------------

set(KAPAH_WL_SOURCES
    globals.cpp
    globals.h
    layersurface.cpp
    layersurface.h
    qtcompat.h
    idlenotify.cpp
    idlenotify.h
)

add_library(kapah_wl STATIC ${KAPAH_WL_SOURCES})

add_library(kapah::wl ALIAS kapah_wl)

target_include_directories(kapah_wl
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(kapah_wl
    PUBLIC
        kapah_wl_protos
        kapah::core
    PRIVATE
        Qt6::Gui
        Qt6::WaylandClientPrivate
        kapah::warnings
        kapah::optimize
        kapah::gc-sections
)

set_target_properties(kapah_wl PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)

# ---------------------------------------------------------------------------
# Optional protocol wrappers, each behind its own build flag
# ---------------------------------------------------------------------------

if(KAPAH_WITH_CLIPBOARD)
    target_sources(kapah_wl PRIVATE datactrl.cpp datactrl.h)
    target_compile_definitions(kapah_wl PUBLIC KAPAH_WITH_CLIPBOARD=1)
endif()

if(KAPAH_WITH_NIGHTLIGHT)
    target_sources(kapah_wl PRIVATE gammacontrol.cpp gammacontrol.h)
    target_compile_definitions(kapah_wl PUBLIC KAPAH_WITH_NIGHTLIGHT=1)
endif()

# The lock screen lives in its own process but shares these wrappers.
add_library(kapah_wl_lock STATIC
    sessionlock.cpp
    sessionlock.h
)
add_library(kapah::wl-lock ALIAS kapah_wl_lock)
target_include_directories(kapah_wl_lock PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(kapah_wl_lock
    PUBLIC kapah_wl_protos kapah::core
    PRIVATE Qt6::Gui Qt6::WaylandClientPrivate kapah::warnings kapah::optimize
)
set_target_properties(kapah_wl_lock PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC ON)