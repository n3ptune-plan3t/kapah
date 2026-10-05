# Per-function / per-data sectioning plus --gc-sections.
#
# This matters for a shell that links Qt: the linker can only drop what it can
# prove is unreferenced, and Qt's static initialisers otherwise drag in large
# swaths of QtGui/QtWidgets.

add_library(kapah_linker INTERFACE)
add_library(kapah::linker ALIAS kapah_linker)

add_library(kapah_gc_sections INTERFACE)
add_library(kapah::gc-sections ALIAS kapah_gc_sections)

target_link_libraries(kapah_gc_sections INTERFACE kapah_linker)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(kapah_gc_sections INTERFACE
        -ffunction-sections
        -fdata-sections
    )
    target_link_options(kapah_gc_sections INTERFACE
        -Wl,--gc-sections
    )
endif()

if(KAPAH_STRIP_RELEASE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_options(kapah_linker INTERFACE
        $<$<CONFIG:Release>:-Wl,-s>
        $<$<CONFIG:RelWithDebInfo>:-Wl,-s>
        $<$<CONFIG:MinSizeRel>:-Wl,-s>
    )
endif()