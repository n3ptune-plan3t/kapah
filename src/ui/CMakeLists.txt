# Shared UI toolkit. Qt Gui + Widgets, no QtQuick (roadmap section 1).

add_library(kapah_ui STATIC
    calendar.cpp
    calendar.h
    icons.cpp
    icons.h
    paint.cpp
    paint.h
    popup.cpp
    popup.h
    theme.cpp
    theme.h
    widgets.cpp
    widgets.h
)

add_library(kapah::ui ALIAS kapah_ui)

target_include_directories(kapah_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(kapah_ui
    PUBLIC
        Qt6::Widgets
        Qt6::Gui
        kapah::core
        kapah::wl
    PRIVATE
        kapah::warnings
        kapah::optimize
        kapah::gc-sections
)

set_target_properties(kapah_ui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)