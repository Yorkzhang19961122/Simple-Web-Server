#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535            // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // epoll最大支持同时监听的事件个数

extern void addfd(int epollfd, int fd, bool one_shot);  // 添加文件描述符到epoll中
extern void removefd(int epollfd, int fd);              // 从epoll中删除文件描述符
// extern void modfd(int epollfd, int fd, int ev);         // 修改epoll中的文件描述符

// 添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    /*
        sa: sigaction结构体类型的指针
        sigaction结构体中的sa_handler成员指定信号处理函数
        sa_mask成员设置进程的信号掩码
    */
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;    // 函数指针: 将handler函数的首地址赋给sa.sa_handler
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);  // 设置信号处理函数
}

int main(int argc, char* argv[]) {
    // 首先判断执行程序传入的参数是否正确
    // 如果不传参数的话，默认只有我们执行函数的命令这一个参数
    if(argc <= 1) {
        printf("请按照如下格式执行程序: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    int port = atoi(argv[1]);  // 获取端口号: 字符串转为整数
    addsig(SIGPIPE, SIG_IGN);  //对SIGPIPE信号进行处理: 忽略SIGPIPE信号

    threadpool<http_conn>* pool = NULL;  // 创建线程池，初始化线程池指针
    // try catch(...)能够捕获任何异常
    try{
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    http_conn* users = new http_conn[MAX_FD];   // 创建MAX_FD个http_conn类对象，存于users数组中

    /*
        socket编程
    */
    // 1.创建监听的socket文件描述符
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    // struct sockaddr_in: IPv4专用的socket地址结构体
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    // htons: 端口->主机字节序转为网络字节序
    // htonl: IP->主机字节序转为网络字节序
    address.sin_port = htons(port);

    // 2.设置socket属性之端口复用，在绑定之前
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 3.绑定IP和PORT地址
    bind(listenfd, (struct sockaddr*) &address, sizeof(address));

    // 4.监听，创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前
    listen(listenfd, 5);

    /*
        epoll的代码
        创建epoll对象，事件数组，添加监听fd
    */
    int epollfd = epoll_create(5);          // 创建epoll对象,创建一个额外的文件描述符来唯一标识内核中的epoll事件表
    epoll_event events[MAX_EVENT_NUMBER];   // 创建内核事件表（用于存储epoll事件表中就绪事件的event数组）
    addfd(epollfd, listenfd, false);        // 将listenfd放在epoll树上，（主线程往epoll内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件）
    http_conn::m_epollfd = epollfd;         // 将上述epollfd赋值给http_conn类对象的m_epollfd属性（static，所有对象使用同一份）

    while(true) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);  // 主线程调用epoll_wait等待一组fd上的事件，并将当前所有就绪的epoll_event复制到events数组中
        if((num < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }
        
        // 然后我们可以遍历事件数组以处理已经就绪的事件
        for(int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;  // 事件表中就绪的socket文件描述符
            if(sockfd == listenfd) {
                // 有客户端连接进来了
                // 5.解除阻塞，接受(accept)客户端的连接，返回一个和客户端通信的connfd
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);

#ifdef LT
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                if(connfd < 0) {
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数满了
                    // TODO: 给客户端写一个服务器内部正忙的信息
                    close(connfd);
                    continue;
                }
                users[connfd].init_conn(connfd, client_address);   // 将新客户的连接数据初始化，放到user数组中
#endif

#ifdef ET
                while(1) {   //需要循环接收数据
                    int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                    if(connfd < 0) {
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD) {
                        // 目前连接数满了
                        // TODO: 给客户端写一个服务器内部正忙的信息
                        close(connfd);
                        break;
                    }
                    users[connfd].init_conn(connfd, client_address);   // 将新客户的连接数据初始化，放到user数组中
                }
                continue;
#endif

            } 
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {

                users[sockfd].close_conn();          // 对方异常断开或错误的事件发生了,关闭连接

            } 
            else if(events[i].events & EPOLLIN) {    // 当这一sockfd上有可读事件时，epoll_wait通知主线程

                if(users[sockfd].read_once()) {      // 有读事件发生,一次性把所有数据都读到对应http_conn对象的缓冲区（主线程从这一sockfd循环读取数据, 直到没有更多数据可读）

                    pool->append(users + sockfd);    // 添加到线程池中（将读取到的数据封装成一个请求对象并插入请求队列）

                } 
                else {

                    users[sockfd].close_conn();      // 读数据失败的话把连接关闭

                }
            } 
            else if(events[i].events & EPOLLOUT) {

                if(!users[sockfd].write()) {         // 有写事件发生,一次性写完所有数据（当这一sockfd上有可写事件时，epoll_wait通知主线程。主线程往socket上写入服务器处理客户请求的结果）

                    users[sockfd].close_conn();      // 写失败的话关闭连接

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