#include "headers/http_conn.h"

// 定义HTTP响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

const char *doc_register = "/register.html";
const char *doc_nonuser = "/user_notfd.html";
const char *doc_welcome = "/LOGIN.html";
const char *doc_pswderror = "/pswd_error.html";

const char *doc_usr_exit = "/exist_user.html";
const char *doc_ept_pswd = "/empty_pswd.html";
const char *doc_rgst_ac = "/rgst_accept.html";

const char *doc_home = "/index.html";


// 网站的根目录
const char *doc_root = "/home/mal/Webserver/resources";

// 所有的客户数
int http_conn::m_user_count = 0;

// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

///设置文件描述符非阻塞
int setnonblocking(int fd)
{
    ///获取文件描述符状态
    int old_option = fcntl(fd, F_GETFL);

    ///新的状态,在原有状态的基础上添加非阻塞
    int new_option = old_option | O_NONBLOCK;

    ///新状态的设置
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;

    event.data.fd = fd;///设置epoll_event的文件描述符
    
    ///有数据可读 | 对端断开连接 | 边沿触发
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;


    if (one_shot)
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }


    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    // 修改epoll上这个事件的状态
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        ///移除文件描述符
        removefd(m_epollfd, m_sockfd);

        ///socket文件描述符赋值为-1        
        m_sockfd = -1;

        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr,MyDB *mydb)
{
    ///设置socket文件描述符
    m_sockfd = sockfd;

    ///设置socket地址
    m_address = addr;

    m_mydb=mydb;
    // 端口复用
    int reuse = 1;
//      1)SOL_SOCKET:通用套接字选项.
//      2)IPPROTO_IP:IP选项.
//      3)IPPROTO_TCP:TCP选项.

//      设置端口复用-------SO_REUSEADDR
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    ///向epoll中添加新的socket描述符
    addfd(m_epollfd, sockfd, true);
    // 客户总数++
    m_user_count++;
    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_linger = false;                        // 默认不保持链接  Connection : keep-alive保持连接

    // 已经发送的字节数为0
    bytes_have_send=0;
    // 等待发送的字节数为0
    bytes_to_send=0;
    // 默认请求方式为GET
    m_method = GET; 
    // 请求的文件名---url
    m_url = 0;
    // HTTP版本号
    m_version = 0;
    // 请求消息的长度
    m_content_length = 0;
    // 主机名
    m_host = 0;
    // 解析行的起始位置
    m_start_line = 0;
    // 当前解析到哪了
    m_checked_idx = 0;
    // 下一个需要读取的位置
    m_read_idx = 0;
    // 待发送的字节数
    m_write_idx = 0;
    // 清空读写缓冲区和路径

    m_login_pswd = 0;

    m_login_name = 0;

    m_rgt_name = 0;

    m_rgt_pswd = 0;



    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 通过recv循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    // 读缓冲区放不下了
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;///每次实际读到了多少
    while (true)
    {
        // 从m_read_buf + m_read_idx索引处开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        // 发生了一些错误
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 没有数据  读完了
                break;
            }
            return false;
        }
        // 对方关闭连接
        else if (bytes_read == 0)
        { 
            return false;
        }
        // 偏移
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    // 尝试从缓冲区读取一行
    // 以\0分割每一行
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        // 缓冲区当前这一位
        if (temp == '\r')
        {
            // 下一个都末尾了,这个\n就已经是最后一个了
            if ((m_checked_idx + 1) == m_read_idx)
            {
                // 这一行还没完,但是已经把缓冲区读完了
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 把\r和\n都变成\0表示结束一行
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                // 读取到了完整的一行
                return LINE_OK;
            }
            // 有问题
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            // 先读到了\n, 可能是之前只读到了\r
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            { 
                // 把\r和\n都变成\0表示结束一行
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                // 读取到了完整的一行
                return LINE_OK;
            }
            // 有问题
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}


// 分割所有的&起来的字符
void http_conn::cut(char *text)
{
    char *now=text;
    while(1)
    {
        now=strpbrk(text, "&");
        if(now)
            *now++='\0';
        if (strncasecmp(text, "username=", 9) == 0)
        {
            text += 9;
            m_login_name=text;
        }
        if (strncasecmp(text, "Password=", 9) == 0)
        {
            text += 9;
            m_login_pswd=text;
        }
        if(strncasecmp(text,"rgtname=",8)==0)
        {
            text+=8;
            m_rgt_name=text;
        }
        if(strncasecmp(text,"rgtpswd=",8)==0)
        {
            text+=8;
            m_rgt_pswd=text;
        }
        text=now;
        if(!now) break;

    }
}

// 查找这个名字是否存在, 存在返回密码, 不存在就返回空
char *http_conn::user_fd(char * name)
{
    std::string a="select user_pswd from webuser where user_name=";
    a=a+"'"+name+"';";
    return m_mydb->ExeSQL(a);
}



// 检查用户的密码是否正确
http_conn::PSWD_STATUS http_conn::check_pswdAnduser()
{
    char *true_pswd=user_fd(m_login_name);
    if(!true_pswd||!m_login_pswd) return USER_NOTFOUND;
    if(strcmp(true_pswd,m_login_pswd)==0)
        return PSWD_RIGHT;
    return PSWD_WRONG;

}

// 尝试注册
http_conn::RGST_STATUS http_conn::try_rgst()
{
    if(user_fd(m_rgt_name))
    {
        return ARD_EXT;
    }
    else
    {
        if(m_rgt_pswd==NULL||m_rgt_pswd[0]=='\0')
            return ERR_IN;
        else
        {
            std::string a="insert into webuser values (";
            a=a+"'"+m_rgt_name+"','"+m_rgt_pswd+"');";

            m_mydb->ExeSQL(a);
            std::cout<<a<<std::endl;
            return RG_ACPT;
        }
            
    }
}


// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0'; // 置位空字符，字符串结束符

    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        // 忽略大小写比较
        m_method = GET;
    }
    else if(strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
    }
    else
    {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }



    /**
     * http://192.168.110.129:10000/index.html
    */

    ///看看有没有密码信息
    char *_find = strpbrk(m_url, "?");
    if(_find)
    {
        // 使用cut获取密码信息
        *_find++='\0';
        cut(_find);
    }



    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    // 请求第一行结束,检查状态变成检查头
    m_check_state = CHECK_STATE_HEADER;
    // 还没有解析完,继续解析
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态

        // 存在消息体
        if (m_content_length != 0)
        {
            // 转移到解析消息体
            m_check_state = CHECK_STATE_CONTENT;
            // 返回解析未结束
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            // 保持连接
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");

        // 消息体长度
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
// 事实上就是把connection, content-length, host都判断一下, 然后把相应的信息存起来    
}


// 我们没有真正解析GET请求的请求体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content_get(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


void http_conn::Post_RGSTandLOGIN(http_conn::HTTP_CODE ret)
{
    if(ret== RIGHT_PSWD)/// POST请求的密码在请求体,如果请求体解析出来的密码正确
    {
        strcpy(m_url,doc_welcome);
    }
    else if(ret == WRONG_PSWD)
    {
        strcpy(m_url,doc_pswderror);
    }
    else if(ret == NOTFOUND_USER)
    {
        strcpy(m_url,doc_nonuser);
    }
    else if(ret == USR_EXT)
    {
        strcpy(m_url,doc_usr_exit);
    }
    else if(ret == ERR_PSWD)
    {
        strcpy(m_url,doc_ept_pswd);
    }
    else if(ret == RGST_AC)
    {
        strcpy(m_url,doc_rgst_ac);
    }
}



// 解析POST请求的请求体
http_conn::HTTP_CODE http_conn::parse_content_post(char *text)
{
    ///m_read_idx 第一个字符位置
    if (m_read_idx < (m_content_length + m_checked_idx))
        return NO_REQUEST;
    text[m_content_length] = '\0';
    if (text[0] == '\0')
        return GET_REQUEST;

    cut(text);

    if(m_login_name)
    {
        http_conn::PSWD_STATUS st=check_pswdAnduser(); 
        if(st==PSWD_RIGHT)
            return RIGHT_PSWD;
        if(st==PSWD_WRONG)
            return WRONG_PSWD;
        if(st==USER_NOTFOUND)
            return NOTFOUND_USER;
    }
    else if(m_rgt_name)
    {
        http_conn::RGST_STATUS st=try_rgst();
        if(st==ARD_EXT)
            return USR_EXT;
        if(st==ERR_IN)
            return ERR_PSWD;
        if(st==RG_ACPT)
            return RGST_AC;
    }
    
    return GET_REQUEST;

}


http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if(m_method==GET)
    {
        return parse_content_get(text);
    }
    if(m_method==POST)
    {
        return parse_content_post(text);
    }
    return GET_REQUEST;
    
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read()
{
    // 初始状态      //前面没有行, 所以是LINE_OK
    LINE_STATUS line_status = LINE_OK;
    // 初始状态      //请求还不完整
    HTTP_CODE ret = NO_REQUEST;
    // 当前行
    char *text = 0;

    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
            || ((line_status = parse_line()) == LINE_OK))
    {
        // 获取一行数据
        text = get_line();

        // 更新当前新的请求行的行首地址
        m_start_line = m_checked_idx;
        printf("%s\n", text);

        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                // 当前状态是请求行,就使用请求行函数
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                // 当前状态是请求头部
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST)// 没有请求体, 已经获取了完整请求
                {
                    if(m_login_name)
                    {
                        http_conn::PSWD_STATUS st=check_pswdAnduser(); 
                        if(st==PSWD_RIGHT)
                        {
                            strcpy(m_url,doc_welcome);
                        }
                        else if(st==PSWD_WRONG)
                        {
                            strcpy(m_url,doc_pswderror);
                        }
                        else if(st==USER_NOTFOUND)
                        {
                            strcpy(m_url,doc_nonuser);
                        }
                    }
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);// 解析请求体
                if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                else if(ret==NO_REQUEST)
                {
                    // 说明行数据还不完整
                    line_status = LINE_OPEN;
                }
                else
                {
                    Post_RGSTandLOGIN(ret);
                    return do_request();
                }
                break;
            }
            default:
            {
                // 这都不对, 就是服务器内部问题
                return INTERNAL_ERROR;
            }
        }
    }
    // 能到这说明请求不完整
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // 把根目录复制到目标文件目录里
    strcpy(m_real_file, doc_root);

    int len = strlen(doc_root);
    // 在根目录后面加上用户请求的路径
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        // 获取失败
        return NO_RESOURCE;
    }

    // 判断访问权限
    // S_IROTH是其他组的读权限
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        // 不可访问
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    // 映射区域可读, 私人的写时拷贝, 想要映射的文件描述符, 偏移量0
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap()
{
    // 如果内存地址存在
    if (m_file_address)
    {
        // 取消文件映射
        munmap(m_file_address, m_file_stat.st_size);
        // 恢复文件映射的地址
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        // 将要发送的字节为0，这一次响应结束。

        // 重置epollin, 并且重新初始化http请求
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        // 聚集写
        // 写缓冲和请求的文件信息一起写进去
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1)
        {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN)
            {
                // 等待EPOLLOUT事件(等待可写)
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            // 这里是出错了, 解除内存映射
            unmap();
            return false;
        }
        // 等待发送的字符个数-=temp
        bytes_to_send -= temp;
        // 已经发送的字符个数+=temp
        bytes_have_send += temp;

        // 如果请求头已经发送完毕
        if(bytes_have_send >= m_iv[0].iov_len)
        {
            // 请求头归零
            m_iv[0].iov_len = 0;
            // 文件可能也已经发送了一部分
            // 更新新的文件地址
            m_iv[1].iov_base = m_file_address + (bytes_have_send-m_write_idx);
            // 更新新的文件长度为待发送长度
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 更新新的地址
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            // 新的文件长度需要减去当前一次发送的文件长度
            m_iv[0].iov_len -= temp;
        }

        if (bytes_to_send <= 0)
        {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            // 发送(写)完了, 等待可读事件
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
    return false;
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    // 如果需要写的东西超过了写缓冲区的大小, 写入失败
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    // 定义可变参数列表
    va_list arg_list;
    // 通过format(可变参数...的前一个)初始化arg_list指针
    va_start(arg_list, format);

    // 可变参数写入, 写入到缓冲区
    // 写入m_write_buf + m_write_idx,写入长度,格式化字符串,可变参数
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    // 写入失败
    if (len < 0)
    {
        return false;
    }
    // 写入大小增加len
    m_write_idx += len;

    // 使用完毕,关闭arg_list
    va_end(arg_list);
    // 返回写入成功
    return true;
}

// 加入状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 加入消息头
bool http_conn::add_headers(int content_len)
{
    return (add_content_length(content_len)&&add_content_type()&&add_linger()&&add_blank_line());
}

// 消息体的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

// 加入链接类型
bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 加入空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

// 添加消息体
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 添加消息体的类型
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    // 传入读的内容, 从而决定写的内容


    switch (ret)
    {
    // 服务器内部错误
    case INTERNAL_ERROR:


        puts("INTERNAL_ERROR");
        // 错误号500
        // 加入状态行
        add_status_line(500, error_500_title);
        // 加入头
        add_headers(strlen(error_500_form));
        // 加入消息体
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    // 语法错误
    case BAD_REQUEST:

        puts("BAD_REQUEST");

        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    // 没有资源
    case NO_RESOURCE:


        puts("NO_RESOURCE");

        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    // 权限不足
    case FORBIDDEN_REQUEST:

        puts("FORBIDDEN_REQUEST");

        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    

    // 文件获取成功
    case FILE_REQUEST:

        puts("FILE_REQUEST");

        // 加入状态行
        add_status_line(200, ok_200_title);
        // 加入消息头
        add_headers(m_file_stat.st_size);
        // 初始化聚集写
        m_iv[0].iov_base = m_write_buf;         //写缓冲地址
        m_iv[0].iov_len = m_write_idx;          //写缓冲大小
        m_iv[1].iov_base = m_file_address;      //文件地址
        m_iv[1].iov_len = m_file_stat.st_size;  //文件大小
        m_iv_count = 2;

        // 更新字节数
        bytes_to_send=m_write_idx+m_file_stat.st_size;
        return true;
    default:
        return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    // 没读完
    if (read_ret == NO_REQUEST)
    {
        // 等待下一波可读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    printf("***********************%s*******************\n",m_real_file);
    printf("username: %s\n",m_login_name);
    printf("userpswd: %s\n",m_login_pswd);
    printf("rgstname: %s\n",m_rgt_name);
    printf("rgstpswd: %s\n",m_rgt_pswd);
    printf("***************\n");

    // 生成响应
    bool write_ret = process_write(read_ret);
    // 如果写失败或者请求有问题
    if (!write_ret)
    {
        // 关闭连接
         close_conn();
    }
    // 等待可写事件, 可写事件的时候才真正把信息返回, 现在还存在缓冲区
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}