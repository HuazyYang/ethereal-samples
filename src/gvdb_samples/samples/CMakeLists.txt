# The samples. Runtime artifacts follow the project convention: everything in
# "${CMAKE_BINARY_DIR}/bin" (executables), "bin/ptx" (CUDA / OptiX modules) and
# "bin/shaders/<target>" (HLSL). Assets are read from GVDB_SAMPLES_ASSETS_DIR.

add_subdirectory(sample-utils)

# One directory per sample (kebab-case), each with its own CMakeLists.txt that
# calls gvdb_add_sample(). Discovered by glob so that samples can be added
# without touching this file.
file(GLOB sample_dirs LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/*")
list(SORT sample_dirs)
foreach(dir IN LISTS sample_dirs)
    if(dir STREQUAL "sample-utils" OR dir STREQUAL "assets")
        continue()
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endforeach()
