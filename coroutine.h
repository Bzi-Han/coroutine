#ifndef COROUTINE_H // !COROUTINE_H
#define COROUTINE_H

#include <coroutine>
#include <vector>
#include <queue>
#include <map>
#include <unordered_map>
#include <mutex>
#include <future>
#include <thread>

namespace coroutine
{
    namespace task_implement
    {
        class TaskBase;

        template <typename return_t, typename yield_t>
        class Task;

        template <typename return_t, typename yield_t>
        struct task_promise_type_base;

        template <typename return_t, typename yield_t>
        struct task_promise_type;

        template <typename yield_t>
        struct task_promise_type<void, yield_t>;

        template <typename coro_t>
        struct awaitable;

        struct deferrable;

        class TaskBase
        {
        public:
            static thread_local std::queue<void *> m_taskQueue;
            static thread_local std::map<uint64_t, deferrable> m_delayQueue;
            static thread_local std::unordered_map<void *, void *> m_resumeQueue;
#ifdef COROUTINE_FEATURE_COROFINALLY // COROUTINE_FEATURE_COROFINALLY
            static thread_local std::queue<std::pair<Task<void, void>, bool>> m_finallyQueue;
#endif // COROUTINE_FEATURE_COROFINALLY
        };

        template <typename return_t, typename yield_t>
        class Task final : public TaskBase
        {
        public:
            using promise_type = task_promise_type<return_t, yield_t>;
            using return_type = return_t;
            using yield_type = yield_t;

        public:
            Task(const Task &) = delete;
            Task &operator=(const Task &) = delete;

            Task(std::coroutine_handle<promise_type> coroutineHandle)
                : m_coroutineHandle(coroutineHandle)
            {
            }
            Task(Task &&other)
            {
                m_coroutineHandle = std::move(other.m_coroutineHandle);
                other.m_coroutineHandle = nullptr;
            }
            ~Task()
            {
                if (nullptr != m_coroutineHandle)
                    m_coroutineHandle.destroy();
            }

            constexpr return_t result() const noexcept
            {
                return m_coroutineHandle.promise().returnValue;
            }

            constexpr operator return_t() const noexcept
            {
                return m_coroutineHandle.promise().returnValue;
            }

            constexpr auto begin() const
            {
                static_assert(!std::is_same_v<void, yield_t>, "The non-yield task cannot be iteration");

                return m_coroutineHandle.promise().yield_values.cbegin();
            }

            constexpr auto end() const
            {
                static_assert(!std::is_same_v<void, yield_t>, "The non-yield task cannot be iteration");

                return m_coroutineHandle.promise().yield_values.cend();
            }

            constexpr bool operator()() const noexcept
            {
                if (nullptr == m_coroutineHandle || m_coroutineHandle.done())
                    return false;

                return (m_coroutineHandle(), true);
            }

            constexpr operator void *() const noexcept
            {
                return nullptr == m_coroutineHandle ? nullptr : m_coroutineHandle.address();
            }

            constexpr bool done() const noexcept
            {
                return m_coroutineHandle.done();
            }

            auto takeCoroOwnership()
            {
                auto result = m_coroutineHandle;
                m_coroutineHandle = nullptr;

                return result;
            }

        private:
            std::coroutine_handle<promise_type> m_coroutineHandle;
        };

        template <typename coro_t>
        struct awaitable
        {
            coro_t &entry;

            constexpr bool await_ready() const noexcept { return false; }

            template <typename resume_t>
            constexpr void await_suspend(resume_t resume)
            {
                TaskBase::m_taskQueue.push(entry);
                TaskBase::m_resumeQueue[entry] = resume.address();
            }

            constexpr auto await_resume() const noexcept
            {
                if constexpr (!std::is_same_v<void, typename coro_t::return_type>)
                    return std::move(entry.result());
            }
        };

        struct deferrable
        {
            void *task = nullptr;
            std::chrono::milliseconds timeout;

            deferrable &&operator=(size_t timeout)
            {
                this->timeout = std::chrono::milliseconds{timeout};

                return std::move(*this);
            }
        };

        template <typename return_t, typename yield_t>
        struct task_promise_type_base
        {
            void *thisCoroutineHandle = nullptr;
            std::vector<std::conditional_t<std::is_same_v<void, yield_t>, uint8_t, yield_t>> yield_values;

            static Task<return_t, yield_t> get_return_object_on_allocation_failure() { return {nullptr}; }

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept
            {
#ifndef COROUTINE_FEATURE_COROFINALLY // !COROUTINE_FEATURE_COROFINALLY
                if (!TaskBase::m_resumeQueue.empty() && TaskBase::m_resumeQueue.contains(thisCoroutineHandle))
                {
                    TaskBase::m_taskQueue.push(TaskBase::m_resumeQueue[thisCoroutineHandle]);
                    TaskBase::m_resumeQueue.erase(thisCoroutineHandle);
                }
#else  // COROUTINE_FEATURE_COROFINALLY
                void *resumeCoro = nullptr;

                if (!TaskBase::m_resumeQueue.empty() && TaskBase::m_resumeQueue.contains(thisCoroutineHandle))
                {
                    resumeCoro = TaskBase::m_resumeQueue[thisCoroutineHandle];
                    TaskBase::m_resumeQueue.erase(thisCoroutineHandle);
                }

                // check if this coroutine is finally block
                if (nullptr != resumeCoro && 1 == reinterpret_cast<size_t>(resumeCoro) % 2)
                {
                    TaskBase::m_taskQueue.push(reinterpret_cast<uint8_t *>(resumeCoro) - 1);
                    return {};
                }

                // clean executed finally block
                while (!TaskBase::m_finallyQueue.empty() && TaskBase::m_finallyQueue.front().first.done())
                    TaskBase::m_finallyQueue.pop();

                if (!TaskBase::m_finallyQueue.empty() && !TaskBase::m_finallyQueue.back().second)
                {
                    auto &[finallyBlock, already] = TaskBase::m_finallyQueue.back();

                    if (nullptr != resumeCoro)
                        TaskBase::m_resumeQueue[finallyBlock] = reinterpret_cast<uint8_t *>(resumeCoro) + 1;

                    TaskBase::m_taskQueue.push(finallyBlock);
                    already = true;
                }
                else if (nullptr != resumeCoro)
                    TaskBase::m_taskQueue.push(resumeCoro);
#endif // !COROUTINE_FEATURE_COROFINALLY

                return {};
            }

            constexpr std::suspend_always yield_value(const std::conditional_t<std::is_same_v<void, yield_t>, uint8_t, yield_t> &result)
            {
                if constexpr (!std::is_same_v<void, yield_t>)
                    yield_values.push_back(std::move(result));

                TaskBase::m_taskQueue.push(thisCoroutineHandle);

                return {};
            }

            constexpr std::suspend_always yield_value(deferrable &&defer)
            {
                auto timeout = std::chrono::system_clock::now().time_since_epoch() + defer.timeout;

                defer.task = thisCoroutineHandle;
                defer.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
                TaskBase::m_delayQueue[timeout.count()] = defer;

                return {};
            }

            template <typename coro_t>
            constexpr auto await_transform(coro_t &&entry)
            {
                return awaitable<coro_t>{entry};
            }

            void unhandled_exception()
            {
                // TODO: use std::current_exception and std::rethrow_exception to implement
            }
        };

        template <typename return_t, typename yield_t>
        struct task_promise_type : public task_promise_type_base<return_t, yield_t>
        {
            return_t returnValue;

            Task<return_t, yield_t> get_return_object()
            {
                auto coroutineHandle = std::coroutine_handle<task_promise_type<return_t, yield_t>>::from_promise(*this);

                this->thisCoroutineHandle = coroutineHandle.address();

                return {coroutineHandle};
            }

            void return_value(const return_t &result)
            {
                returnValue = std::move(result);
            }
        };

        template <typename yield_t>
        struct task_promise_type<void, yield_t> : public task_promise_type_base<void, yield_t>
        {
            Task<void, yield_t> get_return_object()
            {
                auto coroutineHandle = std::coroutine_handle<task_promise_type<void, yield_t>>::from_promise(*this);

                this->thisCoroutineHandle = coroutineHandle.address();

                return {coroutineHandle};
            }

            void return_void() {}
        };

#ifdef COROUTINE_FEATURE_COROFINALLY // COROUTINE_FEATURE_COROFINALLY
        template <typename coro_t>
        class __CoroFinally
        {
        public:
            __CoroFinally(coro_t coro)
                : m_coro(std::move(coro))
            {
            }
            ~__CoroFinally()
            {
                TaskBase::m_finallyQueue.push({m_coro(), false});
            }

        private:
            coro_t m_coro;
        };
#endif // COROUTINE_FEATURE_COROFINALLY

        thread_local std::queue<void *> TaskBase::m_taskQueue;
        thread_local std::map<uint64_t, deferrable> TaskBase::m_delayQueue;
        thread_local std::unordered_map<void *, void *> TaskBase::m_resumeQueue;
#ifdef COROUTINE_FEATURE_COROFINALLY // COROUTINE_FEATURE_COROFINALLY
        thread_local std::queue<std::pair<Task<void, void>, bool>> TaskBase::m_finallyQueue;
#endif // COROUTINE_FEATURE_COROFINALLY
    }

    namespace task_scheduler_implement
    {
        using base = task_implement::TaskBase;

        template <typename... any_t>
        struct TaskStorage
        {
        };

        template <typename head_t, typename... tail_t>
        struct TaskStorage<head_t, tail_t...> : private TaskStorage<tail_t...>
        {
            std::invoke_result_t<head_t> task;

            TaskStorage(head_t head, const tail_t &...tails)
                : task(head()),
                  TaskStorage<tail_t...>(tails...)
            {
                base::m_taskQueue.push(task);
            }

            TaskStorage<tail_t...> &tail()
            {
                return *this;
            }
        };

        class SingleThreadScheduler
        {
        public:
            template <typename coro_t>
            constexpr static void add(const coro_t &coroutine)
            {
                base::m_taskQueue.push(static_cast<void *>(coroutine));
            }

            static void run()
            {
                for (;;)
                {
                    if (!base::m_delayQueue.empty())
                    {
                        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
                        auto cit = base::m_delayQueue.cbegin();

                        if (now > cit->second.timeout)
                        {
                            auto task = std::coroutine_handle<>::from_address(cit->second.task);
                            base::m_delayQueue.erase(cit->first);

                            if (nullptr != task && !task.done())
                                task();
                        }
                    }
                    else if (base::m_taskQueue.empty())
                        break;

                    if (base::m_taskQueue.empty())
                        continue;

                    auto task = std::coroutine_handle<>::from_address(std::move(base::m_taskQueue.front()));
                    base::m_taskQueue.pop();

                    if (nullptr == task || task.done())
                        continue;

                    task();
                }
            }

            template <typename... coro_t>
            constexpr static void runs(coro_t... corotines)
            {
                TaskStorage<coro_t...> tasks(corotines...);

                run();
            }
        };

        class MultiThreadScheduler
        {
        public:
            template <typename coro_t>
            static void add(const coro_t &coroutine)
            {
                std::unique_lock<std::mutex> locker(m_globalMutex);

                m_globalQueue.push(static_cast<void *>(coroutine));
            }

            static void run()
            {
                // check if we need to append new workers
                if (m_workers.size() < m_maxWokers)
                {
                    size_t appendCount = 0;

                    if (m_workers.size() < m_globalQueue.size())
                        appendCount = m_maxWokers < m_globalQueue.size() ? m_maxWokers - m_workers.size() : m_globalQueue.size() - m_workers.size();
                    else
                        appendCount = m_globalQueue.size() - m_channels.size();

                    for (size_t i = 0; i < appendCount; i++)
                        m_workers.emplace_back(worker);
                }
            }

            static void stop()
            {
                m_work = false;

                for (auto &worker : m_workers)
                    if (worker.joinable())
                        worker.join();
            }

        private:
            static void worker()
            {
#ifdef COROUTINE_FEATURE_EXPRIMENTAL // COROUTINE_FEATURE_EXPRIMENTAL
                channel_t channel = nullptr;
                future_t future;
#endif // COROUTINE_FEATURE_EXPRIMENTAL

                while (m_work)
                {
                    while (m_work)
                    {
                        if (!base::m_delayQueue.empty())
                        {
                            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
                            auto cit = base::m_delayQueue.cbegin();

                            if (now > cit->second.timeout)
                            {
                                auto task = std::coroutine_handle<>::from_address(cit->second.task);
                                base::m_delayQueue.erase(cit->first);

                                if (nullptr != task && !task.done())
                                    task();
                            }
                        }
                        else if (m_globalQueue.empty() && base::m_taskQueue.empty())
                            break;

                        if (m_globalQueue.empty() && base::m_taskQueue.empty())
                        {
                            std::this_thread::sleep_for(std::chrono::nanoseconds(1));
                            continue;
                        }
                        if (!m_globalQueue.empty() && m_minTasks > base::m_taskQueue.size())
                        {
                            std::unique_lock<std::mutex> locker(m_globalMutex);

                            base::m_taskQueue.push(m_globalQueue.front());
                            m_globalQueue.pop();
                        }

                        auto task = std::coroutine_handle<>::from_address(std::move(base::m_taskQueue.front()));
                        base::m_taskQueue.pop();

                        if (nullptr == task || task.done())
                            continue;

#ifdef COROUTINE_FEATURE_EXPRIMENTAL // COROUTINE_FEATURE_EXPRIMENTAL
                        if (!m_channels.empty() && m_maxTasks < base::m_taskQueue.size())
                        {
                            for (size_t i = 0; i < (std::min)(base::m_taskQueue.size(), m_channels.size()); i++)
                            {
                                std::promise<std::pair<void *, void *>> *channel = nullptr;

                                {
                                    std::unique_lock<std::mutex> locker(m_localMutex);

                                    channel = m_channels.front();
                                    m_channels.pop();
                                }

                                if (nullptr == channel)
                                    continue;

                                auto takeTask = base::m_taskQueue.front();
                                base::m_taskQueue.pop();

                                channel->set_value({takeTask, base::m_resumeQueue.contains(takeTask) ? base::m_resumeQueue.extract(takeTask).mapped() : nullptr});
                            }
                        }
#endif // COROUTINE_FEATURE_EXPRIMENTAL

                        task();
                    }

#ifdef COROUTINE_FEATURE_EXPRIMENTAL // COROUTINE_FEATURE_EXPRIMENTAL
                    if (m_globalQueue.empty() && base::m_taskQueue.empty())
                    {
                        if (nullptr == channel)
                        {
                            channel = new std::promise<std::pair<void *, void *>>;
                            future = channel->get_future();

                            {
                                std::unique_lock<std::mutex> locker(m_localMutex);

                                m_channels.push(channel);
                            }
                        }

                        if (std::future_status::ready == future.wait_for(std::chrono::milliseconds(1000)))
                        {
                            auto [task, resume] = future.get();

                            base::m_taskQueue.push(task);
                            if (nullptr != resume)
                                base::m_resumeQueue.emplace(task, resume);

                            delete channel;
                            channel = nullptr;
                        }
                    }
#endif // COROUTINE_FEATURE_EXPRIMENTAL
                }

#ifdef COROUTINE_FEATURE_EXPRIMENTAL // COROUTINE_FEATURE_EXPRIMENTAL
                if (nullptr != channel)
                    delete channel;
#endif // COROUTINE_FEATURE_EXPRIMENTAL
            }

        public:
            using channel_t = std::promise<std::pair<void *, void *>> *;
            using future_t = std::future<std::pair<void *, void *>>;

            constexpr static size_t m_minTasks = 2, m_maxTasks = 3; // minimun and maximun size of local task queue
            static size_t m_maxWokers;                              // maximun workers
            static bool m_work;                                     // control the worker thread
            static std::mutex m_globalMutex, m_localMutex;          // queue lock
            static std::vector<std::thread> m_workers;              // worker threads
            static std::queue<void *> m_globalQueue;                // initial task queue
            static std::queue<channel_t> m_channels;                // the channels used for stealing other thread's local task
        };

        size_t MultiThreadScheduler::m_maxWokers = std::thread::hardware_concurrency() + 3;
        bool MultiThreadScheduler::m_work = true;
        std::mutex MultiThreadScheduler::m_globalMutex, MultiThreadScheduler::m_localMutex;
        std::vector<std::thread> MultiThreadScheduler::m_workers;
        std::queue<void *> MultiThreadScheduler::m_globalQueue;
        std::queue<MultiThreadScheduler::channel_t> MultiThreadScheduler::m_channels;
    }

    using task_base = task_implement::TaskBase;
    template <typename return_t = void, typename yield_t = void>
    using task = task_implement::Task<return_t, yield_t>;

    using STScheduler = task_scheduler_implement::SingleThreadScheduler;
    using MTScheduler = task_scheduler_implement::MultiThreadScheduler;
    using scheduler = MTScheduler;
}

#define co_delay co_yield coroutine::task_implement::deferrable{} =

#define co_switch \
    co_yield {}

#ifdef COROUTINE_FEATURE_COROFINALLY // COROUTINE_FEATURE_COROFINALLY
#define _COROMAKELAMBDA(x, y) x##y
#define COROMAKELAMBDA(x, y) _COROMAKELAMBDA(x, y)
#define co_finally coroutine::task_implement::__CoroFinally COROMAKELAMBDA(lambda, __COUNTER__) = [&]() -> coroutine::task_implement::Task<void, void>
#endif // COROUTINE_FEATURE_COROFINALLY

#endif // !COROUTINE_H