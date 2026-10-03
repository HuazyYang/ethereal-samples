# Asteroids benchmark: the CPU-side cost of nvrhi (through Donut) against hand-written
# D3D11 / D3D12 on the Asteroids workload (50,000 asteroids, 1000 meshes, 10 textures).
# See README.md and docs/.
#
#   asteroids_native   native/    the stock Intel sample renderers, D3D11 and D3D12
#   asteroids_nvrhi    nvrhi/     the same workload on Donut + nvrhi (D3D11, D3D12, Vulkan)
#   asteroids_core     core/      simulation, meshes, textures, camera shared by both
#   benchmark_common   common/    the benchmark contract (command line, timers, CSV/JSON writers)
#
# Added by ethereal-samples/benchmark/CMakeLists.txt after Donut.

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "The Asteroids benchmark requires Windows x64")
endif()

option(ASTEROIDS_MATCH_CODEGEN
	"Compile the benchmark and every Donut/nvrhi target with the same Release code generation (/GL /arch:AVX2 /Oi /Ot /GF, LTCG) so that both renderers are optimised alike. Affects Release builds of all of Donut."
	ON)

set(ASTEROIDS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------------------------
# Release code generation
#
# The numbers in report/ were measured with both renderers built with /O2 /Ob2 /Ot /Oi /GF
# /arch:AVX2 /GL and linked with /LTCG, plus symbols (/Zi, /DEBUG:FULL /OPT:REF /OPT:ICF
# /INCREMENTAL:NO) for the profiling phase. Donut and nvrhi set none of these, so they are applied
# from outside, without touching their sources or CMake files. RTTI stays on: nvrhi's validation
# layer and Donut's scene graph use dynamic_cast, and asteroids_nvrhi shares their polymorphic types.
# ---------------------------------------------------------------------------------------------
function(asteroids_match_codegen target)
	if (NOT MSVC)
		return()
	endif()
	get_target_property(_type ${target} TYPE)
	if (NOT _type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
		return()
	endif()
	target_compile_options(${target} PRIVATE
		$<$<CONFIG:Release>:/Zi> $<$<CONFIG:Release>:/GL> $<$<CONFIG:Release>:/arch:AVX2>
		$<$<CONFIG:Release>:/Oi> $<$<CONFIG:Release>:/Ot> $<$<CONFIG:Release>:/GF>)
	if (_type STREQUAL "STATIC_LIBRARY")
		set_property(TARGET ${target} APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:Release>:/LTCG>)
	elseif (NOT _type STREQUAL "OBJECT_LIBRARY")
		target_link_options(${target} PRIVATE
			$<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF>
			$<$<CONFIG:Release>:/OPT:ICF> $<$<CONFIG:Release>:/INCREMENTAL:NO>)
	endif()
endfunction()

function(asteroids_collect_targets dir out)
	get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
	get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
	foreach (_subdir IN LISTS _subdirs)
		# ShaderTool is a build-time tool that links vendored compiler libraries: left as it is.
		if (NOT _subdir MATCHES "/thirdparty/shadertool$")
			asteroids_collect_targets("${_subdir}" _nested)
			list(APPEND _targets ${_nested})
		endif()
	endforeach()
	set(${out} ${_targets} PARENT_SCOPE)
endfunction()

if (ASTEROIDS_MATCH_CODEGEN)
	asteroids_collect_targets("${ETHEREAL_DONUT_DIR}" _donut_targets)
	foreach (_target IN LISTS _donut_targets)
		asteroids_match_codegen(${_target})
	endforeach()
	message(STATUS "Asteroids: benchmark code generation options applied to: ${_donut_targets}")
endif()

# ---------------------------------------------------------------------------------------------
# Shared benchmark contract and sample core
# ---------------------------------------------------------------------------------------------
add_library(benchmark_common INTERFACE)
target_include_directories(benchmark_common INTERFACE "${ASTEROIDS_ROOT}/common")
target_link_libraries(benchmark_common INTERFACE dxgi)

# The simulation, meshes, procedural textures and camera of the Intel sample. Both renderers use
# exactly these sources, so that they render the same scene (static_scene_hash).
add_library(asteroids_core STATIC
	core/camera.cpp core/camera.h
	core/DDSTextureLoader.cpp core/DDSTextureLoader.h core/dds.h
	core/mesh.cpp core/mesh.h
	core/noise.h
	core/settings.h
	core/simplexnoise1234.c core/simplexnoise1234.h
	core/simulation.cpp core/simulation.h
	core/texture.cpp core/texture.h
	core/util.h)
target_include_directories(asteroids_core PUBLIC
	"${ASTEROIDS_ROOT}/core" "${ASTEROIDS_ROOT}/SDK/Include" "${ASTEROIDS_ROOT}/assets/shaders")
target_compile_definitions(asteroids_core PUBLIC NOMINMAX)
target_link_libraries(asteroids_core PUBLIC benchmark_common d3d11 d3d12 dxgi dxguid)
if (MSVC)
	# Warning exclusions of the Intel sample.
	target_compile_options(asteroids_core PUBLIC /wd4201 /wd4324 /wd4238)
endif()
set_target_properties(asteroids_core PROPERTIES FOLDER "Benchmark/Asteroids")
if (ASTEROIDS_MATCH_CODEGEN)
	asteroids_match_codegen(asteroids_core)
endif()

# The skybox and the sprite sheets, next to the executables (the renderers load "media/<file>").
set(ASTEROIDS_MEDIA starbox_1024.dds directx11.dds directx12.dds)
function(asteroids_copy_media target)
	set(_commands COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/media")
	foreach (_file IN LISTS ASTEROIDS_MEDIA)
		list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${ASTEROIDS_ROOT}/assets/media/${_file}" "$<TARGET_FILE_DIR:${target}>/media/${_file}")
	endforeach()
	add_custom_command(TARGET ${target} POST_BUILD ${_commands} VERBATIM)
endfunction()

# ---------------------------------------------------------------------------------------------
# asteroids_native: D3D11 and D3D12. The shaders are compiled to headers with fxc (Windows SDK),
# as the stock sample does: *_5_0, and asteroid_ps as ps_5_1.
# ---------------------------------------------------------------------------------------------
find_program(ASTEROIDS_FXC fxc
	HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64"
	PATHS "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
	DOC "fxc.exe from the Windows SDK")
if (NOT ASTEROIDS_FXC)
	message(FATAL_ERROR "fxc.exe (Windows SDK) was not found. Run from a VS developer prompt, "
		"or set -DASTEROIDS_FXC=<path>, or -DETHEREAL_BUILD_BENCHMARKS=OFF.")
endif()

set(ASTEROIDS_NATIVE_SHADERS
	assets/shaders/asteroid_ps.psh
	assets/shaders/asteroid_ps_d3d11.psh
	assets/shaders/font_ps.psh
	assets/shaders/skybox_ps.psh
	assets/shaders/sprite_ps.psh
	assets/shaders/asteroid_vs.vsh
	assets/shaders/skybox_vs.vsh
	assets/shaders/sprite_vs.vsh)
set_source_files_properties(${ASTEROIDS_NATIVE_SHADERS} PROPERTIES VS_TOOL_OVERRIDE "None")

set(COMPILED_SHADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/CompiledShaders")
file(MAKE_DIRECTORY "${COMPILED_SHADERS_DIR}")
set(ASTEROIDS_COMPILED_SHADERS)
foreach (SRC_SHADER IN LISTS ASTEROIDS_NATIVE_SHADERS)
	get_filename_component(SHADER_NAME ${SRC_SHADER} NAME_WE)
	get_filename_component(SHADER_EXT ${SRC_SHADER} EXT)
	set(COMPILED_SHADER "${COMPILED_SHADERS_DIR}/${SHADER_NAME}.h")
	list(APPEND ASTEROIDS_COMPILED_SHADERS "${COMPILED_SHADER}")

	set(PROFILE vs_5_0)
	if (SHADER_NAME STREQUAL "asteroid_ps")
		set(PROFILE ps_5_1)
	elseif (SHADER_EXT STREQUAL ".psh")
		set(PROFILE ps_5_0)
	endif()

	add_custom_command(OUTPUT "${COMPILED_SHADER}" # the full path is required for correct change detection
		COMMAND "${ASTEROIDS_FXC}" /nologo /T ${PROFILE} /E ${SHADER_NAME} /Vn g_${SHADER_NAME} /Fh "${COMPILED_SHADER}" "${SRC_SHADER}"
		WORKING_DIRECTORY "${ASTEROIDS_ROOT}"
		DEPENDS "${ASTEROIDS_ROOT}/${SRC_SHADER}"
			"${ASTEROIDS_ROOT}/assets/shaders/common_defines.h" "${ASTEROIDS_ROOT}/assets/shaders/shader_common.h"
		COMMENT "fxc ${SRC_SHADER} (${PROFILE})"
		VERBATIM)
endforeach()
set_source_files_properties(${ASTEROIDS_COMPILED_SHADERS} PROPERTIES GENERATED TRUE)

add_executable(asteroids_native
	native/WinWrapper.cpp
	native/benchmark_support.h
	native/gui.h native/font.h native/sprite.h
	native/intel_clear_bd_50_usascii.inl native/stb_font_consolas_bold_50_usascii.inl
	native/d3d11/asteroids_d3d11.cpp native/d3d11/asteroids_d3d11.h
	native/d3d12/asteroids_d3d12.cpp native/d3d12/asteroids_d3d12.h
	native/d3d12/descriptor.h native/d3d12/subset_d3d12.h native/d3d12/upload_heap.h
	${ASTEROIDS_NATIVE_SHADERS}
	assets/shaders/common_defines.h assets/shaders/shader_common.h
	${ASTEROIDS_COMPILED_SHADERS}) # a source that is the output of a custom command gives the target its build rule
target_include_directories(asteroids_native PRIVATE
	native native/d3d11 native/d3d12 "${COMPILED_SHADERS_DIR}")
target_link_libraries(asteroids_native PRIVATE asteroids_core d3d11 d3d12 dxgi dxguid ninput winmm shcore)
set_target_properties(asteroids_native PROPERTIES FOLDER "Benchmark/Asteroids")
source_group("shaders" FILES ${ASTEROIDS_NATIVE_SHADERS} assets/shaders/common_defines.h assets/shaders/shader_common.h)
source_group("generated" FILES ${ASTEROIDS_COMPILED_SHADERS})
asteroids_copy_media(asteroids_native)
if (ASTEROIDS_MATCH_CODEGEN)
	asteroids_match_codegen(asteroids_native)
endif()

# ---------------------------------------------------------------------------------------------
# asteroids_nvrhi: Donut + nvrhi
# ---------------------------------------------------------------------------------------------
add_subdirectory(nvrhi)

# ---------------------------------------------------------------------------------------------
# Header self-test (also prints the adapter LUIDs for -adapter_luid) and analysis unit tests
# ---------------------------------------------------------------------------------------------
add_executable(benchmark_selftest common/selftest.cpp
	common/benchmark.h common/benchmark_d3d11.h common/benchmark_d3d12.h)
target_link_libraries(benchmark_selftest PRIVATE benchmark_common)
set_target_properties(benchmark_selftest PROPERTIES FOLDER "Benchmark/Asteroids")
if (MSVC)
	target_compile_options(benchmark_selftest PRIVATE /W4 /WX)
endif()

enable_testing()
add_test(NAME benchmark_selftest COMMAND benchmark_selftest)
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND AND EXISTS "${ASTEROIDS_ROOT}/tests/analysis")
	add_test(NAME benchmark_analysis
		COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${ASTEROIDS_ROOT}/tests/analysis")
endif()
