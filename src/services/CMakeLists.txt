# System services. Qt DBus + libudev + libpulse + raw netlink. No Qt Widgets:
# a service must be constructible without a QGuiApplication so the unit tests can
# drive it against a fake bus.

add_library(kapah_services STATIC
    audio.cpp
    audio.h
    backlight.cpp
    backlight.h
    logind.cpp
    logind.h
    network.cpp
    network.h
    power.cpp
    power.h
    service.cpp
    service.h
)

add_library(kapah::services ALIAS kapah_services)

target_include_directories(kapah_services PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(kapah_services
    PUBLIC
        Qt6::DBus
        Qt6::Core
        kapah::core
        kapah::ui
    PRIVATE
        PkgConfig::LIBUDEV
        PkgConfig::LIBPULSE
        kapah::warnings
        kapah::optimize
        kapah::gc-sections
)

set_target_properties(kapah_services PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)

if(KAPAH_WITH_TRAY)
    target_sources(kapah_services PRIVATE tray.cpp tray.h)
    target_compile_definitions(kapah_services PUBLIC KAPAH_WITH_TRAY=1)
endif()

# mpris.{h,cpp}, bluetooth.{h,cpp}, powerprofiles.{h,cpp} are not written yet
# (Phase 3). When they exist, add target_sources blocks here again and flip
# the corresponding KAPAH_WITH_* options in the top-level CMakeLists.