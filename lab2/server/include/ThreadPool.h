#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <ThreadSafeQueue.h>
#include <future>
#include <functional>
#include <memory>
#include <type_traits>
#include <thread>
#include <iostream>

class ThreadPool
{
private:
    ThreadSafeQueue<std::function<void()>> m_queue;
    std::vector<std::jthread> workers{};

public:

    explicit ThreadPool(size_t threadCount = 5) :
        workers{std::vector<std::jthread>(threadCount)}
    {
        run();
    }
    ~ThreadPool()
    {
        m_queue.close(); // Wakes up wait_and_pop(), which returns std::nullopt
    }

    //the insane abstraction that makes this so so cool
    auto submitTask(auto&& func)
    {
        using ReturnType = std::invoke_result_t<decltype(func)>;
        auto task{ std::make_shared<std::packaged_task<ReturnType()>>(
            std::forward<decltype(func)>(func)
        )};
        std::future<ReturnType> result{(*task).get_future()};
        m_queue.push([task]{
            (*task)();
        });
        return result;
    }

    void run()
    {
        for(size_t i{}; i < workers.size(); ++i)
        {
            workers[i] = std::jthread([this] {
            while (true) {
                auto task = m_queue.wait_and_pop();
                if (!task) {
                    break; // Queue is closed and empty — exit thread so ~jthread() can join
                }
                (*task)();
                std::cout << "Executed by: " << std::this_thread::get_id() << "\n";
            }
        });
        }
        
    }
};

#endif
