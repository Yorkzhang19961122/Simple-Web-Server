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

/*最大的文件描述符个数*/
#define MAX_FD 65535  
/*epoll最大支持同时监听的事件个数*/
#define MAX_EVENT_NUMBER 10000

/*添加信号捕捉*/
void addsig(int sig, void(handler)(int)) {
    /*sa: sigaction结构体类型的指针
      sigaction结构体中的sa_handler成员指定信号处理函数
      sa_mask成员设置进程的信号掩码*/
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    /*函数指针: 将handler函数的首地址赋给sa.sa_handler*/
    sa.sa_handler = handler; 
    sigfillset(&sa.sa_mask);
    /*设置信号处理函数*/
    sigaction(sig, &sa, NULL);

}

/*添加文件描述符到epoll中*/
extern void addfd(int epollfd, int fd, bool one_shot);
/*从epoll中删除文件描述符*/
extern void removefd(int epollfd, int fd);
/*修改epoll中的文件描述符*/
extern void modfd(int epollfd, int fd, int ev);

int main(int argc, char* argv[]) {
    /*首先判断执行程序传入的参数是否正确*/
    /*如果不传参数的话，默认只有我们执行函数的命令这一个参数*/
    if(argc <= 1) {
        printf("请按照如下格式执行程序: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    /*获取端口号: 字符串转为整数*/
    int port = atoi(argv[1]);
    
    /*对SIGPIPE信号进行处理: 忽略SIGPIPE信号*/
    addsig(SIGPIPE, SIG_IGN);

    /*创建线程池，初始化线程池*/
    threadpool<http_conn>* pool = NULL;
    /*try catch(...)能够捕获任何异常*/
    try{
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    /*创建数组用于保存所有的客户端信息*/
    http_conn* users = new http_conn[MAX_FD];

    /*socket编程*/
    /*1.创建监听的文件描述符*/
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    /*2.设置socket属性之端口复用，在绑定之前*/
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    /*3.绑定IP和PORT地址*/
    /*struct sockaddr_in: IPv4专用的socket地址结构体*/
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    /*htons: 端口->主机字节序转为网络字节序*/
    /*htonl: IP->主机字节序转为网络字节序*/
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*) &address, sizeof(address));
    /*4.监听*/
    listen(listenfd, 5);

    /*epoll的代码*/
    /*创建epoll对象，事件数组，添加监听fd*/
    epoll_event events[MAX_EVENT_NUMBER];
    /*创建epoll对象*/
    int epollfd = epoll_create(5);
    /*将监听的fd添加到epoll对象中*/
    addfd(epollfd, listenfd, false);
    /*将http_conn中的静态成员m_epollfd设置成epollfd*/
    http_conn::m_epollfd = epollfd;

    /**/
    while(true) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }
        
        /*循环遍历epoll事件树*/
        for(int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) {
                /*有客户端连接进来了*/
                /*5.解除阻塞，接受客户端的连接，返回一个和客户端通信的fd*/
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);

                if(http_conn::m_user_count >= MAX_FD) {
                    /*目前连接数满了*/
                    /*TODO: 给客户端写一个服务器内部正忙的信息*/
                    close(connfd);
                    continue;
                }

                /*将新客户的连接数据初始化，放到user数组中*/
                users[connfd].init_conn(connfd, client_address);
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                /*对方异常断开或错误的事件发生了*/
                /*关闭连接*/
                users[sockfd].close_conn();
            } else if(events[i].events & EPOLLIN) {
                /*有读事件发生*/
                /*一次性把所有数据都读完*/
                if(users[sockfd].read()) {
                    /*添加到线程池中*/
                    pool->append(users + sockfd);
                } else {
                    /*读数据失败的话把连接关闭*/
                    users[sockfd].close_conn();
                }
            } else if(events[i].events & EPOLLOUT) {
                /*有写事件发生*/
                /*一次性写完所有数据*/
                if(!users[sockfd].write()) {
                    /*写失败的话关闭连接*/
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