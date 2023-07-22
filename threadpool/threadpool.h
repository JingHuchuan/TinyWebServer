#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

// 线程池类，将它定义为模板类是为了代码复用。模板参数T是任务类
template<typename T>
class threadpool {
public:
    // 参数thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量，connPool是数据库连接池指针，actor_model是模型切换
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);      //构造函数定义，此时可以有初始值
    ~threadpool();

    // 往请求队列中添加任务请求
    bool append(T* request, int state);
    bool append_p(T* request);              // 先前的版本

private:
    // 工作线程运行的函数，它不断从工作队列中取出任务并执行之
    // 线程的工作函数，在c++中必须是static
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;                    // 线程池中的线程数
    int m_max_requests;                     // 请求队列中允许的最大请求数
    pthread_t* m_threads;                   // 描述线程池的数组，其大小为m_thread_number
    std::list<T*> m_workqueue;              // 请求队列
    locker m_queuelocker;                   // 保护请求队列的互斥锁
    sem m_queuestat;                        // 是否有任务需要处理
    connection_pool* m_connPool;            // 数据库连接池
    int m_actor_model;                      // 模型切换       
};

template<typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : 
    m_actor_model(actor_model), m_thread_number(thread_number),
    m_max_requests(max_requests), m_threads(NULL), m_connPool(connPool) {
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw  std::exception();
    }

    // 线程id初始化
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建thread_number个线程，并将他们都设置为脱离线程
    for (int i = 0; i < thread_number; ++i) {
        // printf("create the %dth thread\n", i);
        // 循环创建线程，并将工作线程按要求进行运行
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }

        // 将线程进行分离后，不用单独对工作线程进行回收
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
}

// 向请求队列中添加任务
template<typename T>
bool threadpool<T>::append(T* request, int state) {
    // 操作工作队列时一定要加锁，因为它被所有线程共享
    m_queuelocker.lock();

    // 根据硬件，预先设置请求队列的最大值
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }

    // 添加任务
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();                 // 增加一个任务就需要相应增加一个信号量，用于通知等待的线程有新的任务可用
    return true;
}

template<typename T>
bool threadpool<T>::append_p(T* request) {
    // 操作工作队列时一定要加锁，因为它被所有线程共享
    m_queuelocker.lock();

    // 根据硬件，预先设置请求队列的最大值
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }

    // 添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();                 // 增加一个任务就需要相应增加一个信号量，用于通知等待的线程有新的任务可用
    return true;
}

// 线程处理函数
template<typename T>
void* threadpool<T>::worker(void* arg) {
    // worker函数是static的，不能直接访问非statice的成员，worker的参数arg就是this，可以通过强制转换得到
    threadpool* pool = (threadpool*) arg;
    pool->run();
    return pool;
}

// run执行任务
template<typename T>
void threadpool<T>::run() {
    while (true) {
        // 使用m_queuestat.wait()判断信号量是否有值，如果有值的话信号量就减1，如果没有值就阻塞在这里
        m_queuestat.wait();

        // 被唤醒后先加互斥锁
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            // 如果请求队列为空的话，不进行处理
            m_queuelocker.unlock();
            continue;
        }

        T* request = m_workqueue.front();     // 获取第一个任务
        m_workqueue.pop_front();              // 在工作队列中删除
        m_queuelocker.unlock();

        if (!request) {
            continue;                         // 如果没有请求到，就continue
        }

        // Reactor模式
        if (m_actor_model == 1) {
            // request对象可读
            if (request->m_state == 0) {
                // 从socket中读取数据
                if (request->read_once()) {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);     // 从数据库连接池中取一个连接给到request对象
                    request->process();                                       // process（模板类中的方法，这里是http_conn类）进行处理
                }
                else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else {
                // 向socket写数据
                if (request->write()) {
                    request->improv = 1;
                }
                else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }

        // Proactor模式
        else {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}

#endif