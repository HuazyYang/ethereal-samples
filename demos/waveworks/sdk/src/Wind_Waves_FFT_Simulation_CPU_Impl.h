#ifndef SRC_WWS_WIND_WAVES_FFT_SIMULATION_CPU_IMPL_H
#define SRC_WWS_WIND_WAVES_FFT_SIMULATION_CPU_IMPL_H

#include <cstdint>
#include <vector>
#include <atomic>
#include <memory>
#include <nvrhi/nvrhi.h>
#include <nvrhi/core/foundation.h>
#include <waveworks/GFSDK_WaveWorks.h>
#include "Types.h"
#include "Simulation_Util.h"
#include "ThreadPool.h"
#include "Quadtree.h"

class Wind_Waves_Simulation_Manager_CPU_Impl;

class Wind_Waves_FFT_Simulation
{
public:
    virtual ~Wind_Waves_FFT_Simulation() = default;
};

class Wind_Waves_FFT_Simulation_CPU_Impl : public Wind_Waves_FFT_Simulation
{
public:
    explicit Wind_Waves_FFT_Simulation_CPU_Impl(
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade);
    ~Wind_Waves_FFT_Simulation_CPU_Impl() override;

    int64_t ComputeFFT_X(uint taskX, uint taskY, int sign);
    int64_t ComputeFFT_Y(uint taskX, uint taskY, int sign);

    int64_t GetNumFFT_XTasks() const;
    int64_t GetNumFFT_YTasks() const;
    int64_t GetNumUpdateH0Tasks() const;
    int64_t GetNumUpdateHtTasks() const;
    int64_t GetNumUpdateTextureTasks() const;
    int64_t GetTotalNumberOfTasks() const;

    void OnCompleteSimulationStep(uint64_t frameIndex);
    WWRESULT OnInitiateSimulationStep(double simulationTime);

    int64_t UpdateDisplacements(unsigned int taskIndex);
    int64_t UpdateH0(unsigned int taskIndex);
    int64_t UpdateHt(unsigned int taskIndex);

    WWRESULT allocateAllResources();
    WWRESULT archiveDisplacements(uint64_t frameIndex);

    void calcReinit(
        const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade,
        bool& outRestartAll,
        bool& outReallocateResources,
        bool& outRecreateDisplacementMap,
        bool& outRefreshOmega);

    void closeAndExecuteCmdList();

    WWRESULT getDisplacementArchiveCoords(
        float time,
        int* prevIndex,
        int* nextIndex,
        float* interpolation) const;

    nvrhi::TextureHandle getDisplacementMap() const;
    uint64_t getDisplacementMapVersion() const;

    WWRESULT init(
        nvrhi::IDevice* device,
        nvrhi::CommandListHandle cmdList,
        bool useCpu);

    void initGaussAndOmega();

    WWRESULT mapDisplacementArchiveData(
        uint8_t** data,
        uint32_t* rowPitch,
        uint32_t age,
        bool* outReadOnly);

    WWRESULT mapDisplacementData(
        uint8_t** data,
        uint32_t* rowPitch,
        bool* outReadOnly);

    void openCmdList();
    WWRESULT reinit(const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade);
    void releaseAll();
    void releaseAllResources();
    void unmapDisplacementArchiveData(unsigned int slotIndex);
    void unmapDisplacementData();

private:
    friend class Wind_Waves_Simulation_Manager_CPU_Impl;

    struct ArchiveEntry
    {
        uint64_t frameIndex; // offset 0
        uint8_t* buffer; // offset 8
    };

    struct ArchiveRing
    {
        ArchiveEntry* entries; // offset 0
        uint32_t capacity; // offset 8
        uint32_t startIndex; // offset 12
        uint32_t count; // offset 16
        uint32_t reserved; // offset 20
    };

    Wind_Waves_Detailed_Simulation_Parameters::Cascade m_pendingCascade; // offset 8 = 1 * 8
    Wind_Waves_Detailed_Simulation_Parameters::Cascade m_activeCascade;  // offset 136 = 17 * 8

    bool m_hasPendingCascade;      // offset 264 = 33 * 8
    char m_padding0[3]; // offset 265
    std::atomic<int32_t> m_numUpdateH0Tasks;    // offset 268
    std::atomic<int32_t> m_numUpdateHtTasks;    // offset 272 = 34 * 8
    std::atomic<int32_t> m_numFFT_XTasks;       // offset 276
    std::atomic<int32_t> m_numFFT_YTasks;       // offset 280 = 35 * 8
    std::atomic<int32_t> m_numUpdateTextureTasks; // offset 284

    nvrhi::DeviceHandle m_device;           // offset 288 = 36 * 8
    nvrhi::CommandListHandle m_cmdList;     // offset 296 = 37 * 8
    bool m_useCpu;                                         // offset 304 = 38 * 8
    char m_padding1[7]; // offset 305
    nvrhi::TextureHandle m_displacementTexture; // offset 312 = 39 * 8
    nvrhi::StagingTextureHandle m_staging[2];   // offsets 320/328 = 40 * 8

    uint16_t* m_fftxBuffersFp16[2];          // offset 336 = 42/43 * 8
    uint32_t m_fftxRowPitch;      // offset 352 = 44 * 8
    uint32_t m_padding2;          // offset 356

    float2* m_gaussData;          // offset 360 = 45 * 8
    float2* m_omegaData0;         // offset 368 = 46 * 8
    float2* m_omegaData1;         // offset 376 = 47 * 8
    float* m_inverseRadius;       // offset 384 = 48 * 8
    float2* m_fftBuffers;         // offset 392 = 49 * 8
    float4* m_displacementBuffers[2]; // offset 400 = 50/51 * 8
    uint8_t* m_currentDisplacementBuffer; // offset 416 = 52 * 8
    ArchiveRing* m_archiveRing;   // offset 424 = 53 * 8
    uint32_t m_currentDisplacementBufferIndex; // offset 432 = 54 * 8
    uint32_t m_padding3;          // offset 436
    double m_simulationTime;      // offset 440 = 55 * 8
    bool m_hasDisplacementMap;    // offset 448 = 56 * 8
    char m_padding4[7]; // offset 449
    uint64_t m_displacementMapVersion; // offset 456 = 57 * 8
    bool m_useDelayedCascade;     // offset 464 = 58 * 8
    char m_padding5[7]; // offset 465
};

static_assert(sizeof(std::atomic<int32_t>) == sizeof(int32_t) &&
                  alignof(std::atomic<int32_t>) == alignof(int32_t),
              "Atomic task counters must preserve the recovered object layout.");
static_assert(sizeof(Wind_Waves_Detailed_Simulation_Parameters::Cascade) == 128 && sizeof(Wind_Waves_FFT_Simulation_CPU_Impl) == 59 * 8,
              "Cascade must be 128 bytes.");

#endif /* SRC_WWS_WIND_WAVES_FFT_SIMULATION_CPU_IMPL_H */
