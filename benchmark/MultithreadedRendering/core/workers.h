#pragma once
// Persistent worker threads shared by both renderers: the thread orchestration is part of the
// workload, so it must be identical on both sides. Dispatch() hands out jobs 0..count-1 to the
// workers through an atomic counter; the main thread may take jobs too (Help()) and spins until
// every job finished (Wait()).

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace MtScene
{

class WorkerPool
{
public:
    explicit WorkerPool(uint32_t workerThreads)
    {
        for (uint32_t i = 0; i < workerThreads; ++i)
            m_Threads.emplace_back([this, i] { ThreadMain(i); });
    }

    ~WorkerPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Terminate = true;
            ++m_Generation;
        }
        m_Signal.notify_all();
        for (auto& t : m_Threads) t.join();
    }

    uint32_t WorkerCount() const { return uint32_t(m_Threads.size()); }

    // Starts `jobCount` jobs; `fn(jobIndex)` is called once per job on some thread. Returns immediately.
    void Dispatch(uint32_t jobCount, std::function<void(uint32_t)> fn)
    {
        m_Function = std::move(fn);
        m_JobCount = jobCount;
        m_NextJob.store(0, std::memory_order_relaxed);
        m_Completed.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_Generation;
        }
        m_Signal.notify_all();
    }

    // The calling thread takes jobs until none is left.
    void Help()
    {
        RunJobs();
    }

    // Blocks (spinning) until every job of the last Dispatch() finished.
    void Wait()
    {
        while (m_Completed.load(std::memory_order_acquire) < m_JobCount)
            std::this_thread::yield();
    }

private:
    void RunJobs()
    {
        for (;;)
        {
            const uint32_t job = m_NextJob.fetch_add(1, std::memory_order_relaxed);
            if (job >= m_JobCount) break;
            m_Function(job);
            m_Completed.fetch_add(1, std::memory_order_release);
        }
    }

    void ThreadMain(uint32_t)
    {
        uint64_t seen = 0;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Signal.wait(lock, [&] { return m_Terminate || m_Generation != seen; });
                if (m_Terminate) return;
                seen = m_Generation;
            }
            RunJobs();
        }
    }

    std::vector<std::thread> m_Threads;
    std::mutex m_Mutex;
    std::condition_variable m_Signal;
    uint64_t m_Generation = 0;
    bool m_Terminate = false;
    std::function<void(uint32_t)> m_Function;
    uint32_t m_JobCount = 0;
    std::atomic<uint32_t> m_NextJob{0};
    std::atomic<uint32_t> m_Completed{0};
};

} // namespace MtScene
