if(NOT TARGET gvdb_scene_assets)
    add_library(gvdb_scene_assets STATIC "${CMAKE_CURRENT_LIST_DIR}/SceneAssets.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/ObjLoader.cpp")
    target_link_libraries(gvdb_scene_assets PUBLIC donut_core stb)
    target_include_directories(gvdb_scene_assets PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../..")
endif()
