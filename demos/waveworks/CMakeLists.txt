cmake_minimum_required(VERSION 3.25)

if(NOT WIN32)
    message(STATUS "WaveWorks examples currently require Windows")
    return()
endif()
if(NOT TARGET donut_app OR NOT TARGET nvrhi_d3d11)
    message(FATAL_ERROR "WaveWorks requires Ethereal Donut and the D3D11 nvrhi backend")
endif()

set(WAVEWORKS_DEMO_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_subdirectory(sdk)
add_subdirectory(examples)
