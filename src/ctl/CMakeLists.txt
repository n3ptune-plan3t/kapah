# kapahctl: Qt Core + socket only, no Widgets, no QGuiApplication.
# The whole point of the binary is to start in < 5 ms.

add_executable(kapahctl
    main.cpp
)

target_include_directories(kapahctl PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core"
)

target_link_libraries(kapahctl
    PRIVATE
        Qt6::Core
        kapah::core
        kapah::warnings
        kapah::optimize
)
