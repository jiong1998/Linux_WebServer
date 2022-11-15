//
// Created by 13249091467 on 2022/11/8.
//

#ifndef REVISE_WEBSERVER_SQL_POOL_H
#define REVISE_WEBSERVER_SQL_POOL_H



#include <iostream>
#include "mysql.h"
#include <list>
#include <string>
#include "locker.h"
using namespace std;
//两个类

//单例模式
class Connection_pool
{
public:
    //API
    MYSQL *GetConnection();				 //从连接池中获取一个数据库连接
    bool ReleaseConnection(MYSQL *conn); //释放连接,将连接还给连接池中
    int GetFreeConnNumb();			    //获取当前空闲的连接数
    void DestroyPool();					 //销毁所有连接

    //单例模式
    static Connection_pool * GetInstance();

    void init(string IP, string User, string PassWord, string DataBaseName, int Port, int MaxConn); //初始化

    Connection_pool();
    ~Connection_pool();

private:
    int m_MaxConn;//最大连接数
    int m_CurConn;//当前已使用的连接数
    int m_FreeConn;//当前空闲的连接数

private:
    //锁
    locker m_lock;//互斥锁
    sem m_sem;//信号量

    //连接池
    list<MYSQL*> m_conList;

};

//RAII管理数据库的连接和释放：将数据库连接的获取与释放通过RAII机制封装，避免手动释放。
class ConnectionRAII
{
public:
    ConnectionRAII(MYSQL ** conn, Connection_pool * connPool);//获取一个数据库池中的一个连接，conn为传出参数
    ~ConnectionRAII();
public:
    Connection_pool * m_poolRAII;
    MYSQL * m_conRAII;
};


#endif //REVISE_WEBSERVER_SQL_POOL_H