#ifndef PS2_FAST_SYNC_H
#define PS2_FAST_SYNC_H

// Mutex and condition variable for the busiest thread hand-offs (game thread -> VU1 thread,
// VU1/game thread -> GS feed thread).
//
// On Windows these are the system's slim reader/writer locks and condition variables. The
// MinGW std::mutex / std::condition_variable go through winpthreads, whose condition variables
// wake a thread with a kernel semaphore and take extra internal locks on every signal and wait;
// with tens of thousands of hand-offs a second that showed up as real CPU time.
// Elsewhere they are the std:: types.

#include <condition_variable>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ps2sync
{
#if defined(_WIN32)
    class Mutex
    {
    public:
        Mutex() = default;
        Mutex(const Mutex &) = delete;
        Mutex &operator=(const Mutex &) = delete;

        void lock() noexcept { AcquireSRWLockExclusive(&m_lock); }
        bool try_lock() noexcept { return TryAcquireSRWLockExclusive(&m_lock) != 0; }
        void unlock() noexcept { ReleaseSRWLockExclusive(&m_lock); }
        SRWLOCK *native() noexcept { return &m_lock; }

    private:
        SRWLOCK m_lock = SRWLOCK_INIT;
    };

    class CondVar
    {
    public:
        CondVar() = default;
        CondVar(const CondVar &) = delete;
        CondVar &operator=(const CondVar &) = delete;

        void notify_one() noexcept { WakeConditionVariable(&m_cv); }
        void notify_all() noexcept { WakeAllConditionVariable(&m_cv); }

        void wait(std::unique_lock<Mutex> &lock)
        {
            SleepConditionVariableSRW(&m_cv, lock.mutex()->native(), INFINITE, 0);
        }

        template <class Predicate>
        void wait(std::unique_lock<Mutex> &lock, Predicate predicate)
        {
            while (!predicate())
                wait(lock);
        }

    private:
        CONDITION_VARIABLE m_cv = CONDITION_VARIABLE_INIT;
    };
#else
    using Mutex = std::mutex;
    using CondVar = std::condition_variable;
#endif
}

#endif
