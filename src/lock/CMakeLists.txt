# Lock screen: kapah-lock binary. No dependency on the rest of the shell's UI
# toolkit -- the lock must stay minimal and self-contained.

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXKBCOMMON IMPORTED_TARGET xkbcommon)

set(KAPAH_LOCK_SOURCES
    main.cpp
    pam.cpp
    pam.h
    keyboard.cpp
    keyboard.h
    passwordfield.cpp
    passwordfield.h
    lockview.cpp
    lockview.h
)

add_executable(kapah-lock ${KAPAH_LOCK_SOURCES})

target_include_directories(kapah-lock PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/wl"
    "${CMAKE_SOURCE_DIR}/src/ui"
    "${CMAKE_SOURCE_DIR}/src/core"
    "${CMAKE_SOURCE_DIR}/src/niri"
)

target_link_libraries(kapah-lock PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    Qt6::WaylandClient
    Qt6::WaylandClientPrivate
    kapah::core
    kapah::wl
    kapah::wl-lock
    kapah::niri
    kapah::ui
    PkgConfig::PAM
    PkgConfig::LIBXKBCOMMON
    kapah::warnings
    kapah::optimize
)

if(KAPAH_WITH_PAM)
    target_compile_definitions(kapah-lock PRIVATE KAPAH_WITH_PAM=1)
endif()

set_target_properties(kapah-lock PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
)
