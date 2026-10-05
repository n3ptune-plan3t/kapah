# niri IPC client. Qt Core + JSON only: no Gui, no Widgets, no wayland. This
# keeps the module testable headlessly, which is how the fixture tests work.

add_library(kapah_niri STATIC
    protocol.cpp
    protocol.h
    ipcclient.cpp
    ipcclient.h
    state.cpp
    state.h
    actionsender.cpp
    actionsender.h
)

add_library(kapah::niri ALIAS kapah_niri)

target_include_directories(kapah_niri
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(kapah_niri
    PUBLIC
        Qt6::Core
        kapah::core
    PRIVATE
        kapah::warnings
        kapah::optimize
        kapah::gc-sections
)

set_target_properties(kapah_niri PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)