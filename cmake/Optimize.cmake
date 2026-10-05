# Optimisation flags.
#
# Default is -O2 (roadmap section 3). KAPAH_RELEASE_SMALL switches to -Os for
# distro builds that care more about resident text size than CPU on an already
# slow machine. We never use -Ofast: the JSON parsers and the niri event reducer
# are hot enough that fast-math's value is noise next to the risk.

add_library(kapah_optimize INTERFACE)

if(KAPAH_RELEASE_SMALL)
    target_compile_options(kapah_optimize INTERFACE
        $<$<CONFIG:Release>:-Os>
        $<$<CONFIG:RelWithDebInfo>:-Os>
        $<$<CONFIG:MinSizeRel>:-Os>
    )
else()
    target_compile_options(kapah_optimize INTERFACE
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:MinSizeRel>:-O2>
    )
endif()

# Build-time define so code can compile out debug-only work (see src/core/logging.h).
target_compile_definitions(kapah_optimize INTERFACE
    $<$<CONFIG:Debug>:KAPAH_DEBUG_BUILD=1>
    $<$<CONFIG:RelWithDebInfo>:KAPAH_DEBUG_BUILD=1>
    $<$<CONFIG:Release>:KAPAH_DEBUG_BUILD=0>
    $<$<CONFIG:MinSizeRel>:KAPAH_DEBUG_BUILD=0>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kapah_optimize INTERFACE
        -fno-math-errno
        -fno-semantic-interposition
    )
endif()

if(KAPAH_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT kapah_ipo_ok OUTPUT kapah_ipo_msg)
    if(kapah_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "LTO requested but not supported: ${kapah_ipo_msg}")
    endif()
endif()