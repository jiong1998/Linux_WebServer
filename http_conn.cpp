#include "http_conn.h"

//定义HTTP响应的一些状态

const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to statisfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "The requested file was not found on this server.\n";
const char * error_500_title = "Internal Error";
const char * error_500_form = "There was an unusual problem serving the request file.\n";

//网站的根目录,当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char * doc_root = "/root/TinyWebServer/root1";

//将表中的用户名和密码放入map
//map<string, string> users;
locker m_lock;
map<string, string> m_users;

void http_conn::initmysql_result(Connection_pool * connPool)
{
    //先从连接池获取一个连接
    MYSQL * mysql = NULL;
    ConnectionRAII mysql_con = ConnectionRAII(&mysql, connPool);

    //得到连接后可以开始执行sql语句查询表
    int ret = mysql_query(mysql, "select username,passwd from user");
    if(ret != 0)
    {
        cout<<"mysql_query 失败"<<endl;
    }

    //获取结果集
    MYSQL_RES * results = mysql_store_result(mysql);

    //获取列数
    unsigned int row_num = mysql_num_fields(results);

    //获取结果集中每一行记录
    while( MYSQL_ROW row = mysql_fetch_row(results) )
    {
        m_users[row[0]] = row[1];
    }
}

//设置文件描述符为非阻塞
void setnonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//将文件描述符的EPOLLIN和EPOLLET事件注册到epollfd的epoll内核事件表中,参数oneshot为是否要设置EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
       removefd(m_epollfd, m_sockfd);
       m_sockfd = -1;
       m_user_count --;   //关闭一个连接时, 将客户总量减一
    }
}


void http_conn::init(int sockfd, const sockaddr_in &addr)
{

    m_sockfd = sockfd;
    m_address = addr;
    //如下两行是为了避免TIME_WAIT状态， 仅用于调试，实际使用时应该去掉
//    int reuse = 1;
//    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count ++;
    init();

}

void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);


}


//从状态机，用于解析出一行内容
http_conn::LINE_STATUS http_conn::parse_line()//这里有问题
{
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') 
        {
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') 
            {
                m_read_buf[m_checked_idx ++] = '\0';
                m_read_buf[m_checked_idx ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') 
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx -1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


//循环读取客户数据， 直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE) //如果空间不够
    {
        return false;
    }

    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) 
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) //如果没有数据读了，则返回true
            {
                break;
            }
            return false;
        }
        else if (bytes_read == 0) 
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}


//解析HTTP请求行， 获得请求方法、目标URL， 以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url) 
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0 )
    {
        m_method = GET;
    }
    else if(strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version) 
    {
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) 
    {
        return BAD_REQUEST;
    }
    if (strncasecmp(m_url, "http://", 7) == 0) 
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') 
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}


//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //遇到空行， 表示头部字段解析完毕
    if (text[0] == '\0') 
    {
        //如果HTTP请求有消息体， 则还需要读取m_content_length字节的消息体，
        //状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) 
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }

    //处理Connection头部字段
    else if (strncasecmp(text, "Connection:", 11)  == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) 
        {
            m_linger = true;
        }
    }

    //处理Content-Length头部字节
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) 
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unknow header %s\n", text);
    }
    
    return NO_REQUEST;
}


//解析HTTP请求信息体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;//text保存了账号和密码

        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机，通过主、从状态机对请求报文进行解析。这个函数是专门用来解析请求报文的
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char * text = 0;
    cout<<"process_read"<<endl;

    //判断条件，这里就是从状态机驱动主状态机
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status == parse_line()) == LINE_OK))
        {
            text = get_line();
            m_start_line = m_checked_idx;;

            switch (m_check_state)
            {
                case CHECK_STATE_REQUESTLINE://解析请求行
                {
                    ret = parse_request_line(text);
                    if (ret == BAD_REQUEST) //请求报文语法错误
                    {
                        return BAD_REQUEST;
                    }
                    cout<<"请求行分析完毕"<<endl;
                    break;
                }
                case CHECK_STATE_HEADER://解析头部字段
                {
                    ret = parse_headers(text);
                    if (ret == BAD_REQUEST) //请求报文语法错误
                    {
                        return BAD_REQUEST;;
                    }
                    else if (ret == GET_REQUEST) //获取了完整的http请求，也就是GET
                    {
                        cout<<"头部字段分析完毕"<<endl;
                        return do_request();//对http请求进行执行具体的业务功能
                    }
                    break;
                }
                case CHECK_STATE_CONTENT://解析信息体，仅用于解析POST请求
                {
                    ret = parse_content(text);

                    if (ret == GET_REQUEST) //完整解析POST请求后，跳转到报文响应函数
                    {
                        return do_request();//对http请求进行执行具体的业务功能
                    }

                    //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                    line_status = LINE_OPEN;
                    break;
                }
                default:
                {
                    return INTERNAL_ERROR;
                }
            }
        }
    return NO_REQUEST;
}

/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，
 * 对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功*/
//发送的内容都存在m_iv中！
http_conn::HTTP_CODE http_conn::do_request()
{
    //所请求的有八种可能， 最后一种是get
    cout<<"do_request"<<endl;
    //m_real_file:客户请求的目标文件的完整路径，其内容等于doc_root + m_url,
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    const char * p = strrchr(m_url, '/');//p为"/xxx"，具体来说也就是/加上请求的资源

    if(cgi==1 && ( ( *(p + 1) == '2' ) || ( *(p+1) == '3') ))//如果是进行登录校验或者进行注册校验。此时m_string是账号密码
    {
        //如果是登陆校验或者注册校验

        //标志flag用于判断是登陆还是注册
        char flag = m_url[1];

        //m_url_real获取用户请求的真实的文件路径（url）
        char * m_url_real = (char *)malloc(sizeof(char)*200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url+2);

        cout<<"http_conn.cpp"<<"m_url_real="<<m_url_real<<endl;

        //m_real_file:客户请求的目标文件的完整路径，其内容等于doc_root + m_url,
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);//这是为何。这里的m_url_real是什么
        free(m_url_real);

        //获取用户名和密码, 在m_string中
        //格式：user=123&password=123
        char name[100], password[100];

        memset(name,0x00, sizeof(name));
        memset(password, 0x00, sizeof(password));

        int i = 0 + strlen("user=");//m_string的索引
        int j = 0;// name 和password的索引
        for(; m_string[i] != '&'; ++i, ++j)
        {
            name[j] = m_string[i];
        }
        name[j] = '\0';

        i = i + strlen("&password=");
        j = 0;
        for(; m_string[i] != '\0'; ++i, ++j)
        {
            password[j] = m_string[i];
        }
        password[j] = '\0';



        //如果是登陆
        if(flag == '2')
        {
            //思路：查找m_users表，是否该用户，且密码正确，如果有 返回登陆成功的html，否则返回登陆失败的html

            //如果验证成功跳转到welcome.html
            if(m_users.find(name) != m_users.end() && m_users[name] == password)
                strcpy(m_url, "/log.html");

            //如果验证失败跳转到logError.html
            else
                strcpy(m_url, "/logError.html");
        }

        //如果是注册
        else
        {
            //判断map中能否找到重复的用户名
            if(m_users.find(name) == m_users.end())
            {
                //如果用户名没有重复，插入数据库
                char *sql_insert = (char *)malloc(sizeof(char) * 200);
                strcpy(sql_insert, "insert into user(username, passwd) values (" );
                strcat(sql_insert, "'");
                strcat(sql_insert, name);
                strcat(sql_insert, "', '");

                strcat(sql_insert, password);
                strcat(sql_insert, "')");

                m_lock.lock();
                int ret = mysql_query(mysql, sql_insert);
                m_users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                //判断新注册的用户是否插入成功
                //校验成功，跳转登录页面
                if (ret == 0)
                    strcpy(m_url, "/log.html");

                //校验失败，跳转注册失败页面
                else
                    strcpy(m_url, "/registerError.html");

            }
            else//如果用户名重复则返回注册失败html
                strcpy(m_url, "/registerError.html");
        }
    }

    if( *( p +1 ) == '0' )//POST请求，跳转到register.html，即注册页面
    {
        //m_url_real获取用户请求的真实的文件路径（url）
        char * m_url_real = (char *)malloc(sizeof(char)*200);
        strcpy(m_url_real, "register.html");

        //m_real_file:客户请求的目标文件的完整路径，其内容等于doc_root + m_url
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1')//GET请求，跳转到log.html，即登录页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')//POST请求，图片请求页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')//POST请求，视频请求页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')//POST请求，跳转到fans.html，即关注页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
    //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        strncpy(m_real_file + len, m_url, FILENAME_LEN-len-1);

    cout<<"m_real_file="<<m_real_file<<endl;

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    //避免文件描述符的浪费和占用
    close(fd);

    //表示请求文件存在，且可以访问
    return FILE_REQUEST;

}

/*对内存映射区执行munmap操作*/
void http_conn::unmap()
{
    if (m_file_address) 
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写http响应
bool http_conn::write()
{
    cout<<"write"<<endl;
    int temp = 0;

    int newadd = 0;

    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0) 
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        cout<<"write"<<endl;
        return true;

    }

    while(1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //正常发送，temp为发送的字节数
        if (temp > 0)
        {
            //更新已发送字节
            bytes_have_send += temp;
            //偏移文件iovec的指针
            newadd = bytes_have_send - m_write_idx;
        }

        if (temp <= -1) 
        {
            //判断缓冲区是否满了
            if (errno == EAGAIN)
                {
                //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
                if (bytes_have_send >= m_iv[0].iov_len)
                {
                    //不再继续发送头部信息
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;
                }
                //继续发送第一个iovec头部信息的数据
                else
                {
                    m_iv[0].iov_base = m_write_buf + bytes_to_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                cout<<"write"<<endl;
                return true;

                }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            cout<<"write"<<endl;
            return false;
        }

        //更新已发送字节数
        bytes_to_send -= temp;

        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();

            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            //浏览器的请求为长连接
            if (m_linger)
            {
                //重新初始化HTTP对象
                init();
                cout<<"write"<<endl;
                return true;
            }
            else
            {
                cout<<"write"<<endl;
                return false;
            }
        }

    }
}

//往写缓冲中写入代发送数据
bool http_conn::add_response(const char * format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE) 
    {
        return false;
    }
    //定义可变参数列表
    va_list arg_list;
    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    //更新m_write_idx位置
    m_write_idx += len;
    //清空可变参列表
    va_end(arg_list);
    return true;
}

//添加状态行
bool http_conn::add_status_line(int status, const char * title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}

//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true)?"keep-alive" : "close");
}

//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//添加文本content
bool http_conn::add_content(const char * content)
{
    return add_response("%s", content);
}

/*根据服务器处理HTTP请求的结果，也就是主状态机的状态，决定返回给客户端的内容*/
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        case INTERNAL_ERROR://服务器内部错误，一般不会触发
        {
            //状态行
            add_status_line(500, error_500_title);
            //消息报头
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
            {
                return false;
            }
            break;
            }
        case BAD_REQUEST://请求报文语法出错
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form))
            {
                return false;
            }
            break;
        }
        //资源没有访问权限，403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
            {
                return false;
            }
            break;
        }
//        case NO_RESOURCE://请求不完整，需要继续读取请求报文数据
//        {
//            add_status_line(404, error_404_title);
//            add_headers(strlen(error_404_form));
//            if (!add_content(error_404_form))
//            {
//                return false;
//            }
//            break;
//        }

        //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            //如果请求的资源存在
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len  = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv_count = 2;
                return true;
            }
            //如果请求的资源大小为0，则返回空白html文件
            else
            {
                const char * ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }

    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;

}


//线程池中的线程执行的回调函数
void http_conn::process()
{
    cout<<"0"<<endl;
    //分析请求报文
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) //如果读取的请求报文不完整，则继续注册事件等待对方输入
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    //分析完请求报文后并且根据do_request返回的状态，调用process_write函数向m_write_buf中写入响应报文。
    bool write_ret = process_write(read_ret);
    if (!write_ret) 
    {
        close_conn();
        //这里是不是要关闭定时器
    }

    //服务器子线程调用process_write完成响应报文，随后注册epollout事件。
    // 服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
    cout<<"1"<<endl;
}


