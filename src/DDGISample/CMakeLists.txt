file(GLOB shaders "shaders/*.hlsl" "shaders/*.hlsli")
file(GLOB sources "*.cpp" "*.h" "ddgi/*.h" "ddgi/*.cpp")

set(project DDGISample)
set(folder "DDGISample")

list(APPEND sources "ddgi/DDGIVolume.h" "ddgi/DDGIVolume.cpp")

ethereal_compile_shaders(
    TARGET ${project}_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/shaders ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ddgi
    SOURCES ${shaders}
    FOLDER ${folder}
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_render donut_app donut_engine)
add_dependencies(${project} ${project}_shaders)
ethereal_use_agility_sdk(${project})
set_target_properties(${project} PROPERTIES FOLDER ${folder})
