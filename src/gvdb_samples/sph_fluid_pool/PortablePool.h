// Portable authoring for the fluids5.0 SPH presets.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SPHFluidParams.h"
#include <gvdb/GVDBSlangSph.h>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>
namespace sph {
enum class SPHExample {Regression=0,Tower=1,WavePool=2,DamBreak=3};
inline void setupDefaultParams(SPHFluidParams &params) {
    //  Range = +/- 10.0 * 0.006 (r) =     0.12          m (= 120 mm = 4.7 inch)
    //  Container Volume (Vc) =            0.001728      m^3
    //  Rest Density (D) =              1000.0           kg / m^3
    //  Particle Mass (Pm) =               0.00020543    kg                      (mass = vol * density)
    //  Number of Particles (N) =       4000.0
    //  Water Mass (M) =                   0.821         kg (= 821 grams)
    //  Water Volume (V) =                 0.000821      m^3 (= 3.4 cups, .21 gals)
    //  Smoothing Radius (R) =             0.02          m (= 20 mm = ~3/4 inch)
    //  Particle Radius (Pr) =             0.00366       m (= 4 mm  = ~1/8 inch)
    //  Particle Volume (Pv) =             2.054e-7      m^3 (= .268 milliliters)
    //  Rest Distance (Pd) =               0.0059        m
    //
    // Ideal grid cell size (gs) = 2 * smoothing radius = 0.02*2 = 0.04
    // Ideal domain size = k*gs/d = k*0.02*2/0.005 = k*8 = {8, 16, 24, 32, 40, 48, ..}
    //    (k = number of cells, gs = cell size, d = simulation scale)
    params = SPHFluidParams{};

    // Set by Particles::Initialize in the reference.
    params.example = int(SPHExample::WavePool);
    params.grid_density = 2.0f;
    params.pnum = 65536;

    params.time = 0;
    params.dt = 0.004f;
    params.sim_scale = 0.006f;       // unit size
    params.pvisc = 0.10f;            // pascal-second (Pa.s) = 1 kg m^-1 s^-1
    params.prest_dens = 200.0f;      // kg / m^3
    params.pspacing = 0.0f;          // 0 = derived from the rest density
    params.pmass = 0.00020543f;      // kg
    params.pradius = 0.015f;         // m
    params.pdist = 0.0059f;          // m
    params.psmoothradius = 0.015f;   // m
    params.pintstiff = 1.5f;
    params.bound_stiff = 50000.0f;   // boundary stiffness
    params.bound_damp = 150.0f;
    params.AL = 150.0f;              // accel limit, m / s^2
    params.VL = 50.0f;               // vel limit, m / s

    params.bound_slope = 0.0f;       // ground slope
    params.bound_friction = 0.0f;    // ground friction
    params.bound_wall_force = 0.0f;
    params.grav_amt = 1.0f;

    params.grav_pos = sph_f3(0.f, 0.f, 0.f);
    params.grav_dir = sph_f3(0.f, -4.8f, 0.f);

    // Default sim config
    params.gridSize = sph_f3(params.psmoothradius * 2);
}

inline void setupExampleParams(SPHFluidParams &params, SPHExample example) {
    params.example = int(example);
    switch (example) {
        case SPHExample::Regression: {   // Regression test. N x N x N static grid
            int k = int(ceilf(powf(float(params.pnum), 1.0f / 3.0f)));
            params.bound_min = sph_f3(0.f, 0.f, 0.f);
            params.bound_max = sph_f3(2.0f + (k / 2), 2.0f + (k / 2), 2.0f + (k / 2));
            params.init_min = sph_f3(1.0f, 1.0f, 1.0f);
            params.init_max = sph_f3(1.0f + (k / 2), 1.0f + (k / 2), 1.0f + (k / 2));

            params.grav_amt = 0.0f;
            params.grav_dir = sph_f3(0.f, 0.f, 0.f);
            params.pspacing = 0.5f;                      // fixed spacing
            params.psmoothradius = params.pspacing;      // search radius
        } break;
        case SPHExample::Tower:
            params.bound_min = sph_f3(0.f, 0.f, 0.f);
            params.bound_max = sph_f3(256.f, 128.f, 256.f);
            params.init_min = sph_f3(5.f, 5.f, 5.f);
            params.init_max = sph_f3(256 * 0.3f, 128 * 0.9f, 256 * 0.3f);
            break;
        case SPHExample::WavePool:   // large beach front
            params.bound_min = sph_f3(0.f, 0.f, 0.f);
            params.bound_max = sph_f3(500.f, 200.f, 500.f);
            params.init_min = sph_f3(120.f, 60.f, 0.f);
            params.init_max = sph_f3(500.f, 195.f, 500.f);
            params.bound_wall_force = 100.0f;
            params.bound_wall_freq = 2.0f;
            params.bound_slope = 0.12f;
            break;
        case SPHExample::DamBreak:   // small dam break
            params.bound_min = sph_f3(-40.f, 0.f, -40.f);
            params.bound_max = sph_f3(40.f, 60.f, 40.f);
            params.init_min = sph_f3(0.f, 8.f, -35.f);
            params.init_max = sph_f3(35.f, 55.f, 35.f);
            params.bound_wall_force = 0.0f;
            params.bound_wall_freq = 0.0f;
            break;
    }
}


struct PortablePool {
    SPHFluidParams source;
    gvdb::slang::SphParameters parameters{};
    std::vector<gvdb::slang::Float4> positions;
    PortablePool(uint32_t capacity,SPHExample example) {
        if(!capacity||capacity>4'000'000)throw std::invalid_argument("Particle capacity must be in [1,4000000]");
        setupDefaultParams(source);source.pnum=int(capacity);setupExampleParams(source,example);
        auto& p=source;
        const float radius=p.psmoothradius,scale=p.sim_scale;
        if(p.pspacing==0)p.pspacing=std::cbrt(p.pmass/p.prest_dens)*0.87f/scale;
        else p.prest_dens=p.pmass/std::pow(p.pspacing*scale/0.87f,3.f);
        const float worldRadius=radius/scale;
        const auto minimum=p.bound_min-dm::float3(2*worldRadius);
        const auto maximum=p.bound_max+dm::float3(2*worldRadius);
        parameters.gridMinimumAndCellWidth={minimum.x,minimum.y,minimum.z,worldRadius};
        parameters.gridResolutionAndCount={uint32_t(std::ceil((maximum.x-minimum.x)/worldRadius)),
            uint32_t(std::ceil((maximum.y-minimum.y)/worldRadius)),uint32_t(std::ceil((maximum.z-minimum.z)/worldRadius)),0};
        parameters.physical={worldRadius,scale,p.pmass,p.prest_dens};
        constexpr float pi=3.141592f;
        parameters.kernels={315.f/(64*pi*std::pow(radius,9.f)),-45.f/(pi*std::pow(radius,6.f)),
            45.f*p.pvisc/(pi*std::pow(radius,6.f)),p.pintstiff};
        parameters.boundaryMinimum={p.bound_min.x,p.bound_min.y,p.bound_min.z,0};
        parameters.boundaryMaximum={p.bound_max.x,p.bound_max.y,p.bound_max.z,0};
        parameters.boundary={p.pradius,p.bound_stiff,p.bound_damp,p.bound_friction};
        parameters.integration={0,p.dt,p.AL,p.VL};
        parameters.gravityAndSlope={p.grav_dir.x*p.grav_amt,p.grav_dir.y*p.grav_amt,p.grav_dir.z*p.grav_amt,p.bound_slope};
        parameters.wall={p.bound_wall_freq,p.bound_wall_force,0,0};
        const uint32_t nx=uint32_t(std::ceil((p.init_max.x-p.init_min.x)/p.pspacing));
        const uint32_t nz=uint32_t(std::ceil((p.init_max.z-p.init_min.z)/p.pspacing));
        std::mt19937 generator(std::mt19937::default_seed);
        const auto jitter=[&](){return float(generator()>>8)*(p.pspacing/16777216.f);};
        for(float y=p.init_min.y;y<=p.init_max.y&&positions.size()<capacity;y+=p.pspacing)
            for(uint64_t xz=0;xz<uint64_t(nx)*nz&&positions.size()<capacity;++xz) {
                const float x=p.init_min.x+float(xz%nx)*p.pspacing,z=p.init_min.z+float(xz/nx)*p.pspacing;
                const float tx=(x-p.init_min.x)/(p.init_max.x-p.init_min.x);
                const float tz=(z-p.init_min.z)/(p.init_max.z-p.init_min.z);
                const uint32_t color=51u|(uint32_t((tx*.7f+.2f)*255)<<8)|
                    (uint32_t(((tx+tz)*.35f+.2f)*255)<<16)|(255u<<24);
                float packed;std::memcpy(&packed,&color,4);
                positions.push_back({x+jitter(),y+jitter(),z+jitter(),packed});
            }
        if(positions.empty())throw std::runtime_error("Preset generated no particles");
        parameters.gridResolutionAndCount.w=uint32_t(positions.size());
    }
};
} // namespace sph
