# Core: no Qt GUI dependency, no optional system libraries. Everything else in
# the tree links this.

add_library(kapah_core STATIC
    env.cpp
    env.h
    config.cpp
    config.h
    logging.cpp
    logging.h
    paths.cpp
    paths.h
    toml.cpp
    toml.h
    util.cpp
    util.h
)

add_library(kapah::core ALIAS kapah_core)

target_include_directories(kapah_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(kapah_core
    PUBLIC
        Qt6::Core
    PRIVATE
        kapah::warnings
        kapah::optimize
        kapah::gc-sections
)

set_target_properties(kapah_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)

# Note: we deliberately do not set QT_NO_CAST_FROM_ASCII / QT_USE_QSTRINGBUILDER.
# They are attractive but they only pay off with a fully audited source tree, and
# an unaudited tree fails to compile in ways that are tedious to diagnose. The
# memory win is negligible compared to the budgets we actually track.
target_compile_definitions(kapah_core PRIVATE
    KAPAH_CORE_LIBRARY=1
)