#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/uio.h>
#include "locker.h"


class http_conn {
public:
    /*所有socket上的事件都被注册到同一个epoll对象中*/
    static int m_epollfd;
    /*统计所有用户的数量*/
    static int m_user_count;

    http_conn(){}
    ~http_conn(){}

    /*处理客户端的请求*/
    void process();
    /*初始化新接受的客户连接*/
    void init_conn(int sockfd, const sockaddr_in& addr);
    /*关闭连接*/
    void close_conn();
    /*非阻塞的读*/
    bool read();
    /*非阻塞的写*/
    bool write();
private:
    /*该HTTP连接的socket*/
    int m_sockfd;
    /*通信的客户端socket地址*/
    sockaddr_in m_address;

};



#endif