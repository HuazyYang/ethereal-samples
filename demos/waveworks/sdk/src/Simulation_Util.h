#ifndef SRC_WWS_SIMULATION_UTIL_H
#define SRC_WWS_SIMULATION_UTIL_H
#include "Types.h"
#include <waveworks/GFSDK_WaveWorks.h>

class Simulation_Util {
 public:
    static uint32_t xor_shift_rand128(bool reset);
    static float generate_uniform_01();
    static void powerOf2(uint32_t floorVal, uint32_t *exponent, uint32_t *power);

    static void initGaussData(float2 *table);
    static float Gauss();
    static void initOmegaData(const Wind_Waves_Detailed_Simulation_Parameters::Cascade &cascade, float2 *samples);

    static void addLocalDisplacements(
        const Local_Waves_Detailed_Simulation_Parameters &params,
        const uint8_t *input,
        uint rowPitch,
        const float2 *positions,
        float4 *output,
        uint  start,
        uint count,
        bool fp16
    );

    static void addDisplacements(
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade &cascade,
        const uint8_t *input,
        int rowPitch,
        const float2 * positions,
        float4 * output,
        uint start,
        uint count,
        float scale,
        bool fp16
    );

    static void FFTcSIMD(uint level, float4 *data, int sign);
    // 4 * n
    static void FFT1DSIMD_X_4wide(float2 *data, uint n, int sign);
    static void FFT1DSIMD_Y_4wide(float2 *data, uint n, int sign);

private:
   static constexpr int Float32InputPolicy = 0; // offset not applicable (static member)
   static constexpr int Float16InputPolicy = 1; // offset not applicable (static member)

   template <int InputPolicy>
   static void add_displacements(
       Wind_Waves_Detailed_Simulation_Parameters::Cascade const &cascade,
       const uint8_t *input, uint rowPitch, const float2 *positions, float4 *output,
       uint start, uint count, float scale);

   template <int InputPolicy>
   static void add_local_displacements(
       Local_Waves_Detailed_Simulation_Parameters const &params, const uint8_t *input,
       uint rowPitch, const float2 *positions, float4 *output, uint start, uint count);
};

#endif /* SRC_WWS_SIMULATION_UTIL_H */
