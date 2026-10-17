#ifndef SRC_WWS_THREADPOOL_H
#define SRC_WWS_THREADPOOL_H

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool
{
public:
    explicit ThreadPool(unsigned int threadCount = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void prepareHighPrioritySubmission(unsigned int submittedCount);
    void submitHighPriorityVector(std::vector<std::function<void()>>&& work);
    void submitLowPriorityVector(std::vector<std::function<void()>>&& work);
    void submitLowPriorityVector(
        std::vector<std::function<void()>>&& work,
        unsigned int submittedCount);
    void enqueueLowPriorityVector(std::vector<std::function<void()>>&& work);
    bool isLowPriorityWorkDone() const;
    unsigned int getThreadCount() const;
    void waitForHighPriorityWorkDone();
    void waitForLowPriorityWorkDone();

private:
    using Task = std::function<void()>;
    using SharedTask = std::shared_ptr<Task>;

    struct ThreadSafeQueue
    {
        std::mutex mutex; // offset 0
        std::queue<SharedTask> tasks; // offset 80
        std::condition_variable changed; // offset 120
        bool active = true; // offset 192

        void pushVector(std::vector<Task>&& work);
    };

    struct WorkGroup
    {
        ThreadSafeQueue queue; // offset 0
        std::atomic_uint submitted{0}; // offset 200
        std::atomic_uint completed{0}; // offset 204
        std::condition_variable doneChanged; // offset 208
        std::mutex doneMutex; // offset 280
    };

    void setQueueActive(ThreadSafeQueue& queue, bool active);
    void prepareSubmission(WorkGroup& group, unsigned int submittedCount);
    void markHighPriorityTaskDone();
    void markLowPriorityTaskDone();
    void workerThreadFunction();

#if defined(_WIN32)
    // The recovered worker owner has no implicit join/close on construction unwind.
    struct WorkerThread
    {
        void* handle; // offset 0
        unsigned int id; // offset 8

        explicit WorkerThread(ThreadPool* pool);
        bool joinable() const { return id != 0; }
        void join();
        static unsigned int __stdcall run(void* argument);
    };
    static_assert(sizeof(WorkerThread) == 16, "Recovered worker record size");
#else
    using WorkerThread = std::thread;
#endif

    std::vector<WorkerThread> m_threads; // offset 0
    std::atomic_bool m_shutdown; // offset 24
    WorkGroup m_lowPriorityWork; // offset 32
    WorkGroup m_highPriorityWork; // offset 392
};

#endif /* SRC_WWS_THREADPOOL_H */
