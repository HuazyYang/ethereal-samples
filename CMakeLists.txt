# ethereal-samples -- samples and benchmarks built on the Donut framework.
#
# This directory is not a standalone CMake project: it is added by the root
# CMakeLists.txt of the ethereal aggregate, after Donut, which provides the
# donut_* / nvrhi targets, ShaderTool and the ethereal_compile_shaders() helper.
#
#   src/         Donut-based samples (DDGI, VXGI, GVDB and the Donut examples)
#   benchmark/   CPU-overhead benchmarks (Asteroids)
#   assets/      scenes and textures used by the samples

if (NOT TARGET donut_app)
	message(FATAL_ERROR "ethereal-samples must be configured from the ethereal root CMakeLists.txt "
		"(Donut has to be added first).")
endif()

# Scenes and textures of the samples. Samples that look assets up by name get this directory
# as the ETHEREAL_ASSETS_DIR compile definition instead of guessing it from the executable's location.
set(ETHEREAL_SAMPLES_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

if (ETHEREAL_BUILD_SAMPLES)
	add_subdirectory(src)
endif()

if (ETHEREAL_BUILD_BENCHMARKS)
	add_subdirectory(benchmark)
endif()
