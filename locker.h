#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
/*C++标准异常头文件*/
#include <exception>
/*信号量头文件*/
#include <semaphore.h>

/*线程同步机制的封装类：信号量、互斥量（锁）、条件变量*/

/*互斥锁类*/
class locker {
public:
    /*构造*/
    locker() {
        /*初始化互斥锁，成功返回0，错误返回错误码*/
        if(pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }
    /*析构*/
    ~locker() {
        /*销毁互斥锁*/
        pthread_mutex_destroy(&m_mutex);
    }
    /*加锁*/
    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    /*解锁*/
    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    /*获取互斥量成员*/
    pthread_mutex_t* get() {
        return &m_mutex;
    }

private:
    /*定义互斥锁类型的变量（互斥量）m_mutex，类型是pthread_mutex_t结构体*/
    pthread_mutex_t m_mutex;  
};

/*条件变量类*/
class cond {
public:
    /*构造*/
    cond() {
        if(pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }
    /*析构*/
    ~cond() {
        pthread_cond_destroy(&m_cond);
    }
    
    bool wait(pthread_mutex_t* mutex) {
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }

    bool timedwait(pthread_mutex_t* mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }
    
    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    /*定义条件变量*/
    pthread_cond_t m_cond;
};

/*信号量类*/
class sem {
public:
    /*两种构造*/
    sem() {
        if(sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }
    sem(int num) {
        if(sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }        
    }
    /*析构，销毁信号量*/
    ~sem() {
        sem_destroy(&m_sem);
    }
    /*等待信号量*/
    bool wait() {
        return sem_wait(&m_sem) == 0;
    }
    /*增加信号量*/
    bool post() {
        return sem_post(&m_sem) == 0;
    }
private:
    sem_t m_sem;

};


#endif