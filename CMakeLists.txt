# ethereal-samples -- samples and benchmarks built on the Donut framework.
#
# This directory is not a standalone CMake project: it is added by the root
# CMakeLists.txt of the ethereal aggregate, after Donut, which provides the
# donut_* / nvrhi targets, ShaderTool and the ethereal_compile_shaders() helper.
#
#   src/         Donut-based samples (DDGI, VXGI, GVDB and the Donut examples)
#   demos/       complete application reconstructions (nv_asteroids)
#   benchmark/   CPU-overhead benchmarks (Asteroids)
#   assets/      scenes and textures used by the samples

if (NOT TARGET donut_app)
	message(FATAL_ERROR "ethereal-samples must be configured from the ethereal root CMakeLists.txt "
		"(Donut has to be added first).")
endif()

# Scenes and textures of the samples. Samples that look assets up by name get this directory
# as the ETHEREAL_ASSETS_DIR compile definition instead of guessing it from the executable's location.
set(ETHEREAL_SAMPLES_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

# The Donut examples find their media (glTF-Sample-Assets, nvidia-logo.png, rt_particles/, ...) as
# <exe dir>/../media, i.e. <build>/media. Link it to media/ instead of copying: it is ~100 MB.
# A directory junction on Windows (no elevation needed, unlike a symbolic link), a symlink elsewhere.
if (ETHEREAL_BUILD_SAMPLES)
	get_filename_component(_media_parent "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" DIRECTORY)
	set(_media_link "${_media_parent}/media")
	set(_media_dir "${CMAKE_CURRENT_SOURCE_DIR}/media")
	if (NOT IS_DIRECTORY "${_media_dir}")
		message(WARNING "${_media_dir} is missing: the Sponza, rt_particles and logo-texture samples will not find their media.")
	elseif (NOT EXISTS "${_media_link}")
		if (WIN32)
			file(TO_NATIVE_PATH "${_media_link}" _media_link_native)
			file(TO_NATIVE_PATH "${_media_dir}" _media_dir_native)
			execute_process(COMMAND cmd /c mklink /J "${_media_link_native}" "${_media_dir_native}"
				RESULT_VARIABLE _media_link_result OUTPUT_QUIET ERROR_QUIET)
		else()
			file(CREATE_LINK "${_media_dir}" "${_media_link}" SYMBOLIC RESULT _media_link_result)
		endif()
		if (NOT _media_link_result STREQUAL "0")
			message(WARNING "Could not link ${_media_link} to ${_media_dir} (${_media_link_result}); "
				"link or copy it by hand so the samples find their media.")
		endif()
	endif()

	add_subdirectory(src)
endif()

if (ETHEREAL_BUILD_BENCHMARKS)
	add_subdirectory(benchmark)
endif()

# Opt-in: nv_asteroids needs the original 2018 demo's media.db and shipped DLLs
# (NV_ASTEROIDS_ORIGINAL_DIR), so it cannot be part of a default configure.
if (ETHEREAL_BUILD_NV_ASTEROIDS)
	add_subdirectory(demos)
endif()
