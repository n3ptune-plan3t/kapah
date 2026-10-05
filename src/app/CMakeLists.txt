# kapahd: the main shell process.

add_executable(kapahd
    main.cpp
    daemon.cpp
    daemon.h
    ipcserver.cpp
    ipcserver.h
    idleservice.cpp
    idleservice.h
)

target_include_directories(kapahd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(kapahd
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::DBus
        Qt6::WaylandClient
        Qt6::WaylandClientPrivate
        kapah::core
        kapah::wl
        kapah::niri
        kapah::services
        kapah::ui
        kapah::bar
        kapah::warnings
        kapah::optimize
)

set_target_properties(kapahd PROPERTIES
    AUTOMOC ON
)
