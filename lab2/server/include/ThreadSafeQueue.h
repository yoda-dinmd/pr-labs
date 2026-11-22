// DAY 1 - thread safe queue, atomics, conditional variables, locks

#ifndef THREADSAFEQUEUE
#define THREADSAFEQUEUE

#include <utility>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <optional>

template<typename T>
class ThreadSafeQueue
{
private:
    std::queue<T> m_queue{};
    std::mutex m_mutex{};
    std::condition_variable m_cv{};
    bool m_closed{false};

public:
    bool push(T value)
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_closed)
                return false;
            m_queue.push(std::move(value));
        }
        m_cv.notify_one();
        return true;
    }

    std::optional<T> wait_and_pop()
    {
        std::unique_lock lock{m_mutex};

        m_cv.wait(lock, [&] {
            return m_closed || !m_queue.empty();
        });

        if (m_closed && m_queue.empty())
            return std::nullopt;
        T value{std::move(m_queue.front())};
        m_queue.pop();
        return value;
    }

    void close()
    {
        {
            std::lock_guard lock{m_mutex};
            m_closed = true;
        }

        m_cv.notify_all();
    }
};


#endif
