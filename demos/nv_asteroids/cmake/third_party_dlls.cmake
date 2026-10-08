# Prebuilt third-party DLLs shipped with the original demo (excluded from the
# reconstruction): headers come from the public upstream repositories (sparse,
# header-only checkouts) and import libraries are generated from the DLL export
# tables (tools/dll2def.py + lib.exe).

set(NV_ASTEROIDS_ORIGINAL_DIR "" CACHE PATH
    "Directory of the installed original 2018 demo: its prebuilt DLLs and media.db")
if (NOT NV_ASTEROIDS_ORIGINAL_DIR OR NOT EXISTS "${NV_ASTEROIDS_ORIGINAL_DIR}/media.db")
    message(FATAL_ERROR
        "nv_asteroids needs the original demo's media.db and the DLLs it shipped. Point "
        "NV_ASTEROIDS_ORIGINAL_DIR at the directory that holds Asteroids.exe and media.db, "
        "or configure with -DETHEREAL_BUILD_NV_ASTEROIDS=OFF.")
endif()

# lib.exe turns the generated .def files into import libraries. CMAKE_AR is
# lib.exe under MSVC; look it up when the toolchain leaves it unset.
set(_nv_asteroids_lib_exe "${CMAKE_AR}")
if (NOT _nv_asteroids_lib_exe MATCHES "lib[.]exe$")
    find_program(NV_ASTEROIDS_LIB_EXE NAMES lib
        HINTS "$ENV{VCToolsInstallDir}/bin/Hostx64/x64" REQUIRED)
    set(_nv_asteroids_lib_exe "${NV_ASTEROIDS_LIB_EXE}")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_nv_asteroids_deps_dir "${CMAKE_BINARY_DIR}/_nv_asteroids_deps")

# nv_asteroids_import_dll(<target> <dll-name-without-extension> <include-dirs>...)
# Creates an IMPORTED SHARED target backed by an import library generated from the DLL.
function(nv_asteroids_import_dll target dllname)
    set(dll "${NV_ASTEROIDS_ORIGINAL_DIR}/${dllname}.dll")
    if (NOT EXISTS "${dll}")
        message(FATAL_ERROR "Missing ${dll} (set NV_ASTEROIDS_ORIGINAL_DIR)")
    endif()
    set(def "${_nv_asteroids_deps_dir}/implibs/${dllname}.def")
    set(lib "${_nv_asteroids_deps_dir}/implibs/${dllname}.lib")
    if (NOT EXISTS "${lib}" OR "${dll}" IS_NEWER_THAN "${lib}")
        file(MAKE_DIRECTORY "${_nv_asteroids_deps_dir}/implibs")
        execute_process(COMMAND ${Python3_EXECUTABLE} "${NV_ASTEROIDS_DIR}/tools/dll2def.py" "${dll}" "${def}"
            RESULT_VARIABLE res OUTPUT_QUIET)
        execute_process(COMMAND "${_nv_asteroids_lib_exe}" /nologo /def:${def} /machine:x64 /out:${lib}
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
epm_add_asset(
    NAME physx34
    GITHUB_REPOSITORY NVIDIAGameWorks/PhysX-3.4
    GIT_TAG v3.4.2
    SPARSE_PATHS /PhysX_3.4/Include/ /PxShared/include/)
set(_physx_inc "${physx34_DIR}/PhysX_3.4/Include;${physx34_DIR}/PxShared/include")
nv_asteroids_import_dll(PxFoundation PxFoundationCHECKED_x64 ${_physx_inc})
nv_asteroids_import_dll(PxPvdSDK PxPvdSDKCHECKED_x64 ${_physx_inc})
nv_asteroids_import_dll(PhysX3Common PhysX3CommonCHECKED_x64 ${_physx_inc})
nv_asteroids_import_dll(PhysX3 PhysX3CHECKED_x64 ${_physx_inc})
nv_asteroids_import_dll(PhysX3Cooking PhysX3CookingCHECKED_x64 ${_physx_inc})
nv_asteroids_import_dll(PhysX3CharacterKinematic PhysX3CharacterKinematicCHECKED_x64 ${_physx_inc})
add_library(nv_asteroids_physx INTERFACE)
target_link_libraries(nv_asteroids_physx INTERFACE
    PxFoundation PxPvdSDK PhysX3Common PhysX3 PhysX3Cooking PhysX3CharacterKinematic)
target_compile_definitions(nv_asteroids_physx INTERFACE PX_CHECKED=1 PX_PHYSX_STATIC_LIB=0)

# --- assimp (C API only) ---
epm_add_asset(
    NAME assimp41
    GITHUB_REPOSITORY assimp/assimp
    GIT_TAG v4.1.0
    SPARSE_PATHS /include/)
if (EXISTS "${assimp41_DIR}/include/assimp/config.h.in")
    # single precision (ASSIMP_DOUBLE_PRECISION unset), as in the shipped DLL
    configure_file("${assimp41_DIR}/include/assimp/config.h.in" "${assimp41_DIR}/include/assimp/config.h")
endif()
nv_asteroids_import_dll(assimp assimp-vc140-mt "${assimp41_DIR}/include")

# --- HBAO+ 4.0 (D3D12) ---
epm_add_asset(
    NAME hbaoplus
    GITHUB_REPOSITORY NVIDIAGameWorks/HBAOPlus
    GIT_TAG master
    SPARSE_PATHS /include/)
nv_asteroids_import_dll(GFSDK_SSAO_D3D12 GFSDK_SSAO_D3D12.win64 "${hbaoplus_DIR}/include")

# DLLs to copy next to the reconstructed executable
set(NV_ASTEROIDS_RUNTIME_DLLS
    PxFoundationCHECKED_x64 PxPvdSDKCHECKED_x64 PhysX3CommonCHECKED_x64 PhysX3CHECKED_x64
    PhysX3CookingCHECKED_x64 PhysX3CharacterKinematicCHECKED_x64
    assimp-vc140-mt GFSDK_SSAO_D3D12.win64 nvToolsExt64_1)
