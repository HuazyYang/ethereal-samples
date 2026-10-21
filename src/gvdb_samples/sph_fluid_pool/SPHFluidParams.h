#ifndef SPH_FLUID_PARAMS_H
#define SPH_FLUID_PARAMS_H
// Layouts shared by the portable SPH host code and Slang shader parameters.
// The shader uses matching scalar members; host vector types are 12 bytes with
// 4-byte alignment, so both sides have the same layout.
#include <donut/core/math/math.h>
#include <cstdint>
typedef dm::float3 sph_f3;
typedef dm::int3 sph_i3;

#define SPH_MAX_GRID_ADJ 216        // neighbour cells: search width 6 at most (6^3)

// Particle buffer slots (FPOS .. FGNDX of the reference).
#define SPH_FPOS 0     // float3 position, world units
#define SPH_FVEL 1     // float3 velocity (leapfrog half step)
#define SPH_FCLR 2     // uint   packed RGBA8 colour
#define SPH_FVEVAL 3   // float3 velocity used to evaluate forces
#define SPH_FFORCE 4   // float3 force
#define SPH_FPRESS 5   // float  density (the reference calls it pressure)
#define SPH_FGCELL 6   // int    grid cell, SPH_GRID_UNDEF when outside
#define SPH_FGNDX 7    // int    index inside the grid cell
#define SPH_PARTICLE_BUFFERS 8

// Acceleration grid buffer slots (AGRID .. AAUXSCAN2 of the reference).
#define SPH_AGRID 0        // uint per particle: sorted particle index
#define SPH_AGRIDCNT 1     // uint per cell: particle count
#define SPH_AGRIDOFF 2     // uint per cell: first particle (prefix sum of the counts)
#define SPH_AAUXARRAY1 3   // prefix sum scratch, level 2
#define SPH_AAUXSCAN1 4
#define SPH_AAUXARRAY2 5   // prefix sum scratch, level 3
#define SPH_AAUXSCAN2 6
#define SPH_GRID_BUFFERS 7

// Simulation parameters (FParams_t of the reference; unused members removed,
// names kept). Units: positions in world units, physics in meters through
// sim_scale.
struct SPHFluidParams {
    int example;                 // scene preset (SPHExample)
    float time, dt, sim_scale;

    int numThreads, numBlocks;   // particle kernel launch dimensions
    int pnum;                    // particle capacity

    float pdist, pmass, prest_dens, pintstiff;
    float pradius, psmoothradius, pspacing, r2, pvisc;

    float AL, AL2, VL, VL2;      // acceleration / velocity limits and their squares
    float d2, rd2, vterm;        // used in the force calculation
    float poly6kern, spikykern, lapkern, gausskern;

    float grid_size, grid_density;
    sph_f3 gridSize, gridDelta, gridMin, gridMax;
    sph_i3 gridRes, gridScanMax;
    int gridSrch, gridTotal, gridAdjCnt;
    int gridAdj[SPH_MAX_GRID_ADJ];

    float bound_slope, bound_stiff, bound_friction, bound_damp;
    float bound_wall_force, bound_wall_freq;
    sph_f3 bound_min, bound_max;
    sph_f3 init_min, init_max;

    sph_f3 grav_dir, grav_pos, gravity;
    float grav_amt;
};

// Portable parameter block size; shader and host layouts must remain identical.
static_assert(sizeof(sph_f3) == 12 && sizeof(sph_i3) == 12, "SPH: vector types must be 12 bytes");
static_assert(sizeof(SPHFluidParams) == 1176, "SPH: parameter block layout differs between host and device");

#endif /* SPH_FLUID_PARAMS_H */
