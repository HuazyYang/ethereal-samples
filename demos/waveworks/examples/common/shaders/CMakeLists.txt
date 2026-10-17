#
# Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.
#

set(bytecode_dir "${CMAKE_CURRENT_BINARY_DIR}/bytecode")
set(shader_config "${CMAKE_CURRENT_SOURCE_DIR}/Shaders.cfg")

shadertool_add_shader_objects(
    TARGET waveworks_shaders_bytecode_dx
    CONFIG_FILE "${shader_config}"
    BACKENDS DXBC DXIL
    OUTPUT_MODE HEADER_BLOB
    SHADER_MODEL 6_0
    OUTPUT_DIRECTORY "${bytecode_dir}"
    OUTPUT_VARIABLE EXAMPLE_DX_SHADER_HEADERS
    FOLDER "WaveWorks_example")

set(EXAMPLE_VK_SHADER_HEADERS)
if(DONUT_WITH_VULKAN)
    # The public helper has no stage filter. Derive build-only lists from the
    # shared config so Vulkan can retain NVRHI's graphics-stage binding ranges.
    file(STRINGS "${shader_config}" shader_rows REGEX "^[ \t]*[^# \t].*$")
    set(vk_targets)
    foreach(stage IN ITEMS vs hs ds ps)
        set(stage_config "")
        foreach(shader_row IN LISTS shader_rows)
            if(shader_row MATCHES "(^|[ \t])-T[ \t]+${stage}([ \t]|$)")
                string(STRIP "${shader_row}" shader_row)
                # Derived configs live in the build tree: make their source
                # paths absolute while retaining the row's compiler options.
                string(REGEX REPLACE "^([^ \t]+)"
                    "\"${CMAKE_CURRENT_SOURCE_DIR}/\\1\""
                    stage_row "${shader_row}")
                string(APPEND stage_config "${stage_row}\n")
            endif()
        endforeach()
        set(stage_config_path "${CMAKE_CURRENT_BINARY_DIR}/Shaders.${stage}.cfg")
        file(CONFIGURE OUTPUT "${stage_config_path}" CONTENT "${stage_config}" @ONLY)

        # PS starts at 2048; the unused GS range starts at 1536.
        if(stage STREQUAL vs)
            set(binding_base 0)
        elseif(stage STREQUAL hs)
            set(binding_base 512)
        elseif(stage STREQUAL ds)
            set(binding_base 1024)
        else()
            set(binding_base 2048)
        endif()
        math(EXPR descriptor_set "${binding_base} / 512")
        math(EXPR sampler_binding "${binding_base} + 128")
        math(EXPR constant_binding "${binding_base} + 256")
        math(EXPR storage_binding "${binding_base} + 384")
        shadertool_add_shader_objects(
            TARGET waveworks_shaders_bytecode_vk_${stage}
            CONFIG_FILE "${stage_config_path}"
            BACKENDS SPIRV
            OUTPUT_MODE HEADER_BLOB
            SHADER_MODEL 6_0
            SPIRV_TARGET_ENV vulkan1.0
            EXTRA_FLAGS_SPIRV -DWAVEWORKS_SPACE=space${descriptor_set} -fspv-entrypoint-name=main
            SPIRV_REGISTER_SHIFTS ${binding_base} ${sampler_binding}
                ${constant_binding} ${storage_binding}
            OUTPUT_DIRECTORY "${bytecode_dir}"
            OUTPUT_VARIABLE stage_headers
            FOLDER "WaveWorks_example"
            SOURCES "${shader_config}")
        list(APPEND EXAMPLE_VK_SHADER_HEADERS ${stage_headers})
        list(APPEND vk_targets waveworks_shaders_bytecode_vk_${stage})
    endforeach()
    add_custom_target(waveworks_shaders_bytecode_vk DEPENDS ${vk_targets})
endif()
set(EXAMPLE_DX_SHADER_HEADERS "${EXAMPLE_DX_SHADER_HEADERS}" PARENT_SCOPE)
set(EXAMPLE_VK_SHADER_HEADERS "${EXAMPLE_VK_SHADER_HEADERS}" PARENT_SCOPE)
