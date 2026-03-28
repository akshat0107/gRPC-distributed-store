#include <pthread.h>
#include<mutex>
#include<queue>
#include<iostream>
#include<vector>
#include <condition_variable>
#include<functional>

class threadpool
{
private:
    std::vector<pthread_t> threads;
    std::queue<std::function<void()>> q;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    bool shutdown;  

public:
    static void* entry_point(void* arg)
    {
        threadpool* pool = (threadpool*) arg;
        pool->worker_loop();
        return NULL;
    }

    void worker_loop()
    {
    while(1)
    {
       std::unique_lock<std::mutex> lock(m_mutex);
       m_cond.wait(lock, [this]() { return !q.empty() || shutdown; });

        if(shutdown==true && q.empty())
        {
            return;
        }

        std::function<void()> item = q.front();
        q.pop();
        lock.unlock();
        item();
}
    }

    threadpool(unsigned int num_threads):threads(num_threads),shutdown(false)
    {
        for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, entry_point, this);
        }
    }

    ~threadpool()
    {
       { 
        std::unique_lock<std::mutex> lock(m_mutex);
        shutdown=true;
        m_cond.notify_all();
       }
        for (auto& th : threads) {
            pthread_join(th, NULL);
        }

    }

    void enqueue(std::function<void()> job)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        q.push(job);
        m_cond.notify_one(); 
    }

};

