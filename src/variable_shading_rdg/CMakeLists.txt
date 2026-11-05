#
# Copyright (c) 2026, ethereal contributors. All rights reserved.
# SPDX-License-Identifier: MIT
#
# variable_shading on a render graph (aggregate plan step P2.2). It loads the original's compiled shading-rate shader
# from shaders/variable_shading.

file(GLOB sources "*.cpp" "*.h")

set(project variable_shading_rdg)
set(folder "Examples/Variable Shading")

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_core donut_engine donut_app donut_render)
add_dependencies(${project} variable_shading_shaders)
set_target_properties(${project} PROPERTIES FOLDER ${folder})

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()
