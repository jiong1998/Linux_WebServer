#include "sql_pool.h"
//sql_pool.cpp: 我把具体实现也写进sql_pool.h里了，因为如果分开写，我不会编译，后面在研究怎么办
//-----实现-------

Connection_pool *Connection_pool::GetInstance()
{
    static Connection_pool connection_pool;
    return &connection_pool;
}

Connection_pool::Connection_pool()
{
    this->m_FreeConn=0;
    this->m_CurConn=0;
}

//构造初始化：创建数据库的连接，并放入队列中
void Connection_pool::init(string IP, string User, string PassWord, string DataBaseName, int Port, int MaxConn)
{
    m_lock.lock();
    for(int i=0; i<MaxConn; ++i)
    {
        MYSQL * mysql = mysql_init(NULL);
        if(mysql == NULL)
        {
            cout<<"mysql init error"<<endl;
            exit(1);
        }

        MYSQL * conn = mysql_real_connect(mysql, IP.c_str(), User.c_str(), PassWord.c_str(), DataBaseName.c_str(), Port, NULL, 0);
        if(conn == NULL)
        {
            cout<<"mysql connect error"<<mysql_errno(conn)<<endl;
            exit(1);
        }
        else
            cout<<"mysql连接成功:"<<i<<endl;

        //更新空闲连接数量
        m_FreeConn++;
        //插入连接池
        m_conList.push_back(conn);
    }
    //信号量初始化，信号量的值初始化为连接池的已连接的数量
    m_sem = sem(m_FreeConn);

    m_MaxConn = m_FreeConn;
    m_lock.unlock();
}

//从连接池中获取一个数据库连接
MYSQL *Connection_pool::GetConnection()
{
    if(m_conList.size() == 0)
    {
        cout<<"连接池的size=0！"<<endl;
        return NULL;
    }
    m_sem.wait();//如果没有，则会阻塞在这

    m_lock.lock();

    //从连接池中获取一个连接
    MYSQL * conn = m_conList.front();
    m_conList.pop_front();

    m_FreeConn--;
    m_CurConn++;

    m_lock.unlock();
    return conn;
}

//释放连接,将连接还给连接池中
bool Connection_pool::ReleaseConnection(MYSQL *conn)
{
    if(conn == NULL)
        return false;

    m_lock.lock();
    m_conList.push_back(conn);
    m_FreeConn++;
    m_CurConn--;

    m_sem.post();
    m_lock.unlock();

    return true;
}

//获取当前空闲的连接数
int Connection_pool::GetFreeConnNumb()
{
    return m_FreeConn;
}

//销毁所有连接
void Connection_pool::DestroyPool()
{
    m_lock.lock();
    if(m_conList.size()>0)
    {
        for(list<MYSQL*>::iterator it = m_conList.begin(); it != m_conList.end(); ++it)
        {
            MYSQL * conn = *it;
            //关闭数据库的连接
            mysql_close(conn);
        }
        m_conList.clear();

        m_FreeConn=0;
        m_CurConn=0;
    }
    m_lock.unlock();
}

Connection_pool::~Connection_pool()
{
    this->DestroyPool();
//    cout<<"test"<<endl;
}

//RAII管理数据库的连接和释放：将数据库连接的获取与释放通过RAII机制封装，避免手动释放。
//具体来说就是用户用这个类来使用数据库的连接，用完以后超出作用域了，系统会自动调用析构函数回收
//不需要用户自己回收。这就是RAII的思想
//注意：在获取连接时，coon为传出参数。其中数据库连接本身是指针类型，所以conn需要通过双指针才能对其进行修改。
ConnectionRAII::ConnectionRAII(MYSQL ** conn, Connection_pool * connPool)//构造函数
{
    //conn为传出参数
    m_poolRAII =  connPool;

    *conn = m_poolRAII->GetConnection();
    m_conRAII = *conn;
}

ConnectionRAII::~ConnectionRAII()
{
    m_poolRAII->ReleaseConnection(m_conRAII);
}