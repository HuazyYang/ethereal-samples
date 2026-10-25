# ---------------------------------------------------------------------------
# GVDBSamples -- the gvdb sparse-voxel-database library and its samples.
#
#   gvdb/     the library: the GVDB data structure, its operation objects and
#             the Donut scene-graph integration, on the nvrhi CUDA backend
#             with Slang kernels (gvdb/shaders)
#   samples/  sample-utils (renderers, app framework) and the samples, one
#             directory per sample (kebab-case), ported from gvdb-voxels/source
#
# Requires the CUDA backend of nvrhi (ETHEREAL_WITH_CUDA). OptiX is optional
# (NVRHI_WITH_OPTIX): the OptiX samples are skipped without it.
# ---------------------------------------------------------------------------

# Include root: every header is included as <gvdb/...> or <sample-utils/...>.
set(GVDB_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(GVDB_SAMPLES_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/samples")
# Shared assets of the samples (large; referenced in place, not copied).
set(GVDB_SAMPLES_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/samples/assets")
# Every CUDA shader container lands here; the samples mount it as "shaders/gvdb".
set(GVDB_SHADER_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb")
# ShaderTool flags of every GVDB container: fast math (NVRTC --use_fast_math), as the reference's nvcc builds.
set(GVDB_SHADER_FLAGS -fp-mode fast)

# gvdb_add_sample(<name> SOURCES <files...> [KERNELS <.slang ...>] [SHADERS <shaders.cfg>] [OPTIX]
#                 [LIBRARIES <targets...>])
#
# A console executable (Donut window + stdout logging) linked against the
# library and sample-utils, with its own Slang kernels (compiled for the CUDA
# device into GVDB_SHADER_OUTPUT_DIR) and HLSL shaders. OPTIX samples are
# skipped when nvrhi has no OptiX.
function(gvdb_add_sample name)
    cmake_parse_arguments(arg "OPTIX" "SHADERS" "SOURCES;KERNELS;LIBRARIES" ${ARGN})
    if(arg_OPTIX AND NOT NVRHI_WITH_OPTIX)
        message(STATUS "GVDB sample ${name} skipped (needs OptiX)")
        return()
    endif()
    add_executable(${name} ${arg_SOURCES} ${arg_KERNELS})
    target_link_libraries(${name} PRIVATE gvdb_sample_utils gvdb donut_app donut_engine ${arg_LIBRARIES})
    target_compile_definitions(${name} PRIVATE
        "GVDB_SAMPLES_ASSETS_DIR=\"${GVDB_SAMPLES_ASSETS_DIR}\""
        "GVDB_SAMPLE_NAME=\"${name}\"")
    add_dependencies(${name} gvdb_shaders gvdb_sample_utils_kernels gvdb_sample_utils_shaders)
    if(TARGET gvdb_sample_utils_optix)
        add_dependencies(${name} gvdb_sample_utils_optix)
    endif()
    if(arg_KERNELS)
        set_source_files_properties(${arg_KERNELS} PROPERTIES HEADER_FILE_ONLY ON)
        ethereal_compile_shader_containers(
            TARGET ${name}_kernels
            SOURCES ${arg_KERNELS}
            TARGETS ptx
            OUTPUT_DIRECTORY "${GVDB_SHADER_OUTPUT_DIR}"
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            INCLUDE_DIRECTORIES "${GVDB_INCLUDE_ROOT}/gvdb/shaders" "${GVDB_SAMPLES_INCLUDE_ROOT}/sample-utils/kernels"
            EXTRA_FLAGS ${GVDB_SHADER_FLAGS}
            FOLDER "GVDBSamples/shaders")
        add_dependencies(${name} ${name}_kernels)
    endif()
    if(arg_SHADERS)
        ethereal_compile_shaders(
            TARGET ${name}_shaders
            CONFIG "${arg_SHADERS}"
            FOLDER "GVDBSamples/shaders"
            OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${name}"
            ALL_PLATFORMS)
        add_dependencies(${name} ${name}_shaders)
    endif()
    set_target_properties(${name} PROPERTIES FOLDER "GVDBSamples")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W3 /MP)
    endif()
endfunction()

add_subdirectory(gvdb)
add_subdirectory(samples)
add_subdirectory(sph_fluid_pool)
