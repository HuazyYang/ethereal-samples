#include "ThreadPool.h"

#include <algorithm>
#include <utility>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#include <process.h>
#include <xthreads.h>
#endif

namespace
{
unsigned int resolveThreadCount(unsigned int requested)
{
    unsigned int threadCount = requested;
    if (threadCount == 0)
        threadCount = std::thread::hardware_concurrency();

    const unsigned int hardwareCount = std::thread::hardware_concurrency();
    threadCount = std::min(threadCount, hardwareCount);
    return std::max(1u, threadCount);
}
}

#if defined(_WIN32)
using WorkerStart = std::pair<ThreadPool*, void (ThreadPool::*)()>;

ThreadPool::WorkerThread::WorkerThread(ThreadPool* pool)
    : handle(nullptr), id(0)
{
    auto start = std::make_unique<WorkerStart>(pool, &ThreadPool::workerThreadFunction);
    handle = reinterpret_cast<void*>(_beginthreadex(nullptr, 0, &WorkerThread::run,
                                                   start.get(), 0, &id));
    if (!handle)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    start.release();
}

unsigned int __stdcall ThreadPool::WorkerThread::run(void* argument)
{
    std::unique_ptr<WorkerStart> start(static_cast<WorkerStart*>(argument));
    (start->first->*start->second)();
    _Cnd_do_broadcast_at_thread_exit();
    return 0;
}

void ThreadPool::WorkerThread::join()
{
    if (!joinable())
        throw std::system_error(std::make_error_code(std::errc::invalid_argument));
    if (id == GetCurrentThreadId())
        throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur));
    if (WaitForSingleObject(handle, INFINITE) == WAIT_FAILED)
        throw std::system_error(std::make_error_code(std::errc::no_such_process));
    CloseHandle(handle);
    handle = nullptr;
    id = 0;
}
#endif

void ThreadPool::ThreadSafeQueue::pushVector(std::vector<Task>&& work)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (Task& task : work)
            tasks.push(std::make_shared<Task>(std::move(task)));
    }
    changed.notify_all();
}

ThreadPool::ThreadPool(unsigned int threadCount)
    : m_shutdown(false)
{
    const unsigned int resolvedThreadCount = resolveThreadCount(threadCount);
    try
    {
        for (unsigned int i = 0; i < resolvedThreadCount; ++i)
#if defined(_WIN32)
            m_threads.push_back(WorkerThread(this));
#else
            m_threads.emplace_back(&ThreadPool::workerThreadFunction, this);
#endif
    }
    catch (...)
    {
        m_shutdown.store(true);
#if !defined(_WIN32)
        setQueueActive(m_lowPriorityWork.queue, false);
        setQueueActive(m_highPriorityWork.queue, false);
        for (WorkerThread& thread : m_threads)
        {
            if (thread.joinable())
                thread.join();
        }
#endif
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    m_shutdown.store(true);
    setQueueActive(m_lowPriorityWork.queue, false);
    setQueueActive(m_highPriorityWork.queue, false);

    for (WorkerThread& thread : m_threads)
    {
        if (thread.joinable())
            thread.join();
    }
}

void ThreadPool::setQueueActive(ThreadSafeQueue& queue, bool active)
{
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.active = active;
    }
    queue.changed.notify_all();
}

void ThreadPool::prepareSubmission(WorkGroup& group, unsigned int submittedCount)
{
    group.completed.store(0);
    group.submitted.store(submittedCount);
}

void ThreadPool::submitHighPriorityVector(std::vector<Task>&& work)
{
    setQueueActive(m_lowPriorityWork.queue, false);
    m_highPriorityWork.queue.pushVector(std::move(work));
}

void ThreadPool::prepareHighPrioritySubmission(unsigned int submittedCount)
{
    prepareSubmission(m_highPriorityWork, submittedCount);
}

void ThreadPool::submitLowPriorityVector(std::vector<Task>&& work)
{
    const unsigned int submittedCount = static_cast<unsigned int>(work.size());
    submitLowPriorityVector(std::move(work), submittedCount);
}

void ThreadPool::submitLowPriorityVector(
    std::vector<Task>&& work,
    unsigned int submittedCount)
{
    prepareSubmission(m_lowPriorityWork, submittedCount);
    m_lowPriorityWork.queue.pushVector(std::move(work));
}

void ThreadPool::enqueueLowPriorityVector(std::vector<Task>&& work)
{
    m_lowPriorityWork.queue.pushVector(std::move(work));
}

bool ThreadPool::isLowPriorityWorkDone() const
{
    return m_lowPriorityWork.completed.load() >= m_lowPriorityWork.submitted.load();
}

unsigned int ThreadPool::getThreadCount() const
{
    return static_cast<unsigned int>(m_threads.size());
}

void ThreadPool::waitForHighPriorityWorkDone()
{
    std::unique_lock<std::mutex> lock(m_highPriorityWork.doneMutex);
    m_highPriorityWork.doneChanged.wait(lock, [this]() {
        return m_highPriorityWork.completed.load() >= m_highPriorityWork.submitted.load();
    });
}

void ThreadPool::waitForLowPriorityWorkDone()
{
    std::unique_lock<std::mutex> lock(m_lowPriorityWork.doneMutex);
    m_lowPriorityWork.doneChanged.wait(lock, [this]() {
        return m_lowPriorityWork.completed.load() >= m_lowPriorityWork.submitted.load();
    });
}

void ThreadPool::markHighPriorityTaskDone()
{
    const unsigned int completed = m_highPriorityWork.completed.fetch_add(1) + 1;
    if (completed < m_highPriorityWork.submitted.load())
        return;

    std::lock_guard<std::mutex> doneLock(m_highPriorityWork.doneMutex);
    m_highPriorityWork.doneChanged.notify_all();
    setQueueActive(m_lowPriorityWork.queue, true);
}

void ThreadPool::markLowPriorityTaskDone()
{
    const unsigned int completed = m_lowPriorityWork.completed.fetch_add(1) + 1;
    if (completed < m_lowPriorityWork.submitted.load())
        return;

    std::lock_guard<std::mutex> doneLock(m_lowPriorityWork.doneMutex);
    m_lowPriorityWork.doneChanged.notify_all();
}

void ThreadPool::workerThreadFunction()
{
    while (!m_shutdown.load())
    {
        SharedTask task;

        {
            std::lock_guard<std::mutex> lock(m_highPriorityWork.queue.mutex);
            if (m_highPriorityWork.queue.active &&
                !m_highPriorityWork.queue.tasks.empty())
            {
                task = std::move(m_highPriorityWork.queue.tasks.front());
                m_highPriorityWork.queue.tasks.pop();
            }
        }

        if (task)
        {
            (*task)();
            markHighPriorityTaskDone();
            continue;
        }

        std::unique_lock<std::mutex> lowLock(m_lowPriorityWork.queue.mutex);
        if (m_lowPriorityWork.queue.active &&
            m_lowPriorityWork.queue.tasks.empty())
            m_lowPriorityWork.queue.changed.wait(lowLock);

        if (!m_lowPriorityWork.queue.active ||
            m_lowPriorityWork.queue.tasks.empty())
        {
            lowLock.unlock();
            std::this_thread::yield();
            continue;
        }

        task = std::move(m_lowPriorityWork.queue.tasks.front());
        m_lowPriorityWork.queue.tasks.pop();
        lowLock.unlock();

        (*task)();
        markLowPriorityTaskDone();
    }
}
