#include "Wind_Waves_Simulation_Manager_Compute_Impl.h"

#include <algorithm>
#include <limits>
#include <vector>

void handle_ww_error(const wchar_t* filename, uint32_t line);

namespace
{
constexpr uint64_t kInvalidCursor = std::numeric_limits<uint64_t>::max();
}

Wind_Waves_Simulation_Manager_Compute_Impl::Wind_Waves_Simulation_Manager_Compute_Impl()
    : m_simulations()
    , m_nextKickId(0)
    , m_stagingCursorValid(false)
    , m_padding0{}
    , m_stagingCursorKickId(0)
{
}

Wind_Waves_Simulation_Manager_Compute_Impl::~Wind_Waves_Simulation_Manager_Compute_Impl()
{
    m_simulations.clear();
}

void Wind_Waves_Simulation_Manager_Compute_Impl::VerifyReadbackLockstep() const
{
    if (m_simulations.size() <= 1)
        return;

    uint64_t firstCursor = kInvalidCursor;
    m_simulations.front()->getReadbackCursor(&firstCursor);
    (void)firstCursor;

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
    {
        uint64_t cursor = kInvalidCursor;
        simulation->getReadbackCursor(&cursor);
        (void)cursor;
    }
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::advanceReadbackCursor(bool block)
{
    if (m_simulations.empty())
        return WWE_NONE;

    const bool hasReadbacksInFlight = m_simulations.front()->hasReadbacksInFlight();
    VerifyReadbackLockstep();
    if (!hasReadbacksInFlight)
        return WWE_NONE;

    if (!block)
    {
        for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
        {
            const int64_t ready = simulation->canCollectSingleReadbackResultWithoutBlocking();
            if (ready < 0)
                return WWE_FAIL;
            if (ready == WWE_NONE)
                return WWE_WOULD_BLOCK;
        }
    }

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->collectSingleReadbackResult(true);
        if (result < 0)
            return WWE_FAIL;
    }

    VerifyReadbackLockstep();
    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::advanceStagingCursor(bool block)
{
    (void)block;
    return WWE_NONE;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::archiveDisplacements()
{
    if (!getReadbackCursor(nullptr))
        return WWE_INTERNAL_ERROR;

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->archiveDisplacements();
        if (result < 0)
        {
            handle_ww_error(
                L"C:\\Work\\dev\\waveworks_example.package\\waveworks\\src\\Wind_Waves_Simulation_Manager_Compute_Impl.cpp",
                0xF5u);
            return result;
        }
    }

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::beforeReallocateSimulation()
{
    m_stagingCursorValid = false;

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->resetReadbacks();
        if (result < 0)
            return result;
    }

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::beforeReinit(
    const Wind_Waves_Detailed_Simulation_Parameters& params,
    bool allowSimulationUpdate)
{
    (void)params;
    (void)allowSimulationUpdate;
    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::checkForReadbackResults()
{
    bool collectedAny = false;
    std::vector<Wind_Waves_FFT_Simulation_Compute_Impl*> pendingSimulations;

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->collectSingleReadbackResult(false);
        if (result < 0)
            return result;

        if (result == WWE_NONE)
            pendingSimulations.push_back(simulation);
        else
            collectedAny = true;
    }

    if (collectedAny)
    {
        for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : pendingSimulations)
        {
            const WWRESULT result = simulation->collectSingleReadbackResult(true);
            if (result < 0)
                return result;
        }
    }

    VerifyReadbackLockstep();
    return WWE_OK;
}

Wind_Waves_FFT_Simulation* Wind_Waves_Simulation_Manager_Compute_Impl::createSimulation(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade)
{
    Wind_Waves_FFT_Simulation_Compute_Impl* simulation =
        new Wind_Waves_FFT_Simulation_Compute_Impl(this, cascade);
    m_simulations.push_back(simulation);
    return simulation;
}

bool Wind_Waves_Simulation_Manager_Compute_Impl::getReadbackCursor(uint64_t* cursor) const
{
    if (m_simulations.empty())
        return false;

    VerifyReadbackLockstep();
    return m_simulations.front()->getReadbackCursor(cursor);
}

bool Wind_Waves_Simulation_Manager_Compute_Impl::getStagingCursor(uint64_t* cursor) const
{
    if (cursor && m_stagingCursorValid)
        *cursor = m_stagingCursorKickId;
    return m_stagingCursorValid;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::getTimings(
    GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const
{
    stats.CPU_main_thread_wait_time = 0.0f;
    stats.CPU_threads_total_time = 0.0f;
    stats.GPU_simulation_time = 0.0f;

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
        stats.GPU_simulation_time += simulation->getSimulationTime();

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::kick(
    double simulationTime,
    uint64_t& kickId)
{
    kickId = m_nextKickId;

    WWRESULT readbackResult = checkForReadbackResults();
    if (readbackResult < 0)
        return readbackResult;

    for (Wind_Waves_FFT_Simulation_Compute_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->kick(simulationTime, kickId);
        if (result < 0)
            return result;
    }

    m_stagingCursorKickId = m_nextKickId;
    ++m_nextKickId;
    m_stagingCursorValid = true;
    return WWE_OK;
}

void Wind_Waves_Simulation_Manager_Compute_Impl::releaseSimulation(
    Wind_Waves_FFT_Simulation* simulation)
{
    auto it = std::find(m_simulations.begin(), m_simulations.end(), simulation);
    if (it != m_simulations.end())
        m_simulations.erase(it);

    delete simulation;
}

WWRESULT Wind_Waves_Simulation_Manager_Compute_Impl::waitStagingCursor()
{
    return WWE_NONE;
}
