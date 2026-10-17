add_subdirectory(common/shaders)

set(COMMON_SOURCE_FILES
    common/example_app.cpp
    common/example_app.h
    common/dxgi_adapter_selector.cpp
    common/dxgi_adapter_selector.h
    common/example_options.cpp
    common/example_options.h
    common/main_state.h
    common/sky_renderer.cpp
    common/sky_renderer.h
    common/asset_utils.cpp
    common/asset_utils.h
    common/ui_renderer.cpp
    common/ui_renderer.h
    common/Camera.cpp
    common/Camera.h)

set(OCEAN_SOURCE_FILES
    ocean/ocean_renderer.cpp
    ocean/ocean_renderer.h)

set(BEACH_SOURCE_FILES
    beach/beach_assets.cpp
    beach/beach_assets.h
    beach/beach_shallow_simulation.cpp
    beach/beach_cove.cpp
    beach/beach_cove.h
    beach/beach_shallow_simulation.h
    beach/beach_shallow_water.cpp
    beach/beach_shallow_water.h
    beach/beach_lighting.cpp
    beach/beach_lighting.h
    beach/beach_renderer.cpp
    beach/beach_renderer.h
    beach/beach_scene.cpp
    beach/beach_scene.h
    beach/beach_scene_pipeline.cpp
    beach/beach_scene_pipeline.h)

if (WIN32)
    foreach(REQUIRED_TARGET
            donut_app nvrhi_d3d11 nvrhi_d3d12 nvrhi_vk
            waveworks_shaders_bytecode_dx waveworks_shaders_bytecode_vk)
        if (NOT TARGET ${REQUIRED_TARGET})
            message(FATAL_ERROR
                "WaveWorks examples require ${REQUIRED_TARGET}; enable all three RHIs and both shader toolchains")
        endif()
    endforeach()

    set(EXAMPLE_GENERATED_SHADER_HEADERS
        ${EXAMPLE_DX_SHADER_HEADERS}
        ${EXAMPLE_VK_SHADER_HEADERS})
    set_property(SOURCE
        ocean/ocean_renderer.cpp
        beach/beach_renderer.cpp
        beach/beach_lighting.cpp
        beach/beach_scene_pipeline.cpp
        common/sky_renderer.cpp
        common/ui_renderer.cpp
        APPEND PROPERTY OBJECT_DEPENDS "${EXAMPLE_GENERATED_SHADER_HEADERS}")
    add_library(waveworks_example_common STATIC ${COMMON_SOURCE_FILES})
    target_sources(waveworks_example_common PRIVATE ${EXAMPLE_GENERATED_SHADER_HEADERS})
    target_include_directories(waveworks_example_common PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/ocean"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/common"
        "${WAVEWORKS_DEMO_DIR}/sdk/include")
    target_compile_definitions(waveworks_example_common PRIVATE
        WAVEWORKS_MEDIA_DIR="${WAVEWORKS_DEMO_DIR}/media")
    target_compile_definitions(waveworks_example_common PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    target_link_libraries(waveworks_example_common PUBLIC donut_app imgui dxgi waveworks_sdk ShaderTool::Blob Vulkan::Headers)
    add_dependencies(waveworks_example_common
        donut_app nvrhi_d3d11 nvrhi_d3d12 nvrhi_vk
        waveworks_sdk waveworks_shaders_bytecode_dx waveworks_shaders_bytecode_vk)
    set_target_properties(waveworks_example_common PROPERTIES FOLDER "WaveWorksExample")

    function(add_waveworks_example TARGET_NAME ENTRY_SOURCE)
        add_executable(${TARGET_NAME} "${ENTRY_SOURCE}" ${ARGN})
        target_sources(${TARGET_NAME} PRIVATE ${EXAMPLE_GENERATED_SHADER_HEADERS})
        target_link_libraries(${TARGET_NAME} PRIVATE waveworks_example_common)
        add_dependencies(${TARGET_NAME}
            donut_app nvrhi_d3d11 nvrhi_d3d12 nvrhi_vk waveworks_sdk)
        set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "WaveWorksExample")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:waveworks_sdk>"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/$<TARGET_FILE_NAME:waveworks_sdk>"

)
        install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION "bin")
    endfunction()

    add_waveworks_example(waveworks_ocean ocean/main.cpp
        ${OCEAN_SOURCE_FILES})
    add_waveworks_example(waveworks_beach beach/main.cpp
        ${OCEAN_SOURCE_FILES} ${BEACH_SOURCE_FILES})
endif()
