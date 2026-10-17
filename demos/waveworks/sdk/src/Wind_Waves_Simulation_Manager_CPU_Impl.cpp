#include "Wind_Waves_Simulation_Manager_CPU_Impl.h"

#include <algorithm>
#include <chrono>

void handle_ww_error(const wchar_t* filename, uint32_t line);

namespace
{
uint64_t nowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

float nsToMs(uint64_t value)
{
    return static_cast<float>(value) / 1000000.0f;
}
}

Wind_Waves_Simulation_Manager_CPU_Impl::Wind_Waves_Simulation_Manager_CPU_Impl(
    const Wind_Waves_Detailed_Simulation_Parameters& params,
    ThreadPool* threadPool)
    : m_simulations()
    , m_nextKickId(0)
    , m_tasksInFlight(false)
    , m_completionPending(false)
    , m_padding0{}
    , m_inFlightKickId(0)
    , m_readbackCursorValid(false)
    , m_padding1{}
    , m_readbackCursorKickId(0)
    , m_threadPool(threadPool)
    , m_cpuMainThreadWaitNs(0)
    , m_cpuThreadsTotalNs(0)
    , m_lastCpuThreadsTotalNs(0)
    , m_enableCpuTimers(params.enableCpuTimers)
    , m_padding2{}
{
}

Wind_Waves_Simulation_Manager_CPU_Impl::~Wind_Waves_Simulation_Manager_CPU_Impl()
{
    m_simulations.clear();
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::advanceReadbackCursor(bool block)
{
    (void)block;
    return WWE_NONE;
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::advanceStagingCursor(bool block)
{
    if (m_tasksInFlight)
    {
        if (!block && !m_threadPool->isLowPriorityWorkDone())
            return WWE_WOULD_BLOCK;
        waitTasksCompletion();
    }
    else if (!m_completionPending)
    {
        return WWE_NONE;
    }

    return completePendingSimulation();
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::archiveDisplacements()
{
    if (!m_readbackCursorValid)
        return WWE_FAIL;

    for (Wind_Waves_FFT_Simulation_CPU_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->archiveDisplacements(m_readbackCursorKickId);
        if (result < 0)
        {
            handle_ww_error(
                L"C:\\Work\\dev\\waveworks_example.package\\waveworks\\src\\Wind_Waves_Simulation_Manager_CPU_Impl.cpp",
                0x1E3u);
            return result;
        }
    }

    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::beforeReinit(
    const Wind_Waves_Detailed_Simulation_Parameters& params,
    bool allowSimulationUpdate)
{
    bool restartRequired = !allowSimulationUpdate;
    if (allowSimulationUpdate)
    {
        for (size_t i = 0; i < m_simulations.size(); ++i)
        {
            bool restartAll = false;
            bool reallocateResources = false;
            bool recreateDisplacementMap = false;
            bool refreshOmega = false;
            m_simulations[i]->calcReinit(
                params.cascades[i],
                restartAll,
                reallocateResources,
                recreateDisplacementMap,
                refreshOmega);
            if (restartAll || reallocateResources || refreshOmega)
            {
                restartRequired = true;
                break;
            }
        }
    }

    if (restartRequired)
    {
        if (m_tasksInFlight)
        {
            waitTasksCompletion();
            m_completionPending = false;
        }
        m_readbackCursorValid = false;
        return WWE_OK;
    }

    for (Wind_Waves_FFT_Simulation_CPU_Impl* simulation : m_simulations)
        simulation->m_useDelayedCascade = true;
    return WWE_OK;
}

void Wind_Waves_Simulation_Manager_CPU_Impl::computeFFT_X_NTask(
    uint32_t cascadeIndex,
    uint32_t taskY,
    uint32_t taskX)
{
    Wind_Waves_FFT_Simulation_CPU_Impl* simulation = m_simulations[cascadeIndex];
    if (simulation->ComputeFFT_X(taskY, taskX, -1) != 0)
        return;

    std::vector<std::function<void()>> tasks;
    const uint32_t taskCount = static_cast<uint32_t>(simulation->GetNumFFT_YTasks());
    tasks.reserve(3u * taskCount);
    for (uint32_t plane = 0; plane < 3; ++plane)
        for (uint32_t task = 0; task < taskCount; ++task)
            tasks.emplace_back([this, cascadeIndex, plane, task]() {
                processWindWavesSimulationTask(3, cascadeIndex, plane, task);
            });
    m_threadPool->enqueueLowPriorityVector(std::move(tasks));
}

void Wind_Waves_Simulation_Manager_CPU_Impl::computeFFT_Y_NTask(
    uint32_t cascadeIndex,
    uint32_t taskY,
    uint32_t taskX)
{
    Wind_Waves_FFT_Simulation_CPU_Impl* simulation = m_simulations[cascadeIndex];
    if (simulation->ComputeFFT_Y(taskY, taskX, -1) != 0)
        return;

    std::vector<std::function<void()>> tasks;
    const uint32_t taskCount =
        static_cast<uint32_t>(simulation->GetNumUpdateTextureTasks());
    tasks.reserve(taskCount);
    for (uint32_t task = 0; task < taskCount; ++task)
        tasks.emplace_back([this, cascadeIndex, task]() {
            processWindWavesSimulationTask(4, cascadeIndex, 0, task);
        });
    m_threadPool->enqueueLowPriorityVector(std::move(tasks));
}

Wind_Waves_FFT_Simulation* Wind_Waves_Simulation_Manager_CPU_Impl::createSimulation(
    const Wind_Waves_Detailed_Simulation_Parameters::Cascade& cascade)
{
    Wind_Waves_FFT_Simulation_CPU_Impl* simulation =
        new Wind_Waves_FFT_Simulation_CPU_Impl(cascade);
    m_simulations.push_back(simulation);
    return simulation;
}

void Wind_Waves_Simulation_Manager_CPU_Impl::flip(bool alreadyOpen)
{
    if (!alreadyOpen)
        m_simulations.front()->openCmdList();

    for (Wind_Waves_FFT_Simulation_CPU_Impl* simulation : m_simulations)
        simulation->OnCompleteSimulationStep(m_inFlightKickId);

    if (!alreadyOpen)
        m_simulations.front()->closeAndExecuteCmdList();

    m_lastCpuThreadsTotalNs = m_cpuThreadsTotalNs.load();
    m_readbackCursorKickId = m_inFlightKickId;
    m_completionPending = false;
    m_readbackCursorValid = true;
}

bool Wind_Waves_Simulation_Manager_CPU_Impl::getReadbackCursor(uint64_t* cursor) const
{
    if (cursor && m_readbackCursorValid)
        *cursor = m_readbackCursorKickId;
    return m_readbackCursorValid;
}

bool Wind_Waves_Simulation_Manager_CPU_Impl::getStagingCursor(uint64_t* cursor) const
{
    if (cursor && m_readbackCursorValid)
        *cursor = m_readbackCursorKickId;
    return m_readbackCursorValid;
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::getTimings(
    GFSDK_WaveWorks_Wind_Waves_Simulation_Stats& stats) const
{
    stats.CPU_main_thread_wait_time = nsToMs(m_cpuMainThreadWaitNs);
    stats.CPU_threads_total_time = nsToMs(m_lastCpuThreadsTotalNs);
    stats.GPU_simulation_time = 0.0f;
    return WWE_OK;
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::kick(double simulationTime, uint64_t& kickId)
{
    if (m_simulations.empty())
        return WWE_OK;

    kickId = m_nextKickId;
    // pseudo: the caller (Wind_Waves_Simulation::kick) has already opened the shared
    // command list, so the pending step is completed without open/closeAndExecuteCmdList
    if (m_tasksInFlight)
    {
        waitTasksCompletion();
        flip(true);
    }
    else if (m_completionPending)
    {
        flip(true);
    }

    m_cpuThreadsTotalNs.store(0);

    uint32_t totalTaskCount = 0;
    for (Wind_Waves_FFT_Simulation_CPU_Impl* simulation : m_simulations)
        totalTaskCount += static_cast<uint32_t>(simulation->GetTotalNumberOfTasks());

    // pseudo: completed = 0, submitted = totalTaskCount, set before OnInitiateSimulationStep;
    // the tasks themselves are pushed afterwards
    m_threadPool->submitLowPriorityVector(std::vector<std::function<void()>>(), totalTaskCount);

    for (Wind_Waves_FFT_Simulation_CPU_Impl* simulation : m_simulations)
    {
        const WWRESULT result = simulation->OnInitiateSimulationStep(simulationTime);
        if (result < 0)
        {
            handle_ww_error(
                L"C:\\Work\\dev\\waveworks_example.package\\waveworks\\src\\Wind_Waves_Simulation_Manager_CPU_Impl.cpp",
                0x134u);
            return result;
        }
    }

    std::vector<std::function<void()>> tasks;
    for (uint32_t cascadeIndex = 0; cascadeIndex < m_simulations.size(); ++cascadeIndex)
    {
        Wind_Waves_FFT_Simulation_CPU_Impl* simulation = m_simulations[cascadeIndex];
        const bool updateH0 = simulation->m_hasDisplacementMap;
        const uint32_t taskKind = updateH0 ? 0u : 1u;
        const uint32_t taskCount = static_cast<uint32_t>(
            updateH0 ? simulation->GetNumUpdateH0Tasks() : simulation->GetNumUpdateHtTasks());
        for (uint32_t task = 0; task < taskCount; ++task)
            tasks.emplace_back([this, taskKind, cascadeIndex, task]() {
                processWindWavesSimulationTask(taskKind, cascadeIndex, 0, task);
            });
        simulation->m_hasDisplacementMap = false;
    }

    m_threadPool->enqueueLowPriorityVector(std::move(tasks));
    m_tasksInFlight = true;
    m_inFlightKickId = m_nextKickId;
    ++m_nextKickId;
    return WWE_OK;
}

void Wind_Waves_Simulation_Manager_CPU_Impl::processWindWavesSimulationTask(
    uint32_t taskKind,
    uint32_t cascadeIndex,
    uint32_t taskY,
    uint32_t taskX)
{
    const uint64_t begin = m_enableCpuTimers ? nowNs() : 0;
    switch (taskKind)
    {
    case 0:
        updateH0Task(cascadeIndex, taskX);
        break;
    case 1:
        updateHtTask(cascadeIndex, taskX);
        break;
    case 2:
        computeFFT_X_NTask(cascadeIndex, taskY, taskX);
        break;
    case 3:
        computeFFT_Y_NTask(cascadeIndex, taskY, taskX);
        break;
    case 4:
        updateTextureTask(cascadeIndex, taskX);
        break;
    default:
        break;
    }

    if (m_enableCpuTimers)
        m_cpuThreadsTotalNs.fetch_add(nowNs() - begin);
}

void Wind_Waves_Simulation_Manager_CPU_Impl::releaseSimulation(
    Wind_Waves_FFT_Simulation* simulation)
{
    if (m_tasksInFlight)
    {
        waitTasksCompletion();
        m_completionPending = false;
    }

    auto it = std::find(m_simulations.begin(), m_simulations.end(), simulation);
    if (it != m_simulations.end())
        m_simulations.erase(it);

    delete simulation;
}

void Wind_Waves_Simulation_Manager_CPU_Impl::updateH0Task(
    uint32_t cascadeIndex,
    uint32_t taskIndex)
{
    Wind_Waves_FFT_Simulation_CPU_Impl* simulation = m_simulations[cascadeIndex];
    if (simulation->UpdateH0(taskIndex) != 0)
        return;

    std::vector<std::function<void()>> tasks;
    const uint32_t taskCount = static_cast<uint32_t>(simulation->GetNumUpdateHtTasks());
    tasks.reserve(taskCount);
    for (uint32_t task = 0; task < taskCount; ++task)
        tasks.emplace_back([this, cascadeIndex, task]() {
            processWindWavesSimulationTask(1, cascadeIndex, 0, task);
        });
    m_threadPool->enqueueLowPriorityVector(std::move(tasks));
}

void Wind_Waves_Simulation_Manager_CPU_Impl::updateHtTask(
    uint32_t cascadeIndex,
    uint32_t taskIndex)
{
    Wind_Waves_FFT_Simulation_CPU_Impl* simulation = m_simulations[cascadeIndex];
    if (simulation->UpdateHt(taskIndex) != 0)
        return;

    std::vector<std::function<void()>> tasks;
    const uint32_t taskCount = static_cast<uint32_t>(simulation->GetNumFFT_XTasks());
    tasks.reserve(3u * taskCount);
    for (uint32_t plane = 0; plane < 3; ++plane)
        for (uint32_t task = 0; task < taskCount; ++task)
            tasks.emplace_back([this, cascadeIndex, plane, task]() {
                processWindWavesSimulationTask(2, cascadeIndex, plane, task);
            });
    m_threadPool->enqueueLowPriorityVector(std::move(tasks));
}

void Wind_Waves_Simulation_Manager_CPU_Impl::updateTextureTask(
    uint32_t cascadeIndex,
    uint32_t taskIndex)
{
    if (cascadeIndex < m_simulations.size())
        m_simulations[cascadeIndex]->UpdateDisplacements(taskIndex);
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::waitStagingCursor()
{
    if (!m_tasksInFlight)
        return WWE_NONE;
    waitTasksCompletion();
    return WWE_OK;
}

void Wind_Waves_Simulation_Manager_CPU_Impl::waitTasksCompletion()
{
    const uint64_t begin = m_enableCpuTimers ? nowNs() : 0;
    m_threadPool->waitForLowPriorityWorkDone();
    m_tasksInFlight = false;
    m_completionPending = true;
    if (m_enableCpuTimers)
        m_cpuMainThreadWaitNs = nowNs() - begin;
}

WWRESULT Wind_Waves_Simulation_Manager_CPU_Impl::completePendingSimulation()
{
    if (m_tasksInFlight)
        waitTasksCompletion();
    if (!m_completionPending)
        return WWE_NONE;

    flip(false);
    return WWE_OK;
}
