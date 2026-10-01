# asteroids_nvrhi: the Asteroids workload on Donut + nvrhi (D3D11, D3D12, Vulkan).
# Added by ../CMakeLists.txt, which also defines asteroids_core (the simulation shared with
# asteroids_native), benchmark_common (../common/benchmark.h) and asteroids_copy_media().
# Shader build recipe: docs/BUILD.md.

add_executable(asteroids_nvrhi
	main.cpp
	renderer.cpp
	renderer.h)
target_compile_features(asteroids_nvrhi PRIVATE cxx_std_17)
target_link_libraries(asteroids_nvrhi PRIVATE
	asteroids_core donut_app donut_engine donut_core d3d11 d3d12 dxgi dxguid ninput winmm)
set_target_properties(asteroids_nvrhi PROPERTIES FOLDER "Benchmark/Asteroids")

# HLSL -> dxbc (D3D11), dxil (D3D12), spirv (Vulkan) at build time with Donut's ShaderTool, into
# <bin>/shaders/asteroids_nvrhi/{dxbc,dxil,spirv}. Include path: Donut's shader headers and the
# sample's common_defines.h.
ethereal_compile_shaders(
	TARGET asteroids_nvrhi_shaders
	CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg
	INCLUDE_DIRECTORIES ${ASTEROIDS_ROOT}/assets/shaders
	SOURCES
		shaders/asteroid_vs.hlsl
		shaders/asteroid_ps.hlsl
		shaders/skybox_vs.hlsl
		shaders/skybox_ps.hlsl
	FOLDER "Benchmark/Asteroids"
	ALL_PLATFORMS)
add_dependencies(asteroids_nvrhi asteroids_nvrhi_shaders)

asteroids_copy_media(asteroids_nvrhi)
if (ASTEROIDS_MATCH_CODEGEN)
	asteroids_match_codegen(asteroids_nvrhi)
endif()
