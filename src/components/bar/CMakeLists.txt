# The bar (Phase 3): one layer-shell surface per output, one widget per
# module. Qt Widgets only; no QML (ADR / roadmap section 1).

set(KAPAH_BAR_SOURCES
    bar.cpp
    bar.h
    modules.cpp
    modules.h
    workspaceitem.cpp
    workspaceitem.h
    windowtitleitem.cpp
    windowtitleitem.h
    clockitem.cpp
    clockitem.h
    batteryitem.cpp
    batteryitem.h
    networkitem.cpp
    networkitem.h
    audioitem.cpp
    audioitem.h
    backlightitem.cpp
    backlightitem.h
    trayitem.cpp
    trayitem.h
    layoutitem.cpp
    layoutitem.h
    sessionitem.cpp
    sessionitem.h
)

add_library(kapah_bar STATIC ${KAPAH_BAR_SOURCES})

add_library(kapah::bar ALIAS kapah_bar)

target_include_directories(kapah_bar PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(kapah_bar
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    PRIVATE
        kapah::core
        kapah::wl
        kapah::niri
        kapah::services
        kapah::ui
        kapah::warnings
        kapah::optimize
)

set_target_properties(kapah_bar PROPERTIES
    AUTOMOC ON
)
