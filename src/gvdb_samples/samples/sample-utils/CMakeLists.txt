# sample-utils: what the samples share on top of the gvdb library.
#   VolRenderer     CUDA raycaster over the volume instances of a GVDBSceneGraph
#   OptixRenderer   OptiX path tracer over volume + mesh instances (needs OptiX)
#   GVDBApp         Donut application base (devices, scene graph, camera/light
#                   orbit, screen presentation, debug lines, ImGui)
#   HeadlessContext console samples (devices + file system), ImageIO (PNG)
#   ObjMesh         OBJ -> Donut MeshInfo / MeshInstance and gp buffers

# --- CUDA render kernels ----------------------------------------------------

gvdb_add_ptx(gvdb_sample_utils_ptx
    SOURCES "kernels/GVDBRaycast.cu"
)
target_sources(gvdb_sample_utils_ptx_objects PRIVATE "kernels/GVDBRaycast.cuh" "kernels/GVDBScene.cuh")

if (ETHEREAL_OPTIX_FOUND)
    gvdb_add_ptx(gvdb_sample_utils_optix_ptx
        OPTIX
        SOURCES "kernels/optix/OptixPrograms.cu"
    )
    target_sources(gvdb_sample_utils_optix_ptx_objects PRIVATE "kernels/optix/OptixLaunchParams.h")
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
    OptixRenderer.h
    OptixRenderer.cpp
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
)
target_include_directories(gvdb_sample_utils
    PUBLIC "$<BUILD_INTERFACE:${GVDB_SAMPLES_INCLUDE_ROOT}>"
)
# PUBLIC gvdb: the headers include <gvdb/GVDB.h> / <gvdb/GVDBScene.h>.
target_link_libraries(gvdb_sample_utils PUBLIC gvdb gpdevice donut_core donut_engine donut_app)
target_compile_definitions(gvdb_sample_utils PUBLIC "GVDB_SAMPLES_ASSETS_DIR=\"${GVDB_SAMPLES_ASSETS_DIR}\"")
target_include_directories(gvdb_sample_utils PRIVATE "${ETHEREAL_DONUT_DIR}/thirdparty/stb")
add_dependencies(gvdb_sample_utils gvdb_sample_utils_ptx gvdb_sample_utils_shaders)
if (TARGET gvdb_sample_utils_optix_ptx)
    add_dependencies(gvdb_sample_utils gvdb_sample_utils_optix_ptx)
endif()
set_target_properties(gvdb_sample_utils PROPERTIES FOLDER "GVDBSamples")
if(MSVC)
    target_compile_options(gvdb_sample_utils PRIVATE /W3 /MP)
endif()

# --- tools ------------------------------------------------------------------

add_executable(VBXConverter VBXConverter.cpp)
target_link_libraries(VBXConverter PRIVATE donut_core gvdb)
set_target_properties(VBXConverter PROPERTIES FOLDER "GVDBSamples")
