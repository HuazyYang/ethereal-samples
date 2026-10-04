# Prebuilt third-party DLLs shipped with the original demo (excluded from the
# reconstruction): headers come from the public upstream repositories (sparse,
# header-only checkouts) and import libraries are generated from the DLL export
# tables (tools/dll2def.py + lib.exe).

set(ASTEROIDS_ORIGINAL_DIR "${CMAKE_SOURCE_DIR}/../asteroids" CACHE PATH
    "Directory of the original demo (prebuilt DLLs, media.db)")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Git REQUIRED)

set(_asteroids_deps_dir "${CMAKE_BINARY_DIR}/_asteroids_deps")

# asteroids_fetch_sparse(<name> <repo-url> <ref> <path>...)
function(asteroids_fetch_sparse name url ref)
    set(dir "${_asteroids_deps_dir}/${name}")
    set(${name}_SOURCE_DIR "${dir}" PARENT_SCOPE)
    if (EXISTS "${dir}/.fetched-${ref}")
        return()
    endif()
    file(REMOVE_RECURSE "${dir}")
    message(STATUS "Fetching ${name} (${ref}) headers from ${url}")
    execute_process(COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none --no-checkout --depth 1 --branch ${ref} ${url} "${dir}"
        RESULT_VARIABLE res)
    if (NOT res EQUAL 0)
        message(FATAL_ERROR "Failed to clone ${url}")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${dir}" sparse-checkout set --no-cone ${ARGN} RESULT_VARIABLE res)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C "${dir}" checkout RESULT_VARIABLE res2)
    if (NOT res EQUAL 0 OR NOT res2 EQUAL 0)
        message(FATAL_ERROR "Failed to check out ${name}")
    endif()
    file(TOUCH "${dir}/.fetched-${ref}")
endfunction()

# asteroids_import_dll(<target> <dll-name-without-extension> <include-dirs>...)
# Creates an IMPORTED SHARED target backed by an import library generated from the DLL.
function(asteroids_import_dll target dllname)
    set(dll "${ASTEROIDS_ORIGINAL_DIR}/${dllname}.dll")
    if (NOT EXISTS "${dll}")
        message(FATAL_ERROR "Missing ${dll} (set ASTEROIDS_ORIGINAL_DIR)")
    endif()
    set(def "${_asteroids_deps_dir}/implibs/${dllname}.def")
    set(lib "${_asteroids_deps_dir}/implibs/${dllname}.lib")
    if (NOT EXISTS "${lib}" OR "${dll}" IS_NEWER_THAN "${lib}")
        file(MAKE_DIRECTORY "${_asteroids_deps_dir}/implibs")
        execute_process(COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/dll2def.py" "${dll}" "${def}"
            RESULT_VARIABLE res OUTPUT_QUIET)
        execute_process(COMMAND "${CMAKE_AR}" /nologo /def:${def} /machine:x64 /out:${lib}
            RESULT_VARIABLE res2 OUTPUT_QUIET)
        if (NOT res EQUAL 0 OR NOT res2 EQUAL 0)
            message(FATAL_ERROR "Failed to create the import library for ${dllname}")
        endif()
    endif()
    add_library(${target} SHARED IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES IMPORTED_IMPLIB "${lib}" IMPORTED_LOCATION "${dll}")
    if (ARGN)
        set_target_properties(${target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ARGN}")
    endif()
endfunction()

# --- PhysX 3.4.2 (CHECKED DLLs) ---
asteroids_fetch_sparse(physx34 https://github.com/NVIDIAGameWorks/PhysX-3.4.git v3.4.2
    /PhysX_3.4/Include/ /PxShared/include/)
set(_physx_inc "${physx34_SOURCE_DIR}/PhysX_3.4/Include;${physx34_SOURCE_DIR}/PxShared/include")
asteroids_import_dll(PxFoundation PxFoundationCHECKED_x64 ${_physx_inc})
asteroids_import_dll(PxPvdSDK PxPvdSDKCHECKED_x64 ${_physx_inc})
asteroids_import_dll(PhysX3Common PhysX3CommonCHECKED_x64 ${_physx_inc})
asteroids_import_dll(PhysX3 PhysX3CHECKED_x64 ${_physx_inc})
asteroids_import_dll(PhysX3Cooking PhysX3CookingCHECKED_x64 ${_physx_inc})
asteroids_import_dll(PhysX3CharacterKinematic PhysX3CharacterKinematicCHECKED_x64 ${_physx_inc})
add_library(asteroids_physx INTERFACE)
target_link_libraries(asteroids_physx INTERFACE
    PxFoundation PxPvdSDK PhysX3Common PhysX3 PhysX3Cooking PhysX3CharacterKinematic)
target_compile_definitions(asteroids_physx INTERFACE PX_CHECKED=1 PX_PHYSX_STATIC_LIB=0)

# --- assimp (C API only) ---
asteroids_fetch_sparse(assimp41 https://github.com/assimp/assimp.git v4.1.0 /include/)
if (EXISTS "${assimp41_SOURCE_DIR}/include/assimp/config.h.in")
    # single precision (ASSIMP_DOUBLE_PRECISION unset), as in the shipped DLL
    configure_file("${assimp41_SOURCE_DIR}/include/assimp/config.h.in" "${assimp41_SOURCE_DIR}/include/assimp/config.h")
endif()
asteroids_import_dll(assimp assimp-vc140-mt "${assimp41_SOURCE_DIR}/include")

# --- HBAO+ 4.0 (D3D12) ---
asteroids_fetch_sparse(hbaoplus https://github.com/NVIDIAGameWorks/HBAOPlus.git master /include/)
asteroids_import_dll(GFSDK_SSAO_D3D12 GFSDK_SSAO_D3D12.win64 "${hbaoplus_SOURCE_DIR}/include")

# DLLs to copy next to the reconstructed executable
set(ASTEROIDS_RUNTIME_DLLS
    PxFoundationCHECKED_x64 PxPvdSDKCHECKED_x64 PhysX3CommonCHECKED_x64 PhysX3CHECKED_x64
    PhysX3CookingCHECKED_x64 PhysX3CharacterKinematicCHECKED_x64
    assimp-vc140-mt GFSDK_SSAO_D3D12.win64 nvToolsExt64_1)
