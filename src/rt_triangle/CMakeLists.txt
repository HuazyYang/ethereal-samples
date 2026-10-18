#
# Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.


file(GLOB shaders "*.slang")
file(GLOB sources "*.cpp" "*.h")

set(project rt_triangle)
set(folder "Examples/Ray Traced Triangle")

ethereal_compile_shaders(
    SLANG
    TARGET ${project}_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
    SOURCES ${shaders}
    FOLDER ${folder}
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_app donut_engine)
add_dependencies(${project} ${project}_shaders donut_shaders)
ethereal_use_agility_sdk(${project})
set_target_properties(${project} PROPERTIES FOLDER ${folder})

# Run the sample renderer into an offscreen texture and validate GPU readback.
if (NVRHI_WITH_DX12)
    add_test(NAME slang_render_rt_triangle_DX12 COMMAND $<TARGET_FILE:${project}> -dx12 --test-headless)
    set_tests_properties(slang_render_rt_triangle_DX12 PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
if (NVRHI_WITH_VULKAN)
    add_test(NAME slang_render_rt_triangle_VULKAN COMMAND $<TARGET_FILE:${project}> -vk --test-headless)
    set_tests_properties(slang_render_rt_triangle_VULKAN PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

# One scene verifies the unified local binding interface across DXR/Vulkan/OptiX.
set(shared_rt_backends)
if(NVRHI_WITH_DX12)
    list(APPEND shared_rt_backends SLANG-DXIL)
endif()
if(NVRHI_WITH_VULKAN)
    list(APPEND shared_rt_backends SLANG-SPIRV)
endif()
if(NVRHI_WITH_OPTIX)
    list(APPEND shared_rt_backends SLANG-OPTIX)
endif()
if(shared_rt_backends)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shared-rt.cfg"
        CONTENT "\"${CMAKE_CURRENT_SOURCE_DIR}/tests/shared-rt.slang\" -T lib\n" @ONLY)
    shadertool_add_shader_objects(TARGET slang_rt_shared_shaders
        CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/shared-rt.cfg"
        BACKENDS ${shared_rt_backends} ARTIFACT
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/shared_rt"
        DEFINES_SPIRV TARGET_VULKAN=1
        EXTRA_FLAGS_OPTIX "-I" "${NVRHI_OPTIX_INCLUDE_DIR}")
    add_executable(slang_rt_shared tests/shared-rt.cpp)
    target_link_libraries(slang_rt_shared PRIVATE donut_app)
    add_dependencies(slang_rt_shared slang_rt_shared_shaders)
    ethereal_use_agility_sdk(slang_rt_shared)
    foreach(api DX12 VULKAN CUDA)
        if(api STREQUAL "CUDA")
            if(NOT NVRHI_WITH_OPTIX)
                continue()
            endif()
            target_compile_definitions(slang_rt_shared PRIVATE ETHEREAL_TEST_CUDA=1)
            target_link_libraries(slang_rt_shared PRIVATE nvrhi::cuda)
            set(arg -cuda)
            set(kind optix)
        elseif(api STREQUAL "VULKAN")
            if(NOT NVRHI_WITH_VULKAN)
                continue()
            endif()
            set(arg -vk)
            set(kind spirv)
        else()
            if(NOT NVRHI_WITH_DX12)
                continue()
            endif()
            set(arg -dx12)
            set(kind dxil)
        endif()
        add_test(NAME slang_rt_shared_${api} COMMAND slang_rt_shared ${arg}
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/shared_rt/${kind}/shared-rt.bin")
        set_tests_properties(slang_rt_shared_${api} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    endforeach()
endif()
