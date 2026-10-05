# AddressSanitizer + UndefinedBehaviorSanitizer job (roadmap section 10.1).
#
# ASan roughly doubles PSS, so the perf gate and the sanitizer job are separate
# CI jobs; never combine them.

option(KAPAH_ENABLE_ASAN "AddressSanitizer + UBSan" OFF)

if(KAPAH_ENABLE_ASAN)
    add_library(kapah_sanitizers INTERFACE)
    target_compile_options(kapah_sanitizers INTERFACE
        -fsanitize=address
        -fsanitize=undefined
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(kapah_sanitizers INTERFACE
        -fsanitize=address
        -fsanitize=undefined
    )

    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address,undefined")
endif()