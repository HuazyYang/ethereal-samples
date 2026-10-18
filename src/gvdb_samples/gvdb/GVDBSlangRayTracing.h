// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GVDBSlangVolume.h"
#include <filesystem>
#include <nvrhi/shader-program.h>
namespace gvdb::slang
{
// Runtime and volume must outlive the renderer. The existing volume buffers are
// reused; atlas changes are visible without duplicate topology or atlas uploads.
class SlangVolumeRT
{
    ethereal::slang::ComputeRuntime *m_runtime;
    SlangVolume *m_volume;
    nvrhi::ShaderProgramHandle m_program;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BindingLayoutDesc m_layoutDesc;
    nvrhi::rt::PipelineHandle m_pipeline;
    nvrhi::rt::ShaderTableHandle m_tables[2];
    nvrhi::BufferHandle m_bounds;
    nvrhi::rt::AccelStructHandle m_blas, m_tlas;
    nvrhi::CommandListHandle m_commands;
    bool m_topBuilt = false;
    void submit();

  public:
    SlangVolumeRT(ethereal::slang::ComputeRuntime &runtime, SlangVolume &volume, const std::filesystem::path &artifact);
    // Camera/light are world-space; step size and extinction remain index-space.
    RaycastOutput render(RaycastParameters parameters, const std::vector<Float4> &transfer, bool surface = false,
                         const nvrhi::rt::AffineTransform &transform = nvrhi::rt::c_IdentityTransform);
};
} // namespace gvdb::slang
