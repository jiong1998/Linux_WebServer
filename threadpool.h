#ifndef THREADPOOL_H
#define THREADPOOL_H
//半同步/半反应堆模式
//主线程负责数据读写，当数据读写完了后，插入请求队列，由工作线程竞争获得。
#include "locker.h"
#include <pthread.h>
#include <list>
#include <exception>
#include "sql_pool.h"

template<typename T>

//请求队列中的每一个任务其实就是一个个的http_con对象
class threadpool
{
public:
    threadpool(Connection_pool * connPool, int thread_number = 8, int max_requests = 10000);//初始化线程，设置分离属性
    threadpool(const threadpool<T> & threadpool1)
    {
        cout<<"调用了拷贝构造函数"<<endl;
    }
    ~threadpool();

    bool append(T * request);//在请求队列中插入任务
private:
    int m_thread_number;//线程池中的线程数
    int m_max_requests;//请求队列的最大长度
    pthread_t * m_pthread_id;
    std::list<T *> m_workqueue;//请求队列
    locker m_queuelocker;//请求队列的互斥锁
    sem m_queuestat;//请求队列的信号量
    bool m_stop;//停止线程标志
    Connection_pool * m_connPool;//数据池


private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* work(void *arg);//线程运行的函数，其中调用了run
    void run();
};


template<typename T>
//初始化线程，设置分离属性
threadpool<T>::threadpool(Connection_pool * connPool, int thread_number, int max_requests): m_connPool(connPool), m_thread_number(thread_number), m_max_requests(max_requests),m_pthread_id(NULL), m_stop(
        false)
{
    //参数异常判断
    if(( thread_number <= 0 )|| ( max_requests<=0 ))
        throw std::exception();

    //创建线程号数组
    m_pthread_id = new pthread_t[m_thread_number];
    if(!m_pthread_id)
        throw std::exception();
    for(int i=0; i<m_thread_number; ++i)
    {
        //创建m_thread_number个子线程
        if(pthread_create(&m_pthread_id[i], NULL, work, this))
        {
            delete [] m_pthread_id;
            throw std::exception();
        }
        else
            cout<<"子线程创建创建成功!"<<endl;

        //设置分离属性
        if(pthread_detach(m_pthread_id[i]))
        {
            delete [] m_pthread_id;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    cout<<"调用析构函数"<<endl;
    delete [] m_pthread_id;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T *request)
{
    //添加任务到请求队列。请求队列是共享资源，注意互斥问题
    m_queuelocker.lock();//这里出现段错误。其实不是这里粗错，把加锁解锁注释掉后还是会段错误，所以应该是任务粗错了

    if(m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    cout<<"开始push！"<<endl;
    m_workqueue.push_back(request);//就是这里出错了
    cout<<"添加成功！"<<endl;
    m_queuelocker.unlock();
    m_queuestat.post();//信号量+1，通过信号量传递任务多少
    return true;
}

template<typename T>
//线程运行的函数，其中调用了run
void* threadpool<T>::work(void *arg)
{
    threadpool * pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run()
{
    //线程将持续不断的运行这个函数。
    while(!m_stop)
    {
        m_queuestat.wait();//持续在这阻塞，直到有任务进来，会竞争获取
        cout<<"一个子线程解除解除阻塞"<<endl;
        //当取消阻塞时，说明可以获取任务了
        m_queuelocker.lock();

        if(m_workqueue.empty())//判断队列是否空，一般能执行到这都不会空把，因为有信号量？
        {
            m_queuelocker.unlock();
            continue;
        }

        //任务其实就是http对象
        T * task = m_workqueue.front();
        m_workqueue.pop_front();

        m_queuelocker.unlock();

        //如果任务不存在
        if( !task )
            continue;

        //源码中这里获取了连接池的连接，确实是需要，
        ConnectionRAII mysqlcom = ConnectionRAII(&task->mysql, m_connPool);


        //运行该任务的执行函数。在本项目中，每个任务对应一个http_conn对象，也就是执行http_conn对象的执行函数。
        cout<<"task->process();"<<endl;
        task->process();
    }
}
#endif