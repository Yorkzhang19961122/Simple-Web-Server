#include "http_conn.h"

// #define connfdLT    // 水平触发阻塞
#define connfdET    // 边缘触发非阻塞

#define listenfdLT  // 水平触发阻塞
// #define listenfdET  // 边缘触发非阻塞

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

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
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

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
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

#ifdef connfdLT

    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if(bytes_read <= 0) {
        return false;
    }
    return true;

#endif

#ifdef connfdET

    while(true) {  // recv读到的数据小于我们期望的缓冲区大小，因此要多次调用直到读完
        // recv(要读取的socket的fd, 读缓冲区的位置, 读缓冲区的大小, flag一般取0)
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);  // 从套接字接收数据，存储在m_read_buf缓冲区
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {  // 非阻塞ET模式下，需要一次性将数据读完
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
    
#endif

}

/* 
    从状态机的实现：
    解析(获取)一行，判断依据\r\n
*/
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for( ; m_checked_index < m_read_idx ; ++m_checked_index) {
        temp = m_read_buf[m_checked_index];                       // 从m_read_buf中逐字节读取
        if(temp == '\r') {                                        // 如果当前字节为\r
            if((m_checked_index + 1) == m_read_idx) {             // 接下来达到了buffer末尾，表示buffer还需要继续接收，返回LINE_OPEN
                return LINE_OPEN;
            } else if(m_read_buf[m_checked_index + 1] == '\n') {  // "\r"接下来的字符是"\n"，则将"\r\n"修改成"\0\0"，且将m_checked_index指向下一行的开头
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;                                   // 完整读取到一行
            }
            return LINE_BAD;                                      // 否则，表示语法错误，返回LINE_BAD
        } else if(temp == '\n') {                                 // 当前字节不是"\r"，判断是否是"\n",一般是上次读取到\r就到了buffer末尾，没有接收完整，再次接收时会出现这种情况
            if((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r')) {  // 如果前一个字符是"\r"，则将"\r\n"修改为"\0\0",将m_checked_index指向下一行的开头
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;                                   // 完整读取到一行
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;                                             // 当前接受的字节既不是"\r"，也不是"\n"，表示接收不完整，需要继续接收
}

// 解析请求首行(请求行)，获得请求方法，目标URL，HTTP版本号
// text = "GET /index.html HTTP/1.1"    
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // 请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔
    // C库函数-- strpbck(str1, str2): 检索字符串 str1 中第一个匹配字符串str2中字符的字符，并返回该字符的位置
    m_url = strpbrk(text, " \t");
    /*    
        m_url = " /index.html HTTP/1.1"
        text  = "GET /index.html HTTP/1.1"    
    */
   if(!m_url) {
       return BAD_REQUEST;   // 如果没有空格或\t，则报文格式有误
   }  

    *m_url++ = '\0';  // 将该位置改为\0，用于将前面数据取出：字符串数组的结尾为"\0"
    /*    
        m_url = "/index.html HTTP/1.1"    
        text  = "GET\0/index.html HTTP/1.1" 
    */  

    char* method = text;  // 取出请求方式
    /*
        method = "GET"
    */

    if(strcasecmp(method, "GET") == 0) {  // 忽略大小写比较，确定请求方式
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

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

    /* 
        去除"http://"和"https://"的影响
        此时使用m_url已经变成了"/index.html" 
    */
    // strncasecmp(str1, str2, n): 用来比较str1和str2前n个字符（忽略大小写），相同则返回0
    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');   // strchr(str, c): 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
    }
    // 一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    // 请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求头（和空行）
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    /*遇到空行，表示请求头解析完毕，进而判断content-length是否为0*/
    if(text[0] == '\0') {
        if(m_content_length != 0) {                // 如果不是0，HTTP请求有消息体，说明是POST请求，则还需要读取m_content_length字节的消息体
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;                     // 状态机转移到CHECK_STATE_CONTENT状态
        }
        /*否则说明没有消息体，是一个GET请求，意味着我们已经得到了一个完整的HTTP请求，报文解析结束*/
        return GET_REQUEST;
    // 解析请求头部字段
    } else if(strncasecmp(text, "Connection:", 11) == 0) {
        /*处理Connection头部字段 Connection: keep-alive*/
        text += 11;
        text += strspn(text, " \t");               // strspn(str1, str2): 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
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
    // 初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status= LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    /*
        (主状态机解析请求体 && 从状态机检查的一行OK) || 解析到一行完整的数据OK
        即：解析到了请求体且是完整的数据(针对POST请求，因为POST请求的消息体末尾没有任何字符) 或者 解析到了一行完整的数据(GET和POST请求都适用)        
    */
    // parse_line为从状态机的具体实现
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))  // 初始化m_check_state = CHECK_STATE_REQUESTLINE: 初始化状态为解析请求首行
            || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();        // 获取一行数据
        // m_start_line是每一个数据行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_index;
        printf("[INFO] 读取到一条http请求信息: %s\n", text);

        switch(m_check_state) {                 // 主状态机的三种状态转移逻辑
            case CHECK_STATE_REQUESTLINE: {     // 解析请求行
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {          // 解析请求头
                ret = parse_headers(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {         // 解析消息体
                ret = parse_content(text);
                if(ret == GET_REQUEST) {
                    return do_request();
                }
                // 完成消息体的解析后，主状态机的状态仍然是CHECK_STATE_CONTENT，还是会进入该while循环，
                // 因此增加此处语句，在结束消息体的解析后，将line_status更改为LINE_OPEN，则可跳出循环，完成请求报文的解析
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
    // 将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // 将url和网站目录拼接
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    /*通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体，返回值-1失败，0成功*/
    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;  //失败则返回NO_RESOURCE，表示请求资源不存在
    }
    /*判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态*/
    if(!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    /*判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误*/
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    /*以只读方式打开文件*/
    int fd = open(m_real_file, O_RDONLY);
    /*创建内存映射*/
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    /*避免文件描述符的浪费和占用*/
    close(fd);
    /*表示请求文件存在，且可以访问*/
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
    int newadd = 0;
    // 若要发送的数据长度为0
    // 表示响应报文为空，一般不会出现这种情况
    if ( bytes_to_send == 0 ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 将响应报文的状态行、消息头、空行和响应正文写到TCP Socket本身定义的发送缓冲区，交由内核发送给浏览器端
        // writev函数用于在一次函数调用中写多个非连续缓冲区，有时也将这该函数称为聚集写，若成功返回已写的字节数，若失败返回-1
        // writev以顺序iov[0]，iov[1]至iov[iovcnt-1]从缓冲区中聚集输出数据
        temp = writev(m_sockfd, m_iv, m_iv_count);
        // writev单次发送成功，temp为发送的字节数
        if (temp > 0) {
            // 更新已发送字节
            bytes_have_send += temp;
            // 偏移文件iovec的指针
            newadd = bytes_have_send - m_write_idx;
        }
        // writev单次发送失败
        if ( temp <= -1 ) {
            // 判断是否是写缓冲区满了，如果满了
            if (errno == EAGAIN) {
                // 第一个iovec头部信息的数据已发送完，发送第二个iovec数据
                if (bytes_have_send >= m_iv[0].iov_len) {
                    // 不再继续发送头部信息
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;
                }
                // 继续发送第一个iovec头部信息的数据
                else {
                    m_iv[0].iov_base = m_write_buf + bytes_to_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                // 重新注册写事件，等待下一次写事件触发（当缓冲区从不可写变为可写，触发epollout），因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            // 如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }
        // 更新待发送的字节数
        bytes_to_send -= temp;

        // 判断条件，数据已全部发送完
        if (bytes_to_send <= 0) {
            unmap();
            // 在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            // 浏览器的请求为长连接
            if (m_linger) {
                // 重新初始化HTTP对象
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
    // 如果写入内容超出m_write_buf大小则报错
    if(m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    // 定义可变参数列表
    va_list arg_list;
    // 将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    // 将数据format从可变参数列表写入写缓冲区，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }
    // 更新m_write_idx位置
    m_write_idx += len;
    // 清空可变参列表
    va_end(arg_list);
    return true;

}
// 添加响应状态行
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加消息报头，具体的添加文本长度、文本类型、连接状态和空行
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}
// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}
// 添加文本类型，这里是html
bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}
// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}
// 添加文本content
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

// 写HTTP响应,根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:                         // 内部错误，500
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:                            // 报文语法有误，404
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
        case FORBIDDEN_REQUEST:                      // 资源没有访问权限，403
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:                           // 文件存在，200
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            // 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            // 发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        default:
            return false;
    }
    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用的，这是处理HTTP请求的入口函数
void http_conn::process() {
    HTTP_CODE read_ret = process_read();       // 解析HTTP请求
    if(read_ret == NO_REQUEST) {               // NO_REQUEST，表示请求不完整，需要继续接收请求数据
        modfd(m_epollfd, m_sockfd, EPOLLIN);   // 修改socket事件，注册并监听读事件
        return;
    }

    bool write_ret = process_write(read_ret);  // 生成响应
    if(!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);      // 注册并监听写事件，服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器
}