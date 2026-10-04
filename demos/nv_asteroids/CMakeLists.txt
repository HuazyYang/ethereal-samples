include(FetchContent)

# Statically linked third-party code that the 2018 executable embedded.
# SQLite version string in Asteroids.exe: "3.24.0", source id "2018-06-04 19:24:41 c7ee0833...".
FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2018/sqlite-amalgamation-3240000.zip
)
FetchContent_Declare(lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.8.3
    SOURCE_SUBDIR build/cmake/__none__   # only lib/lz4.c is used
)
FetchContent_MakeAvailable(sqlite3 lz4)

add_library(asteroids_sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(asteroids_sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(asteroids_sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

add_library(asteroids_lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
target_include_directories(asteroids_lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)

set(ASTEROIDS_BIN_DIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Output directory of the Asteroids executable")

# ---------------------------------------------------------------------------
# Shaders (reconstructed HLSL, compiled to DXIL with ShaderMake)
# ---------------------------------------------------------------------------
set(asteroids_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(asteroids_shader_output "${ASTEROIDS_BIN_DIR}/shaders/asteroids")

file(GLOB_RECURSE asteroids_shader_cfgs CONFIGURE_DEPENDS "${asteroids_shader_dir}/*.cfg")
file(GLOB_RECURSE asteroids_shader_sources CONFIGURE_DEPENDS
    "${asteroids_shader_dir}/*.hlsl" "${asteroids_shader_dir}/*.hlsli" "${asteroids_shader_dir}/*.h")

if (asteroids_shader_cfgs)
    # Concatenate the per-group config fragments into one ShaderMake config.
    set(asteroids_shader_cfg "${CMAKE_CURRENT_BINARY_DIR}/AsteroidsShaders.cfg")
    set(cfg_text "")
    foreach(cfg IN LISTS asteroids_shader_cfgs)
        file(READ "${cfg}" text)
        string(APPEND cfg_text "${text}\n")
    endforeach()
    file(CONFIGURE OUTPUT "${asteroids_shader_cfg}" CONTENT "${cfg_text}" @ONLY)

    include(${CMAKE_SOURCE_DIR}/external/donut/compileshaders.cmake)
    donut_compile_shaders(
        TARGET asteroids_shaders
        PROJECT_NAME "Asteroids"
        CONFIG "${asteroids_shader_cfg}"
        FOLDER "Asteroids"
        DXIL "${asteroids_shader_output}/dxil"
        OUTPUT_FORMAT BINARY
        SHADER_MODEL 6_5
        SOURCES ${asteroids_shader_sources}
        INCLUDES "${asteroids_shader_dir}" "${asteroids_shader_dir}/demo/include"
                 "${CMAKE_SOURCE_DIR}/external/donut/include"
                 "${CMAKE_SOURCE_DIR}/external/nvapi"   # nvHLSLExtns.h for the 2018 NVAPI mesh shaders (demo/nvapi)
        IGNORE_INCLUDES "cstddef"
        SHADERMAKE_OPTIONS "--sourceDir ${asteroids_shader_dir}")
endif()

# ---------------------------------------------------------------------------
# Asteroids demo (reconstructed)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE asteroids_src CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)
list(FILTER asteroids_src EXCLUDE REGEX "/src/app/main\\.cpp$")
file(GLOB asteroids_rc CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.rc)

add_library(asteroids_core STATIC ${asteroids_src})
target_include_directories(asteroids_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${asteroids_shader_dir}/demo/include)
target_link_libraries(asteroids_core PUBLIC
    donut_app donut_render donut_engine donut_core
    jsoncpp_static asteroids_sqlite3 asteroids_lz4 bcrypt xaudio2
    asteroids_physx assimp GFSDK_SSAO_D3D12)
if (NVRHI_WITH_NVAPI)
    target_link_libraries(asteroids_core PUBLIC nvapi)
    target_compile_definitions(asteroids_core PUBLIC ASTEROIDS_WITH_NVAPI=1)
endif()
target_compile_definitions(asteroids_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    ASTEROIDS_SHADER_DIR="${asteroids_shader_output}/dxil"
    ASTEROIDS_DONUT_SHADER_DIR="${DONUT_SHADERS_OUTPUT_DIR}/dxil")
set_target_properties(asteroids_core PROPERTIES FOLDER "Asteroids")

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
    add_executable(Asteroids WIN32 src/app/main.cpp ${asteroids_rc})
    target_link_libraries(Asteroids PRIVATE asteroids_core)
    target_compile_definitions(Asteroids PRIVATE ASTEROIDS_MAIN_EXE=1)
    set_target_properties(Asteroids PROPERTIES FOLDER "Asteroids"
        RUNTIME_OUTPUT_DIRECTORY "${ASTEROIDS_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${ASTEROIDS_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ASTEROIDS_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ASTEROIDS_BIN_DIR}")
    add_dependencies(Asteroids donut_shaders)
    if (TARGET asteroids_shaders)
        add_dependencies(Asteroids asteroids_shaders)
    endif()

    # Runtime: prebuilt third-party DLLs and the original media.db next to the executable.
    foreach(dll IN LISTS ASTEROIDS_RUNTIME_DLLS)
        add_custom_command(TARGET Asteroids POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASTEROIDS_ORIGINAL_DIR}/${dll}.dll" "$<TARGET_FILE_DIR:Asteroids>")
    endforeach()
    add_custom_command(TARGET Asteroids POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASTEROIDS_ORIGINAL_DIR}/media.db" "$<TARGET_FILE_DIR:Asteroids>")
endif()

add_executable(asteroids_asset_check tools/asset_check.cpp)
target_link_libraries(asteroids_asset_check PRIVATE asteroids_core)
set_target_properties(asteroids_asset_check PROPERTIES FOLDER "Asteroids")
