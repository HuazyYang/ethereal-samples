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

set(project basic_triangle)
set(folder "Examples/Basic Triangle")

ethereal_compile_shaders(
    SLANG
    ALL_PLATFORMS
    TARGET ${project}_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
    FOLDER ${folder}
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_app donut_engine)
add_dependencies(${project} ${project}_shaders)
set_target_properties(${project} PROPERTIES FOLDER ${folder})

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()

# Run the sample renderer into an offscreen texture and validate GPU readback.
if (NVRHI_WITH_DX11)
    add_test(NAME slang_render_basic_triangle_DX11 COMMAND $<TARGET_FILE:${project}> -dx11 --test-headless)
    set_tests_properties(slang_render_basic_triangle_DX11 PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
if (NVRHI_WITH_DX12)
    add_test(NAME slang_render_basic_triangle_DX12 COMMAND $<TARGET_FILE:${project}> -dx12 --test-headless)
    set_tests_properties(slang_render_basic_triangle_DX12 PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
if (NVRHI_WITH_VULKAN)
    add_test(NAME slang_render_basic_triangle_VULKAN COMMAND $<TARGET_FILE:${project}> -vk --test-headless)
    set_tests_properties(slang_render_basic_triangle_VULKAN PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
