# WaveWorks SDK: C++, ISPC and HLSL sources only. No packaged SDK libraries.
find_program(WAVEWORKS_ISPC NAMES ispc REQUIRED)
set(algebra_dir "${CMAKE_CURRENT_BINARY_DIR}/algebra")
set(algebra_object "${algebra_dir}/BasicAlgebra.obj")
set(algebra_header "${algebra_dir}/BasicAlgebra_ispc.h")
add_custom_command(OUTPUT "${algebra_object}" "${algebra_header}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${algebra_dir}"
    COMMAND "${WAVEWORKS_ISPC}" "${CMAKE_CURRENT_SOURCE_DIR}/src/BasicAlgebra.ispc"
        --arch=x86-64 --target=sse2-i32x4 --target-os=windows -O2
        -o "${algebra_object}" -h "${algebra_header}"
    DEPENDS src/BasicAlgebra.ispc VERBATIM)
set_source_files_properties("${algebra_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

shadertool_add_shader_objects(
    TARGET waveworks_sdk_shaders
    CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/Shaders.cfg"
    BACKENDS DXBC SPIRV OUTPUT_MODE HEADER_BLOB SHADER_MODEL 6_0
    SPIRV_TARGET_ENV vulkan1.0
    EXTRA_FLAGS_SPIRV -fspv-entrypoint-name=main
    SPIRV_REGISTER_SHIFTS 0 128 256 384
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    OUTPUT_VARIABLE waveworks_shader_headers)
file(GLOB sdk_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_library(waveworks_sdk SHARED ${sdk_sources} src/ShaderBytecode.c
    "${algebra_object}" "${algebra_header}" ${waveworks_shader_headers})
target_compile_features(waveworks_sdk PUBLIC cxx_std_17)
target_compile_definitions(waveworks_sdk INTERFACE WAVEWORKS_DYNAMIC_BUILD=1)
target_compile_definitions(waveworks_sdk PRIVATE GFSDK_WAVEWORKS_BUILDING_DLL=1
    WAVEWORKS_DYNAMIC_BUILD=1 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
target_include_directories(waveworks_sdk PUBLIC include PRIVATE src "${algebra_dir}"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_link_libraries(waveworks_sdk PRIVATE nvrhi nvrhi_d3d11 nvrhi_d3d12 nvrhi_vk Vulkan::Headers)
set_target_properties(waveworks_sdk PROPERTIES OUTPUT_NAME WaveWorks FOLDER Demos/WaveWorks)
set_property(SOURCE src/ShaderBytecode.c APPEND PROPERTY OBJECT_DEPENDS "${waveworks_shader_headers}")
add_dependencies(waveworks_sdk waveworks_sdk_shaders)
