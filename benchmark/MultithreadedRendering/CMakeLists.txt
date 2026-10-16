# MultithreadedRendering benchmark: the CPU-side cost of nvrhi (through Donut) against native D3D12 on
# the MultithreadedRendering11 workload (SquidRoom, ~1000 meshes / 1025 draws per pass, 6 passes:
# a shadow map, four stencil mirrors and the direct view), recorded single-threaded, one command
# list per pass, or split across threads. Ported from D3D12-Trial/MultithreadedRendering.
# See README.md.
#
#   mt_rendering_native   native/   the sample's D3D12 renderer under a minimal Win32 host
#   mt_rendering_nvrhi    nvrhi/    the same workload on Donut + nvrhi (D3D12, Vulkan)
#   mt_rendering_core     core/     sdkmesh / DDS readers, scene, worker pool, frame schedule shared by both
#   common/benchmark.h              the benchmark contract (command line, timers, CSV/JSON writers)
#
# Added by ethereal-samples/benchmark/CMakeLists.txt after Donut.

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "The MultithreadedRendering benchmark requires Windows x64")
endif()

set(MT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MT_FOLDER "Benchmark/MultithreadedRendering")

# ---------------------------------------------------------------------------------------------
# Shared core
# ---------------------------------------------------------------------------------------------
add_library(mt_rendering_common INTERFACE)
target_include_directories(mt_rendering_common INTERFACE "${MT_ROOT}/common")
target_link_libraries(mt_rendering_common INTERFACE dxgi)

add_library(mt_rendering_core STATIC
	core/dds.cpp core/dds.h
	core/frame_scheduler.h
	core/scene.cpp core/scene.h
	core/sdkmesh.cpp core/sdkmesh.h
	core/workers.h)
target_include_directories(mt_rendering_core PUBLIC "${MT_ROOT}/core")
target_compile_features(mt_rendering_core PUBLIC cxx_std_17)
target_compile_definitions(mt_rendering_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mt_rendering_core PUBLIC mt_rendering_common)
set_target_properties(mt_rendering_core PROPERTIES FOLDER ${MT_FOLDER})

# ---------------------------------------------------------------------------------------------
# Native D3D12: the shader is compiled to headers with fxc (vs_5_0 / ps_5_0), as the sample did
# with D3DCompile at run time.
# ---------------------------------------------------------------------------------------------
find_program(MT_FXC fxc
	HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64"
	PATHS "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
	DOC "fxc.exe from the Windows SDK")
if (NOT MT_FXC)
	message(FATAL_ERROR "fxc.exe (Windows SDK) was not found. Run from a VS developer prompt, "
		"or set -DMT_FXC=<path>, or -DETHEREAL_BUILD_BENCHMARKS=OFF.")
endif()

set(MT_SHADER "${MT_ROOT}/shaders/mt_scene.hlsl")
set(MT_COMPILED_SHADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/CompiledShaders")
file(MAKE_DIRECTORY "${MT_COMPILED_SHADERS_DIR}")
set(MT_COMPILED_SHADERS "${MT_COMPILED_SHADERS_DIR}/mt_scene_vs.h" "${MT_COMPILED_SHADERS_DIR}/mt_scene_ps.h")
add_custom_command(OUTPUT "${MT_COMPILED_SHADERS_DIR}/mt_scene_vs.h"
	COMMAND "${MT_FXC}" /nologo /T vs_5_0 /E VSMain /Vn g_VSMain /Fh "${MT_COMPILED_SHADERS_DIR}/mt_scene_vs.h" "${MT_SHADER}"
	DEPENDS "${MT_SHADER}"
	COMMENT "fxc mt_scene.hlsl (vs_5_0)"
	VERBATIM)
add_custom_command(OUTPUT "${MT_COMPILED_SHADERS_DIR}/mt_scene_ps.h"
	COMMAND "${MT_FXC}" /nologo /T ps_5_0 /E PSMain /Vn g_PSMain /Fh "${MT_COMPILED_SHADERS_DIR}/mt_scene_ps.h" "${MT_SHADER}"
	DEPENDS "${MT_SHADER}"
	COMMENT "fxc mt_scene.hlsl (ps_5_0)"
	VERBATIM)
set_source_files_properties(${MT_COMPILED_SHADERS} PROPERTIES GENERATED TRUE)
set_source_files_properties("${MT_SHADER}" PROPERTIES VS_TOOL_OVERRIDE "None")

add_executable(mt_rendering_native
	native/main.cpp
	native/renderer_d3d12.cpp native/renderer_d3d12.h
	native/d3d12_helpers.h
	"${MT_SHADER}"
	${MT_COMPILED_SHADERS})
target_include_directories(mt_rendering_native PRIVATE native "${MT_COMPILED_SHADERS_DIR}")
target_link_libraries(mt_rendering_native PRIVATE mt_rendering_core d3d12 dxgi dxguid winmm shcore)
set_target_properties(mt_rendering_native PROPERTIES FOLDER ${MT_FOLDER})
source_group("shaders" FILES "${MT_SHADER}")
source_group("generated" FILES ${MT_COMPILED_SHADERS})

# ---------------------------------------------------------------------------------------------
# Donut + nvrhi: the same HLSL through ShaderTool (DXIL, SPIR-V) into <bin>/shaders/mt_rendering_nvrhi/.
# ---------------------------------------------------------------------------------------------
add_executable(mt_rendering_nvrhi
	nvrhi/main.cpp
	nvrhi/renderer.cpp nvrhi/renderer.h)
target_link_libraries(mt_rendering_nvrhi PRIVATE mt_rendering_core donut_app donut_engine donut_core d3d12 dxgi dxguid winmm)
set_target_properties(mt_rendering_nvrhi PROPERTIES FOLDER ${MT_FOLDER})

ethereal_compile_shaders(
	TARGET mt_rendering_nvrhi_shaders
	CONFIG ${MT_ROOT}/shaders/shaders.cfg
	INCLUDE_DIRECTORIES ${MT_ROOT}/shaders
	SOURCES "${MT_SHADER}"
	FOLDER ${MT_FOLDER})
add_dependencies(mt_rendering_nvrhi mt_rendering_nvrhi_shaders)

# Release code generation of the Asteroids benchmark (same flags for the benchmark and Donut/nvrhi).
if (COMMAND asteroids_match_codegen AND ASTEROIDS_MATCH_CODEGEN)
	asteroids_match_codegen(mt_rendering_core)
	asteroids_match_codegen(mt_rendering_native)
	asteroids_match_codegen(mt_rendering_nvrhi)
endif()
