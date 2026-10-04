# nv_asteroids -- the 2018 NVIDIA Asteroids demo (D3D12 meshlets), reconstructed
# from the shipped binaries and ported to this tree's Donut/nvrhi.
#
# The demo needs data the reconstruction does not contain: the original's
# media.db and the third-party DLLs it shipped (PhysX 3.4.2 CHECKED, assimp 4.1,
# HBAO+ 4.0, nvToolsExt). NV_ASTEROIDS_ORIGINAL_DIR points at the installed
# original; the aggregate's ETHEREAL_BUILD_NV_ASTEROIDS therefore defaults to OFF.
#
# Targets:
#   nv_asteroids               the demo executable
#   nv_asteroids_core          its static library (everything but WinMain)
#   nv_asteroids_shaders       SM 6.5 meshlet amplification/mesh shaders
#   nv_asteroids_shaders_sm60  every other stage, at SM 6.0 (2018 min16float)
#   nv_asteroids_asset_check   loads every .chk LOD out of media.db, no device
#   NvVolumetricLighting.d3d11 the companion D3D11 DLL reconstruction

include(FetchContent)

set(NV_ASTEROIDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Statically linked third-party code that the 2018 executable embedded.
# SQLite version string in Asteroids.exe: "3.24.0", source id
# "2018-06-04 19:24:41 c7ee0833...".
# ---------------------------------------------------------------------------
FetchContent_Declare(nv_asteroids_sqlite3_src
    URL https://www.sqlite.org/2018/sqlite-amalgamation-3240000.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(nv_asteroids_lz4_src
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.8.3
    SOURCE_SUBDIR build/cmake/__none__)   # only lib/lz4.c is used
FetchContent_MakeAvailable(nv_asteroids_sqlite3_src nv_asteroids_lz4_src)

add_library(nv_asteroids_sqlite3 STATIC "${nv_asteroids_sqlite3_src_SOURCE_DIR}/sqlite3.c")
target_include_directories(nv_asteroids_sqlite3 PUBLIC "${nv_asteroids_sqlite3_src_SOURCE_DIR}")
target_compile_definitions(nv_asteroids_sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
set_target_properties(nv_asteroids_sqlite3 PROPERTIES FOLDER "Demos/nv_asteroids/thirdparty")

add_library(nv_asteroids_lz4 STATIC "${nv_asteroids_lz4_src_SOURCE_DIR}/lib/lz4.c")
target_include_directories(nv_asteroids_lz4 PUBLIC "${nv_asteroids_lz4_src_SOURCE_DIR}/lib")
set_target_properties(nv_asteroids_lz4 PROPERTIES FOLDER "Demos/nv_asteroids/thirdparty")

# PhysX / assimp / HBAO+ import libraries generated from the shipped DLLs.
include("${NV_ASTEROIDS_DIR}/cmake/third_party_dlls.cmake")

# ---------------------------------------------------------------------------
# Shaders (reconstructed HLSL -> DXIL through ShaderTool)
#
# Two targets, because ShaderTool takes the shader model per invocation: the
# meshlet amplification/mesh stages need 6.5, everything else stays at 6.0 so
# that DXC does not enable 16-bit types and `min16float` keeps its 2018
# semantics. Both write into one output directory, which is where the demo's
# ShaderFactory base path points.
# ---------------------------------------------------------------------------
set(nv_asteroids_shader_dir "${NV_ASTEROIDS_DIR}/shaders")
set(nv_asteroids_shader_output "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/nv_asteroids")

file(GLOB_RECURSE nv_asteroids_shader_sources CONFIGURE_DEPENDS
    "${nv_asteroids_shader_dir}/*.hlsl" "${nv_asteroids_shader_dir}/*.hlsli"
    "${nv_asteroids_shader_dir}/*.h")

# external/nvapi is on the include path for nvHLSLExtns.h, used by the 2018
# NVAPI mesh shaders in shaders/demo/nvapi.
set(nv_asteroids_shader_includes
    "${nv_asteroids_shader_dir}"
    "${nv_asteroids_shader_dir}/demo/include"
    "${NV_ASTEROIDS_DIR}/external/nvapi")

ethereal_compile_shaders(
    TARGET nv_asteroids_shaders
    CONFIG "${nv_asteroids_shader_dir}/shaders_sm65.cfg"
    OUTPUT_DIRECTORY "${nv_asteroids_shader_output}"
    FOLDER "Demos/nv_asteroids"
    BACKENDS DXIL
    SHADER_MODEL 6_5
    INCLUDE_DIRECTORIES ${nv_asteroids_shader_includes}
    SOURCES ${nv_asteroids_shader_sources})

ethereal_compile_shaders(
    TARGET nv_asteroids_shaders_sm60
    CONFIG "${nv_asteroids_shader_dir}/shaders_sm60.cfg"
    OUTPUT_DIRECTORY "${nv_asteroids_shader_output}"
    FOLDER "Demos/nv_asteroids"
    BACKENDS DXIL
    SHADER_MODEL 6_0
    INCLUDE_DIRECTORIES ${nv_asteroids_shader_includes})

# ---------------------------------------------------------------------------
# The reconstructed demo
# ---------------------------------------------------------------------------
file(GLOB_RECURSE nv_asteroids_src CONFIGURE_DEPENDS
    "${NV_ASTEROIDS_DIR}/src/*.cpp" "${NV_ASTEROIDS_DIR}/src/*.h")
list(FILTER nv_asteroids_src EXCLUDE REGEX "/src/app/main\\.cpp$")
file(GLOB nv_asteroids_rc CONFIGURE_DEPENDS "${NV_ASTEROIDS_DIR}/src/app/*.rc")

add_library(nv_asteroids_core STATIC ${nv_asteroids_src})
target_include_directories(nv_asteroids_core PUBLIC
    "${NV_ASTEROIDS_DIR}/src"
    "${nv_asteroids_shader_dir}/demo/include")
target_link_libraries(nv_asteroids_core PUBLIC
    donut_app donut_render donut_engine donut_core
    jsoncpp_static nv_asteroids_sqlite3 nv_asteroids_lz4 bcrypt xaudio2
    nv_asteroids_physx assimp GFSDK_SSAO_D3D12)
if (NVRHI_WITH_NVAPI)
    target_link_libraries(nv_asteroids_core PUBLIC nvapi)
    target_compile_definitions(nv_asteroids_core PUBLIC ASTEROIDS_WITH_NVAPI=1)
endif()
target_compile_definitions(nv_asteroids_core PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    ASTEROIDS_SHADER_DIR="${nv_asteroids_shader_output}/dxil"
    ASTEROIDS_DONUT_SHADER_DIR="${DONUT_SHADERS_OUTPUT_DIR}/dxil")
set_target_properties(nv_asteroids_core PROPERTIES FOLDER "Demos/nv_asteroids")
add_dependencies(nv_asteroids_core nv_asteroids_shaders nv_asteroids_shaders_sm60)

add_executable(nv_asteroids WIN32 "${NV_ASTEROIDS_DIR}/src/app/main.cpp" ${nv_asteroids_rc})
target_link_libraries(nv_asteroids PRIVATE nv_asteroids_core)
target_compile_definitions(nv_asteroids PRIVATE ASTEROIDS_MAIN_EXE=1)
set_target_properties(nv_asteroids PROPERTIES FOLDER "Demos/nv_asteroids")
add_dependencies(nv_asteroids donut_shaders)

# Runtime: the DLLs the original shipped, and media.db, next to the executable.
foreach(dll IN LISTS NV_ASTEROIDS_RUNTIME_DLLS)
    add_custom_command(TARGET nv_asteroids POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NV_ASTEROIDS_ORIGINAL_DIR}/${dll}.dll" "$<TARGET_FILE_DIR:nv_asteroids>")
endforeach()
add_custom_command(TARGET nv_asteroids POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NV_ASTEROIDS_ORIGINAL_DIR}/media.db" "$<TARGET_FILE_DIR:nv_asteroids>")

# Loads every .chk LOD out of media.db through the reconstructed reader; no device.
add_executable(nv_asteroids_asset_check "${NV_ASTEROIDS_DIR}/tools/asset_check.cpp")
target_link_libraries(nv_asteroids_asset_check PRIVATE nv_asteroids_core)
set_target_properties(nv_asteroids_asset_check PROPERTIES FOLDER "Demos/nv_asteroids")

# ---------------------------------------------------------------------------
# Companion reconstruction: NvVolumetricLighting.d3d11.dll. Independent of the
# demo (the D3D12 demo never calls it), kept here because it comes from the same
# 2018 title and shares its tooling.
# ---------------------------------------------------------------------------
add_subdirectory(nv_volumetric_lighting)
