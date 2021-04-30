#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "headers/locker.h"
#include "headers/threadpool.h"
#include "headers/http_conn.h"

#define MAX_FD 65536           // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大的事件数量

// 添加文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int))
{
    // 创建新的信号
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    // 指定信号捕捉函数的地址
    sa.sa_handler = handler;
    // 初始化信号集
    sigfillset(&sa.sa_mask);
    // 设定新的信号处理方式为SIG_IGN(忽略)
    assert(sigaction(sig, &sa, NULL) != -1);
}

int main(int argc, char *argv[])
{
    //没有输入端口参数
    if (argc <= 1)
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);
    // 处理sigpipe信号
    // 一个对端已经关闭的socket调用两次write, 第二次将会生成SIGPIPE信号, 该信号默认结束进程.
    // 所以需要忽略该信号
    addsig(SIGPIPE, SIG_IGN);
    //signal(SIGPIPE,SIG_IGN);


    MyDB *mydb=NULL;
    try
    {
        mydb=new MyDB;
        mydb->InitDB("localhost","root","123456","web_user");
    }
    catch(...)
    {
        printf("MYSQL WRONG\n");
        return 1;
    }
    



    // 创建线程池,捕获错误
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        // 创建线程池的时候出现了某些问题
        printf("Something Wrong\n");
        return 1;
    }




    http_conn *users = new http_conn[MAX_FD];

    // 创建socket, 使用ipv4, tcp流传输, 默认参数
    // 这里相当于接入口的文件描述符
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    int ret = 0;

    // 设置地址
    struct sockaddr_in address;

    // 任意地址
    address.sin_addr.s_addr = INADDR_ANY;

    // 使用internet协议族
    address.sin_family = AF_INET;

    // 设置端口
    // 把主机字节序转化为网络字节序
    address.sin_port = htons(port);

    // 端口复用
    int reuse = 1;
    // 设置端口复用
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // 绑定
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    // 监听
    ret = listen(listenfd, 5);

    // 创建epoll对象，和事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    
    // 添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while (true)
    {
        // 等待一个EPOLL事件
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);

        // EPOLL炸了
        // EINTR如果在进行系统调用时发生信号，许多系统调用将报告错误代码。
        // 实际上没有发生任何错误，只是以这种方式报告，因为系统无法自动恢复系统调用。
        // 这种编码模式仅在发生这种情况时重试系统调用，以忽略中断。
        // 比如超时write, 仅仅是需要重读就可以了
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        // 循环EPOLL的所有处理
        for (int i = 0; i < number; i++)
        {

            int sockfd = events[i].data.fd;
            // 当前发生事件的文件描述符

            // 有连接请求
            if (sockfd == listenfd)
            {
                // 创建连接
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                // 出现了问题
                if (connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                // 用户数量太多了
                if (http_conn::m_user_count >= MAX_FD)
                {
                    close(connfd);
                    continue;
                }
                // 初始化这个连接的文件描述符
                users[connfd].init(connfd, client_address,mydb);
            }
            // 出现了问题
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].close_conn();
            }
            // 出现了可读事件
            else if (events[i].events & EPOLLIN)
            {
                // 读取所有数据到缓冲区
                if (users[sockfd].read())
                {
                    // 加入请求队列, 等待线程池取出
                    // 这里线程做的事情是, 解析请求并且生成响应,放入写缓冲区
                    if(pool->append(users + sockfd)==false)
                        users[sockfd].close_conn();
                }
                // 读取失败(可能是缓冲区放不下了)
                else
                {
                    users[sockfd].close_conn();
                }
            }
            // 出现了可写事件
            else if (events[i].events & EPOLLOUT)
            {
                // 写入socket
                if (!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}