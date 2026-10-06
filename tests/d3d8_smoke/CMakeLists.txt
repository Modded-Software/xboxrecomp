# d3d8_smoke - offline smoke test for the pure xbox_d3d8 format layer.
# Compiles the real d3d8_resources.c + d3d8_swizzle.h with lightweight device
# accessor stubs so the format tables, predicates, swizzle classification and
# software conversions can be validated without a D3D11 device.

if(WIN32)
    add_executable(d3d8_gamma test_gamma.c)
    target_link_libraries(d3d8_gamma PRIVATE xbox_d3d8)

    add_executable(d3d8_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/test_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_resources.c
    )
    target_include_directories(d3d8_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    )
    target_link_libraries(d3d8_smoke PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
    add_executable(nv2a_shader_smoke
        test_nv2a_shader.c
        ../../src/kernel/nv2a_pb_scan.c
        ../../src/kernel/nv2a_pb_exec.c
        ../../src/kernel/nv2a_gpu_d3d11.cpp
    )
    target_include_directories(nv2a_shader_smoke PRIVATE ../../src)
    target_link_libraries(nv2a_shader_smoke PRIVATE platform d3d11 d3dcompiler)
    target_compile_features(nv2a_shader_smoke PRIVATE cxx_std_17)
    target_compile_definitions(nv2a_shader_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(d3d8_smoke PRIVATE /W3)
    endif()

    # Exercise bound state objects on D3D11's software device (no window/GPU).
    add_executable(d3d8_states_smoke
        test_states.c
        ../../src/d3d/d3d8_states.c
    )
    target_include_directories(d3d8_states_smoke PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_states_smoke PRIVATE platform d3d11)

    add_executable(d3d8_a8
        test_a8.c
        ../../src/d3d/d3d8_resources.c
        ../../src/d3d/d3d8_shaders.c
        ../../src/d3d/d3d8_combiners.c
    )
    target_include_directories(d3d8_a8 PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_a8 PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
endif()
