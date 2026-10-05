# Warnings shared by every kapah target.
#
# The roadmap mandates -Wall -Wextra -Wpedantic -Werror in CI. We keep -Werror
# off by default for developer convenience and turn it on via
# -DKAPAH_WERROR=ON (which CI does) so that a local build of a work-in-progress
# branch is not blocked.

option(KAPAH_WERROR "Treat warnings as errors" OFF)

add_library(kapah_warnings INTERFACE)
add_library(kapah::warnings ALIAS kapah_warnings)

target_compile_options(kapah_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference
    -Wno-unused-parameter
)

if(KAPAH_WERROR)
    target_compile_options(kapah_warnings INTERFACE -Werror)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kapah_warnings INTERFACE
        -Wno-psabi
        # Qt headers and generated wayland code are not -Wpedantic clean.
        -Wno-pedantic
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(kapah_warnings INTERFACE
        -Wno-maybe-uninitialized
        -Wno-dangling-else
    )
endif()