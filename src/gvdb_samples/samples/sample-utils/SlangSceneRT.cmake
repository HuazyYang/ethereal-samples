# Portable mixed mesh/volume ray tracing; no native SDK in application sources.
if(gvdb_rt_backends)
    set(scene_rt_dir "${CMAKE_CURRENT_LIST_DIR}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-scene-rt.cfg"
        CONTENT "\"${scene_rt_dir}/SlangSceneRT.slang\" -T lib\n" @ONLY)
    shadertool_add_shader_objects(TARGET gvdb_scene_rt_shaders
        CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/gvdb-scene-rt.cfg"
        BACKENDS ${gvdb_rt_backends} ARTIFACT
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_slang"
        INCLUDE_DIRECTORIES "${scene_rt_dir}/../../gvdb/slang"
        DEFINES_DXIL TARGET_D3D12 DEFINES_SPIRV SPIRV TARGET_VULKAN
        EXTRA_FLAGS_OPTIX "-I" "${NVRHI_OPTIX_INCLUDE_DIR}"
            "-texture-sampler" "g_Environment=g_EnvironmentSampler")
    add_library(gvdb_scene_rt STATIC "${scene_rt_dir}/SlangSceneRT.cpp")
    target_link_libraries(gvdb_scene_rt PUBLIC gvdb_slang_runtime donut_core)
    target_include_directories(gvdb_scene_rt PUBLIC "${scene_rt_dir}" "${scene_rt_dir}/../..")
    add_dependencies(gvdb_scene_rt gvdb_scene_rt_shaders)
    add_executable(gvdb_scene_rt_tests "${scene_rt_dir}/SlangSceneRTTests.cpp")
    target_link_libraries(gvdb_scene_rt_tests PRIVATE gvdb_scene_rt donut_app)
    add_dependencies(gvdb_scene_rt_tests gvdb_slang_shaders)
    ethereal_use_agility_sdk(gvdb_scene_rt_tests)
    if(NVRHI_WITH_OPTIX)
        target_link_libraries(gvdb_scene_rt_tests PRIVATE nvrhi::cuda)
        target_compile_definitions(gvdb_scene_rt_tests PRIVATE ETHEREAL_TEST_CUDA=1)
    endif()
    foreach(api DX12 VULKAN CUDA)
        if(api STREQUAL "CUDA")
            if(NOT NVRHI_WITH_OPTIX)
                continue()
            endif()
            set(argument -cuda)
        elseif(api STREQUAL "VULKAN")
            if(NOT NVRHI_WITH_VULKAN)
                continue()
            endif()
            set(argument -vk)
        else()
            if(NOT NVRHI_WITH_DX12)
                continue()
            endif()
            set(argument -dx12)
        endif()
        add_test(NAME gvdb_scene_rt_${api} COMMAND gvdb_scene_rt_tests ${argument})
        set_tests_properties(gvdb_scene_rt_${api} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    endforeach()
endif()
