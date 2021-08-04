#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"

/*线程池模板类，为了代码的复用*/
/*模板参数T就是任务类*/
template<typename T>
class threadpool {
public:
    threadpool(int thread_number = 8, int max_requests = 10000);  // 构造函数，初始化线程数量和最大请求数量
    ~threadpool();                // 析构
    bool append(T* request);      // 添加任务

private:
    /*线程的工作函数worker()定义——函数指针*/
    /*必须是静态成员函数，因为非静态会有this指针，导致调用时参数个数不匹配*/
    static void* worker(void* arg);
    /*线程池工作函数run()的定义*/
    /*从工作队列中取数据*/
    void run();

private:
    int m_thread_number;         // 成员1:线程的数量
    pthread_t* m_threads;        // 成员2:线程池数组，大小为m_thread_number，存放线程ID
    int m_max_requests;          // 成员3:请求队列中最多允许的，等待处理的请求数量
    std::list<T*> m_workqueue;   // 成员4:请求队列
    locker m_queuelocker;        // 成员5:互斥锁
    sem m_queuestat;             // 成员6:信号量，用来判断是否有任务需要处理
    bool m_stop;                 // 成员7:是否结束线程
};

/*类模板的构造函数在类外实现*/
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL) {
        if(thread_number <= 0 || max_requests <= 0) {  // 传入的初始化参数合法性判断
            throw std::exception();
        }
        m_threads = new pthread_t[m_thread_number];    // 创建线程池数组并判断是否创建成功
        if(!m_threads) {
            throw std::exception();
        }
        /*创建thread_number个线程，并将它们设置为线程脱离(线程结束后自己释放资源)*/
        for(int i = 0; i < thread_number; i++) {
            std::printf("Create the %d thread\n", i);
            /*此处将this作为参数传递给static成员函数worker()，使它可以访问到成员变量*/
            if(pthread_create(m_threads + i, NULL, worker, this) != 0) {  //将创建的线程的ID存到m_threads + i中，也就是说数组m_threads中存放了线程的ID
                /*创建失败: 释放数组，抛出异常*/
                delete[] m_threads; 
                throw std::exception();
            }
            /*创建成功后设置脱离: 设置失败，释放数组，抛出异常*/
            if(pthread_detach(m_threads[i])) {
                delete[] m_threads;
                throw std::exception();
            }
        }
    }

/*类模板的析构函数在类外实现*/
template<typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

/*类模板的成员函数在类外实现*/
/*往队列中添加任务，需用锁保证线程同步*/
template<typename T>
bool threadpool<T>::append(T* request) {
    m_queuelocker.lock();                        // 上锁
    if(m_workqueue.size() > m_max_requests) {    // 如果请求队列超出最大量了，解锁并返回false
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);              // 否则正常添加任务，并解锁
    m_queuelocker.unlock();                      // 解锁
    m_queuestat.post();                          // 信号量加1，当信号量值大于0时，其他正在调用wait()等待信号量的线程将被唤醒
    return true;
}

/*线程的工作函数worker()的实现*/
template<typename T>
void* threadpool<T>::worker(void* arg) {   // 传入的this
    threadpool* pool = (threadpool*) arg;
    pool->run();
    return pool;
}

/*线程池工作函数run()的实现*/
template<typename T>
void threadpool<T>::run() {
    while(!m_stop) {                       // 循环从list中取出任务，直到m_stop为true才停止
        m_queuestat.wait();                // 通过判断信号量是否有值来确定是否有任务可做，有的话不阻塞且信号量减1，没有的话就阻塞
        m_queuelocker.lock();              // 有任务，要操作队列(共享资源)所以上锁
        if(m_workqueue.empty()) {          // 判断请求队列是否为空，为空则解锁并继续查看队列中有无数据？
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();  // 队列中有数据，则获取队列头的任务request
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request) {                     // 没获取到任务，继续
            continue;
        }
        request->process();                // 获取到了，执行线程的任务函数
    }
}

#endif
