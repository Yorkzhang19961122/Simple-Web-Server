#include "http_conn.h"

// 静态值的初始化
// 所有socket上的事件都被注册到同一个epoll对象中
int http_conn::m_epollfd = -1;
// 统计所有用户的数量
int http_conn::m_user_count = 0;
// 网站的根目录
const char* doc_root = "/home/admin1/Simple-Web-Server/resources";

/*定义HTTP响应的一些状态信息*/
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);     // 获取文件描述符原来的flag
    int new_flag = old_flag | O_NONBLOCK;  // 增加非阻塞的flag
    fcntl(fd, F_SETFL, new_flag);          // 将新的属性设置到该文件描述符上
    return old_flag;
}

// 向epoll中添加需要监听的文件描述符（内核事件表注册新事件 放树上）
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if(one_shot) {
        // 针对客户端连接的文件描述符connfd，开启EPOLLONESHOT，监听描述符listenfd不用开启，我们希望每个连接的socket在任意时刻都只被一个线程处理
        event.events | EPOLLONESHOT;
    }
    //往epoll事件表中注册fd上的事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中移除需要监听的文件描述符（内核事件表删除事件）
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上EPOLLONESHOT事件，确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init_conn(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;
    
    // 设置m_sockfd端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 将accept()到的socket文件描述符connfd注册到内核事件表中，等用户发来请求报文
    addfd(m_epollfd, sockfd, true);
    // 总用户数加1
    m_user_count++;

    init();
}

void http_conn::init() {
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始化状态为解析请求首行
    m_linger = false;  // 默认不保持链接Connection :keep-alive保持连接

    m_method = GET;    // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_index = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        // 关闭连接，客户数量减一
        m_user_count--;
    }
}

// 非阻塞的读
// 循环读取客户的数据直到无数据可读
bool http_conn::read_once() {
    // 对本程序来说可有可无
    if(m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    // 读取到的字节
    int bytes_read = 0;
    while(true) {  // recv读到的数据小于我们期望的缓冲区大小，因此要多次调用直到读完
        // recv(要读取的socket的fd, 读缓冲区的位置, 读缓冲区的大小, flag一般取0)
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);  // 从套接字接收数据，存储在m_read_buf缓冲区
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {  // 非阻塞ET模式下，需要一次性将数据读完?
                // 没有数据了
                break;
            }
            return false;
        } else if(bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;  // 修改m_read_idx的读取字节数
    }
    printf("[INFO] 读取到了请求报文: \n%s\n", m_read_buf);
    return true;
}

// 解析(获取)一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for( ; m_checked_index < m_read_idx ; ++m_checked_index) {
        temp = m_read_buf[m_checked_index];
        if(temp == '\r') {
            if((m_checked_index + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if(m_read_buf[m_checked_index + 1] == '\n') {
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(temp == '\n') {
            if((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r')) {
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析请求首行(请求行)，获得请求方法，目标URL，HTTP版本
// text = "GET /index.html HTTP/1.1"    
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    
    m_url = strpbrk(text, " \t");
    /*    
        m_url = " /index.html HTTP/1.1"
        text  = "GET /index.html HTTP/1.1"    
    */
   if(!m_url) {
       return BAD_REQUEST;
   }  

    *m_url++ = '\0';  // 置位空字符，字符串结束符
    /*    
        m_url = "/index.html HTTP/1.1"    
        text  = "GET\0/index.html HTTP/1.1" 
    */  

    char* method = text;
    /*
        method = "GET"
    */

    if(strcasecmp(method, "GET") == 0) {  // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk(m_url, " \t");
    /*
        m_version = " HTTP/1.1"
        m_url     = "/index.html HTTP/1.1"
    */

    if(!m_version) {
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    /*
        m_version = "HTTP/1.1"
        m_url     = "/index.html\0HTTP/1.1"
    */

    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    /*去除"http://"的影响*/
    /* 此时使用m_url都已经变成了"/index.html" */
    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    /*主状态机的检查状态变成检查请求头*/
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    /*遇到空行，表示头部字段解析完毕*/
    if(text[0] == '\0') {
        /*如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体*/
        /*状态机转移到CHECK_STATE_CONTENT状态*/
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        /*否则说明我们已经得到了一个完整的HTTP请求*/
        return GET_REQUEST;
    } else if(strncasecmp(text, "Connection:", 11) == 0) {
        /*处理Connection头部字段 Connection: keep-alive*/
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if(strncasecmp(text, "Content-Length:", 15) == 0) {
        /*处理Content-Length头部字段*/
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if(strncasecmp(text, "Host:", 5) == 0) {
        /*处理Host头部字段*/
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        /*未知的请求头*/
        printf("[INFO] 未知的请求头          : %s\n", text);
    }
    return NO_REQUEST;
}

// 解析请求体
// 我们没有真正去解析HTTP请求的请求体，只是判断它是否被完整的读入
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if(m_read_idx >= (m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    
    LINE_STATUS line_status= LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    /*
        (主状态机解析请求体 && 从状态机检查的一行OK) || 解析到一行完整的数据OK
        即：解析到了请求体且是完整的数据 或者 解析到了一行完整的数据        
    */
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))  // 初始化m_check_state = CHECK_STATE_REQUESTLINE: 初始化状态为解析请求首行
            || ((line_status = parse_line()) == LINE_OK)) {
        /*获取一行数据*/
        text = get_line();

        m_start_line = m_checked_index;
        printf("[INFO] 读取到一条http请求信息: %s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                if(ret == GET_REQUEST) {
                    return do_request();
                }
                /*行数据不完整*/
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            } 
        }
    }
    return NO_REQUEST;
}

/*
    当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性
    如果目标文件存在、对所有的用户可读，且不是目录，则使用mmap将其
    映射到内存地址m_file_address处，并告诉调用者获取文件成功
*/
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    /*stat函数获取m_real_file文件的相关的状态信息，-1失败，0成功*/
    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    /*判断访问权限*/
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    /*判断是否是目录*/
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    /*以只读方式打开文件*/
    int fd = open(m_real_file, O_RDONLY);
    /*创建内存映射*/
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 非阻塞的写HTTP响应
bool http_conn::write() {
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {
            // 头已经发送完毕
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0) {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger) {
                init();
                return true;
            }
            else {
                return false;
            }
        }
    }
}

// 往写缓冲中写入待发送的数据，可变参数
bool http_conn::add_response(const char* format, ...) {
    if(m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;

}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

// 写HTTP响应,根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用的，这是处理HTTP请求的入口函数
void http_conn::process() {
    /*解析HTTP请求*/
    HTTP_CODE read_ret = process_read();
    /*请求不完整*/
    if(read_ret == NO_REQUEST) {
        /*修改socket事件，监听读事件*/
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    /*生成响应*/
    bool write_ret = process_write(read_ret);
    if(!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}














