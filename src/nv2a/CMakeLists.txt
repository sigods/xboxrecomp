# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_pgraph_d3d11.c
    nv2a_pb_replay.c
    nv2a_pb_gl.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
)

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# nv2a_pb_gl.c implements the executor's back-end interface (nv2a_backend.h),
# which lives with the executor in src/kernel.
target_include_directories(xbox_nv2a PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../kernel)

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
else()
    # nv2a_pb_gl.c: the OpenGL back end draws with SDL2 + libepoxy, the same
    # pair src/d3d uses on POSIX (d3d8_gl.c). Same _LINK_LIBRARIES form, for
    # the same reason: absolute paths, so no -L guessing.
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_link_libraries(xbox_nv2a PRIVATE SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
    target_include_directories(xbox_nv2a PRIVATE ${EPOXY_INCLUDE_DIRS})
endif()
