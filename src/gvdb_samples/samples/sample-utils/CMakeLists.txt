# sample-utils: what the samples share on top of the gvdb library.
#   VolRenderer     CUDA raycaster over the volume instances of a GVDBSceneGraph
#   OptixRenderer   OptiX path tracer over volume + mesh instances (needs OptiX)
#   GVDBApp         Donut application base (devices, scene graph, camera/light
#                   orbit, screen presentation, debug lines, ImGui)
#   HeadlessContext console samples (CUDA device + file system), ImageIO (PNG)
#   ObjMesh         OBJ -> Donut MeshInfo / MeshInstance and device buffers

# --- CUDA render kernels (Slang) ----------------------------------------------
# kernels/*.slang -> bin/shaders/gvdb/*.nvsc, next to the library's containers.

ethereal_compile_shader_containers(
    TARGET gvdb_sample_utils_kernels
    SOURCES "kernels/GVDBRaycast.slang"
    TARGETS ptx
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb"
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/kernels"
    INCLUDE_DIRECTORIES "${GVDB_INCLUDE_ROOT}/gvdb/shaders" "${CMAKE_CURRENT_LIST_DIR}/kernels"
    EXTRA_FLAGS ${GVDB_SHADER_FLAGS}
    FOLDER "GVDBSamples/shaders")

# --- OptiX programs (Slang) ---------------------------------------------------
# kernels/optix/OptixPrograms.slang -> bin/shaders/gvdb/OptixPrograms.nvsc (OptixRenderer).

if (NVRHI_WITH_OPTIX)
    ethereal_compile_shader_containers(
        TARGET gvdb_sample_utils_optix
        SOURCES "kernels/optix/OptixPrograms.slang"
        TARGETS optix
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb"
        BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/kernels/optix"
        INCLUDE_DIRECTORIES "${GVDB_INCLUDE_ROOT}/gvdb/shaders" "${CMAKE_CURRENT_LIST_DIR}/kernels"
        EXTRA_FLAGS ${GVDB_SHADER_FLAGS}
        FOLDER "GVDBSamples/shaders")
endif()

# --- HLSL -------------------------------------------------------------------

file(GLOB sample_utils_shader_src "shaders/*.hlsl" "shaders/*.hlsli")
ethereal_compile_shaders(
    TARGET gvdb_sample_utils_shaders
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg"
    FOLDER "GVDBSamples/shaders"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_sample_utils"
    SOURCES ${sample_utils_shader_src}
    ALL_PLATFORMS
)

# --- host library -----------------------------------------------------------

add_library(gvdb_sample_utils STATIC
    SampleTypes.h
    SampleTypes.cpp
    ObjLoader.h
    ObjLoader.cpp
    ObjMesh.h
    ObjMesh.cpp
    VolRenderer.h
    VolRenderer.cpp
    ScreenPresenter.h
    ScreenPresenter.cpp
    DebugDraw.h
    DebugDraw.cpp
    GVDBApp.h
    GVDBApp.cpp
    HeadlessContext.h
    HeadlessContext.cpp
    ImageIO.h
    ImageIO.cpp
    kernels/GVDBScene.slangh
    kernels/GVDBRaycast.slangh
    kernels/GVDBRaycast.slang
)
set_source_files_properties(kernels/GVDBScene.slangh kernels/GVDBRaycast.slangh kernels/GVDBRaycast.slang
    PROPERTIES HEADER_FILE_ONLY ON)
if (NVRHI_WITH_OPTIX)
    target_sources(gvdb_sample_utils PRIVATE
        OptixRenderer.h
        OptixRenderer.cpp
        kernels/optix/OptixConstants.h
        kernels/optix/OptixPrograms.slang)
    set_source_files_properties(kernels/optix/OptixPrograms.slang PROPERTIES HEADER_FILE_ONLY ON)
    add_dependencies(gvdb_sample_utils gvdb_sample_utils_optix)
endif()
target_include_directories(gvdb_sample_utils
    PUBLIC "$<BUILD_INTERFACE:${GVDB_SAMPLES_INCLUDE_ROOT}>"
)
# PUBLIC gvdb: the headers include <gvdb/GVDB.h> / <gvdb/GVDBScene.h>.
target_link_libraries(gvdb_sample_utils PUBLIC gvdb donut_core donut_engine donut_app)
if (TARGET nvrhi_factory)
    target_link_libraries(gvdb_sample_utils PUBLIC nvrhi_factory)
endif()
target_compile_definitions(gvdb_sample_utils PUBLIC "GVDB_SAMPLES_ASSETS_DIR=\"${GVDB_SAMPLES_ASSETS_DIR}\"")
target_link_libraries(gvdb_sample_utils PRIVATE stb)
add_dependencies(gvdb_sample_utils gvdb_sample_utils_shaders)
if (TARGET gvdb_sample_utils_kernels)
    add_dependencies(gvdb_sample_utils gvdb_sample_utils_kernels)
endif()
set_target_properties(gvdb_sample_utils PROPERTIES FOLDER "GVDBSamples")
if(MSVC)
    target_compile_options(gvdb_sample_utils PRIVATE /W3 /MP)
endif()

# --- tools ------------------------------------------------------------------

add_executable(VBXConverter VBXConverter.cpp)
target_link_libraries(VBXConverter PRIVATE donut_core gvdb)
set_target_properties(VBXConverter PROPERTIES FOLDER "GVDBSamples")
