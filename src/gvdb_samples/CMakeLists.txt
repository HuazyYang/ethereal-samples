# ---------------------------------------------------------------------------
# GVDBSamples -- the gvdb sparse-voxel-database library and its samples.
#
#   gvdb/     the library: the gp compute abstraction (CUDA backend, OptiX
#             backend, nvrhi interop), the GVDB data structure and its
#             operation objects, the Donut scene-graph integration
#   samples/  sample-utils (renderers, app framework) and the samples, one
#             directory per sample (kebab-case), ported from gvdb-voxels/source
#
# Requires the CUDA toolkit (CUDA is enabled as a project language by the root
# CMakeLists when ETHEREAL_BUILD_GVDB is ON). OptiX is optional
# (ETHEREAL_OPTIX_FOUND, set by the root CMakeLists).
# ---------------------------------------------------------------------------

find_package(CUDAToolkit REQUIRED)

# Include root: every header is included as <gvdb/...> or <sample-utils/...>.
set(GVDB_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(GVDB_SAMPLES_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/samples")
# Shared assets of the samples (large; referenced in place, not copied).
set(GVDB_SAMPLES_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/samples/assets")
# Every PTX module lands here; the samples mount it as "ptx".
set(GVDB_PTX_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ptx")
# Virtual architecture the PTX is generated for (JIT-compiled by the driver).
set(GVDB_PTX_ARCH "75" CACHE STRING "CUDA architecture of the GVDB PTX modules")

# gvdb_add_ptx(<target> SOURCES <.cu ...> [INCLUDE_DIRECTORIES <dir ...>] [OPTIX] [DEFINES <def ...>])
#
# Compiles the .cu files to one PTX file each (named after the source stem) and
# copies them to GVDB_PTX_OUTPUT_DIR when <target> is built. Add <target> as a
# dependency of the executables that load the modules. OPTIX adds the OptiX
# include directory and the flags its device code needs.
function(gvdb_add_ptx target)
    cmake_parse_arguments(arg "OPTIX" "" "SOURCES;INCLUDE_DIRECTORIES;DEFINES" ${ARGN})
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "gvdb_add_ptx(${target}): SOURCES required")
    endif()
    set(objlib ${target}_objects)
    add_library(${objlib} OBJECT ${arg_SOURCES})
    set_target_properties(${objlib} PROPERTIES
        CUDA_PTX_COMPILATION ON
        CUDA_ARCHITECTURES "${GVDB_PTX_ARCH}"
        FOLDER "GVDBSamples/ptx")
    target_compile_options(${objlib} PRIVATE --use_fast_math -lineinfo "-Xcompiler=/wd4819")
    target_include_directories(${objlib} PRIVATE "${GVDB_INCLUDE_ROOT}" "${GVDB_SAMPLES_INCLUDE_ROOT}" ${arg_INCLUDE_DIRECTORIES})
    if(arg_DEFINES)
        target_compile_definitions(${objlib} PRIVATE ${arg_DEFINES})
    endif()
    if(arg_OPTIX)
        if(NOT ETHEREAL_OPTIX_FOUND)
            message(FATAL_ERROR "gvdb_add_ptx(${target}): OPTIX requested but the OptiX SDK was not found")
        endif()
        target_include_directories(${objlib} PRIVATE "${OptiX_INCLUDE}")
        target_compile_definitions(${objlib} PRIVATE OPTIX_PATHWAY=1)
        # OptiX device code: relocatable so unused functions survive, and no
        # host-side exception handling flags.
        target_compile_options(${objlib} PRIVATE -rdc=true)
    else()
        target_compile_definitions(${objlib} PRIVATE CUDA_PATHWAY=1)
    endif()
    add_custom_target(${target}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GVDB_PTX_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_OBJECTS:${objlib}> "${GVDB_PTX_OUTPUT_DIR}"
        COMMAND_EXPAND_LISTS
        DEPENDS ${objlib}
        COMMENT "Copying PTX modules of ${target} to ${GVDB_PTX_OUTPUT_DIR}")
    set_target_properties(${target} PROPERTIES FOLDER "GVDBSamples/ptx")
endfunction()

# gvdb_add_sample(<name> SOURCES <files...> [PTX <.cu ...>] [SHADERS <shaders.cfg>] [OPTIX] [LIBRARIES <targets...>])
#
# A console executable (Donut window + stdout logging) linked against the
# library and sample-utils, with its own PTX modules and HLSL shaders. OPTIX
# samples are skipped when the SDK is not available.
function(gvdb_add_sample name)
    cmake_parse_arguments(arg "OPTIX" "SHADERS" "SOURCES;PTX;LIBRARIES" ${ARGN})
    if(arg_OPTIX AND NOT ETHEREAL_OPTIX_FOUND)
        message(STATUS "GVDB sample ${name} skipped (needs OptiX)")
        return()
    endif()
    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE gvdb_sample_utils gvdb donut_app donut_engine ${arg_LIBRARIES})
    target_compile_definitions(${name} PRIVATE
        "GVDB_SAMPLES_ASSETS_DIR=\"${GVDB_SAMPLES_ASSETS_DIR}\""
        "GVDB_SAMPLE_NAME=\"${name}\"")
    add_dependencies(${name} gvdb_ptx gvdb_sample_utils_ptx gvdb_sample_utils_shaders)
    if(TARGET gvdb_sample_utils_optix_ptx)
        add_dependencies(${name} gvdb_sample_utils_optix_ptx)
    endif()
    if(arg_PTX)
        set(ptx_args SOURCES ${arg_PTX})
        if(arg_OPTIX)
            list(APPEND ptx_args OPTIX)
        endif()
        gvdb_add_ptx(${name}_ptx ${ptx_args})
        add_dependencies(${name} ${name}_ptx)
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
