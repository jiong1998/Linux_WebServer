#ifndef LOCKER_H_
#define LOCKER_H_

#include <exception>
#include <pthread.h>
#include <sys/types.h>
#include <semaphore.h>
#include <iostream>
using namespace std;

//封装信号量
class sem
{
public:
    sem_t m_sem;
    sem()
    {
        if(sem_init(&m_sem, 0, 0) != 0)
        {
            //由于构造函数没有返回值，通过抛异常来报告错误
            throw std::exception();
        }
    }
    sem(int value)
    {
        if(sem_init(&m_sem, 0, value) != 0)
        {
            //由于构造函数没有返回值，通过抛异常来报告错误
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()//信号量-1
    {
        if(sem_wait(&m_sem) != 0)
            return false;
        return true;
    }
    bool post()//信号量+1
    {
        if(sem_post(&m_sem)!=0)
            return false;
        return true;
    }
};

//封装互斥锁
class locker
{
private:
    pthread_mutex_t m_mutex;
public:
    locker()
    {

        if(pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        if(pthread_mutex_lock(&m_mutex)==0)
        {
            return true;
        }

        return false;
    }
    bool unlock()
    {
        if(pthread_mutex_unlock(&m_mutex)==0)
        {
            return true;
        }
        return false;
    }
};
#endif