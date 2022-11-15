//第八章：epoll开发web服务器代码
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include "threadpool.h"
#include "timer.cpp"
#include "http_conn.h"
#include "sql_pool.h"


#define FD_LIMIT 65536           //最大文件描述符
#define TIMESLOT 5             //最小超时单位
#define MAX_PTHREAD 8   //最大线程数


//全局变量
static int pipefd[2];
static sort_timer_lst timer_list;
static int epollfd = 0;

//如果想在本源文件(例如文件名A)中使用另一个源文件(例如文件名B)的函数，就要使用extern+函数原型（extern可省略）
extern void addfd(int epollfd, int fd, bool one_shot);
extern void setnonblocking(int fd);

//信号相关----------
//信号处理函数
void sig_handler(int sig)
{
    //信号处理函数只负责往管道写端写数据
    int errno_temp = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = errno_temp;
}

//注册信号处理函数
void addsig(int sig, void(handler)(int))
{
    struct sigaction act;
    memset(&act, '\0', sizeof(act));
    act.sa_handler = handler;
    act.sa_flags |= SA_RESTART;
    sigfillset(&act.sa_mask);
    assert( sigaction(sig, &act, NULL)!=-1);
}

//设置定时器回调函数，用于关闭非活跃的connfd
void cb_func(client_data * user_data)
{
    //用于关闭非活跃的connfd
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;

}

int main()
{

    //需要修改的数据库信息,登录名,密码,库名
    string db_ip = "localhost";
    string db_user = "root";
    string db_passwd = "Aa82981388";
    string db_databasename = "Tinywebserver";

    //将SIGPIPE信号设置成忽略。
    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    Connection_pool * connPool = Connection_pool::GetInstance();
    connPool->init(db_ip, db_user, db_passwd, db_databasename, 3306, MAX_PTHREAD);

    //创建线程池
    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch(...)
    {
        cout<<"创造线程池失败"<<endl;
        return -1;
    }

    //创建http类的对象
    http_conn * users_http = new http_conn[FD_LIMIT];
    assert(users_http);

    //初始化数据池的map容器：从数据库中保存账号和密码到map
    users_http->initmysql_result(connPool);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //设置端口复用
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    //绑定
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(8888);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    int ret = bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    assert(ret >= 0);


    //监听
    ret = listen(listenfd, 128);
    assert(ret >= 0);

    //构建事件结构体数组，作为传出参数用于接收epoll_wait中发生变化的事件
    struct epoll_event event[10000];
    epollfd =  epoll_create(1);
    assert(epollfd != -1);

    //将listenfd挂上树
    addfd(epollfd, listenfd, false);

    //将上述epollfd赋值给http类对象的m_epollfd属性
    http_conn::m_epollfd = epollfd;


    //统一信号源
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);

    //添加到epoll中， 统一事件源
    addfd(epollfd, pipefd[0], false);

    /*设置信号处理函数*/
    //若web服务器给浏览器发送数据的时候, 浏览器已经关闭连接,则web服务器就会收到SIGPIPE信号
    addsig(SIGTERM,sig_handler);
    addsig(SIGALRM, sig_handler);
    bool stop_server = false;

    //用户的数据结构，用于存:用户的socket地址，文件描述符，读缓冲，定时器
    client_data * users_timer = new client_data[FD_LIMIT];

    bool timeout = false;
    alarm(TIMESLOT);//开始定时

    while(!stop_server)
    {
        //委托内核监控
        int nready = epoll_wait(epollfd, event, 1024, -1);
        if(nready < 0 && errno != EINTR )
        {
            break;
        }

        //四个事件：1、新客户的连接到，2、客户的数据到， 3、信号源 4、错误信息 5、EPOLLOUT
        for(int i=0; i<nready; ++i)
        {
            int sockfd = event[i].data.fd;
            //1、新客户的连接到
            if(sockfd == listenfd)
            {
                cout<<"新客户的连接到！！"<<endl;
                //ET的情况的话需要使用while循环接收
                struct sockaddr_in cliaddr;
                socklen_t len = sizeof(cliaddr);
                while(1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &len);

                    if(connfd < 0)
                        break;
                    //考虑连接满的情况
                    if(http_conn::m_user_count >= FD_LIMIT)
                        break;

                    //对connfd的http对象初始化：包括注册epoll事件，等
                    users_http[connfd].init(connfd, cliaddr);

                    //创建定时器，设置回调函数与超时事件，将定时器与用户绑定，最后添加到定时器链表中
                    users_timer[connfd].address = cliaddr;
                    users_timer[connfd].sockfd = connfd;
                    util_timer * timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    timer->expire = time(NULL)+3*TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_list.add_timer(timer);
                }
                continue;
            }

            //2、异常事件处理:
            //EPOLLRDHUP：对面close事件
            //EPOLLHUP：意外出错事件
            //EPOLLERR：错误事件
            else if(event[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                cout<<"异常事件到！！！"<<endl;
                //断开连接，注意要删除事件和计时器节点(优化：删除计时器的节点是不是可以写到回调函数里.以优化)
                util_timer *timer = users_timer[sockfd].timer;
                users_timer[sockfd].timer->cb_func(&users_timer[sockfd]);
                if(timer)
                    timer_list.del_timer(timer);
            }

            //2、信号源
            else if(sockfd == pipefd[0] && event[i].events & EPOLLIN)
            {
                //可能不止来一个信号
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1)
                {
                    continue;
                }
                else if(ret == 0)
                {
                    continue;
                }
                else
                {
                    /* 每个信号值占1字节，所以按字节来接收信号 */
                    for(int i=0; i<ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM://计时器信号
                            {
                                //用timeout标记有定时任务要处理，但是不着急处理，因为优先级不高
                                timeout = true;
                                break;
                            }
                            case SIGTERM://终止信号，由终端发出通过kill命令发出
                            {
                                stop_server = true;
                                break;
                            }
                        }
                    }
                }
            }

            //4、客户的数据到
            else if(event[i].events  & EPOLLIN)
            {
                cout<<"客户的数据到到！！！"<<endl;
                //读数据（模拟proactor）

                util_timer * timer = users_timer[sockfd].timer;
                //如果返回true(读到没有数据读了)
                if(users_http[sockfd].read())
                {
                    cout<<"开始添加数据！！！"<<endl;
                    pool->append(users_http + sockfd);//将发生读事件的http对象指针放入请求队列&users_http[sockfd]
                    //若有数据传输，则重新更新用户的定时器的时间
                    //并对新的定时器在链表上的位置进行调整

                    if(timer)
                    {
                        timer->expire = time(NULL) + 3 * TIMESLOT;
                        timer_list.adjust_timer(timer);
                    }

                }

                //如果返回false（buf空间不够或者对方关闭连接）
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_list.del_timer(timer);
                    }
                }
            }

            //5、EPOLLOUT
            else if(event[i].events & EPOLLOUT)
            {
                cout<<"EPOLLOUT到！！！"<<endl;

                util_timer * timer = users_timer[sockfd].timer;
                if(users_http[sockfd].write())//如果返回true，有两种可能：1、缓冲区写满了，需要重新等待对方读完在写。2、数据写完了，但是浏览器的请求为长连接
                {
                    //更新计时器
                    if(timer)
                    {
                        timer->expire = time(NULL) + 3 * TIMESLOT;
                        timer_list.adjust_timer(timer);
                    }
                }
                else//返回false，有两种可能：1、发送失败 2、发送成功了，但是浏览器请求为短连接
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                        timer_list.del_timer(timer);
                }
            }

        }//主体的for循环

        //最后处理定时事件，因为I/O事件有更高的优先级。
        if(timeout)
        {
            timeout = false;
            //检查是否有超时的连接
            timer_list.tick();
            //重新打开计时器
            alarm(TIMESLOT);
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users_http;
    delete[] users_timer;
    delete pool;
    return 0;
}


