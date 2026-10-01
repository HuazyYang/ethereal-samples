
add_library(
    sample_utils_ptx
    OBJECT
    "kernels/cuda_gvdb_particles.cu"
    "kernels/cuda_gvdb_raycast.cuh"
    "kernels/cuda_gvdb_raycast.cu"
    "kernels/cuda_gvdb_scene.cuh"
)
set_target_properties(sample_utils_ptx PROPERTIES
    CUDA_PTX_COMPILATION ON
    CUDA_ARCHITECTURES "75"
     COMPILE_FLAGS  "-Xcompiler \"/wd 4819\""
)
target_compile_options(sample_utils_ptx PRIVATE --use_fast_math)
target_include_directories(sample_utils_ptx
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../..>")

file(GLOB sample_utils_shader_src "shaders/*.hlsl" "shaders/*.hlsli")

ethereal_compile_shaders(
    TARGET sample_uitls_shaderlib
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Shaders.cfg"
    FOLDER "shaders"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/compiled_shaders"
    SOURCES ${sample_utils_shader_src}
    ALL_PLATFORMS
)

add_library(sample_utils STATIC
    SampleTypes.h
    SampleTypes.cpp
    Camera.h
    ObjLoader.h
    ObjLoader.cpp
    Scene.h
    Scene.cpp
    VolRenderer.h
    VolRenderer.cpp
    Voxelizer.h
    Voxelizer.cpp
)

# PUBLIC gvdb: the public header VolRenderer.h includes <gvdb/GVDB.h>, so gvdb
# (and transitively gpdevice) must propagate to consumers of sample_utils.
target_link_libraries(sample_utils PUBLIC gvdb gpdevice donut_core)
target_include_directories(sample_utils
    PUBLIC      "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>"
)
add_dependencies(sample_utils sample_utils_ptx)

add_executable(VBXConverter
    VBXConverter.cpp
)
target_link_libraries(VBXConverter donut_core gvdb)