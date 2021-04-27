#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);
private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();
private:
    // 线程的数量
    int m_thread_number;

    // 描述线程池的数组，大小为m_thread_number
    pthread_t *m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量
    int m_max_requests;

    // 请求队列
    std::list<T *> m_workqueue;

    // 保护请求队列的互斥锁
    locker m_queuelocker;

    // 是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) 
: m_thread_number(thread_number), m_max_requests(max_requests),m_stop(false), m_threads(NULL)
{

    if ((thread_number <= 0) || (max_requests <= 0))
    {
        throw std::exception();
    }
    ///new线程
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for (int i = 0; i < thread_number; ++i)
    {
        //printf("create the %dth thread\n", i);
        ///创建线程
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        ///设置线程脱离
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;

    ///使得所有的线程都停止run函数
    m_stop = true;
    
}

///添加连接请求到请求队列
template <typename T>
bool threadpool<T>::append(T *request)
{
    
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    ///信号量在这里表示等待处理的事件数量
    m_queuestat.post();
    return true;
}

///线程创建的传参是void *,所以传参this
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)///如果没有停止
    {
        m_queuestat.wait();///如果请求队列里没东西就阻塞在这,直到请求队列有东西为止
        m_queuelocker.lock();///加锁
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        ///取出一个http请求
        T *request = m_workqueue.front();
        m_workqueue.pop_front();

        //解锁
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        request->process();
    }
}

#endif
