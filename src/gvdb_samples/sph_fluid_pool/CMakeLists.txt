# sph_fluid_pool: port of fluids5.0 (SPH fluid, "wave pool" scene). The fluid
# is simulated by CUDA kernels through the gp abstraction (ISPHParticles +
# ISPHSolver, SPHFluid.h) and drawn as points by nvrhi from vertex buffers
# shared with the compute device. Uses the sample framework of the GVDB port
# (GVDBApp, scene graph, ImGui, --screenshot).
gvdb_add_sample(sph_fluid_pool
    SOURCES
        main.cpp
        SPHFluid.h
        SPHFluidParams.h
        SPHFluidScene.h
        SPHFluidScene.cpp
        SPHParticles.cpp
        SPHSolver.cpp
    PTX kernels/SPHFluidKernels.cu
    SHADERS "${CMAKE_CURRENT_LIST_DIR}/shaders/shaders.cfg")
target_include_directories(sph_fluid_pool PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
