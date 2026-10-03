# gFluidSurface: SPH fluid (FluidSystemKernels.cu, 1.5M particles) surfaced through GVDB
# (points -> level set) every frame and path traced with OptiX. Skipped without OptiX.
# The fluid kernels are a plain CUDA module (not OptiX device code), so they are
# built with their own gvdb_add_ptx() instead of the PTX option of gvdb_add_sample().
gvdb_add_sample(fluid-surface OPTIX SOURCES main.cpp FluidSystem.h FluidSystem.cpp)
if(TARGET fluid-surface)
    gvdb_add_ptx(fluid-surface_ptx SOURCES FluidSystemKernels.cu)
    target_sources(fluid-surface_ptx_objects PRIVATE FluidSystem.h)
    add_dependencies(fluid-surface fluid-surface_ptx)
endif()
