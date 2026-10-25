# gFluidSurface: SPH fluid (FluidSystemKernels.slang, 1.5M particles) surfaced through GVDB
# (points -> level set) every frame and path traced with OptiX. Skipped without OptiX.
gvdb_add_sample(fluid-surface OPTIX SOURCES main.cpp FluidSystem.h FluidSystem.cpp KERNELS FluidSystemKernels.slang)
