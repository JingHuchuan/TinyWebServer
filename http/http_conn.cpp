#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// static变量类外初始化
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

locker m_lock;
map<string, string> users;

// 定义HTTP响应的一些状态
const char * ok_200_title = "OK";
const char * error_400_title = "Bad Request";
const char * error_400_form = "Your request has bad syntax or is inherently impossible to statisfy.\n";
const char * error_403_title = "Forbidden";
const char * error_403_form = "You do not have permission to get file from this server.\n";
const char * error_404_title = "Not Found";
const char * error_404_form = "The requested file was not found on this server.\n";
const char * error_500_title = "Internal Error";
const char * error_500_form = "There was an unusual problem serving the request file.\n";

http_conn::http_conn() {

}

http_conn::~http_conn() {

}

// 数据库初始化
void http_conn::initmysql_result(connection_pool *connPool) {
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    
    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;
    
    // ET触发，使用EPOLLONESHOT
    if (TRIGMode == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核事件表中删除文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上EPOLLONESHPT事件，确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    // ET
    if (TRIGMode == 1) {
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    }
    else {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
    int close_log, string user, string passwd, string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;

    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;                                         // 总用户数+1

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();                                                 // 私有init()函数
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init() {
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
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或对方关闭连接，非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;                                     // 读取到的字节

    // LT读取数据
    if (m_TRIGMode == 0) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0) {
            // 对方关闭连接或者出现错误
            return false;
        }
        return true;
    }

    // ET读取数据，ET工作模式下，需要一次性将数据读完
    else {
        while (true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 以非阻塞的方式去读，当返回这两个错误码的时候，代表没有数据了
                    break;
                }
                return false;
            }
            else if (bytes_read == 0) {
                // 对方关闭连接
                return false;
            }
            m_read_idx += bytes_read;       // 更新读的索引
        }
        return true;
    }
    return true;
}

// 解析HTTP请求，入口函数，主状态机
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;      // 记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST;             // 记录HTTP请求的处理结果
    char* text = 0;

    //主状态机， 用于从buffer中取出所有完整的行，有两种情况，(解析请求体)或者(解析请求行和请求头部)
    while( ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)) {
        // 解析到了一行完整的数据，或者解析到了请求体，也是完整的数据，就执行下面的操作
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;            // 更新行开头
        LOG_INFO("%s", text);                    // 日志记录

        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }

            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST) {
                    // 表示获得了一个完整的客户请求
                    return do_request();
                }
                break;
            }

            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                // 这里就不存在请求语法的问题了，不用判断BAD_REQUEST
                if (ret == GET_REQUEST) {
                    return do_request();
                }
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

// 从状态机，用于解析出一行的内容，然后给相应的解析函数解析
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;

    /*checked_index指向buffer（应用程序的缓冲区）中当前正在分析的字节，read_index指向buffer中客户数据的
    尾部的下一字节。buffer中第0 ~ checked_index字节都已经分析完毕，第checked_index ~ (read _index - 1)
    字节由下面的循环挨个分析*/
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        // 获得当前要分析的字节号
        temp = m_read_buf[m_checked_idx];

        // 如果当前的字节是"\r"，即回车符，则说明可能读取到一个完整的行
        if (temp == '\r') {
            // 如果'\r'字符碰巧是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行，返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析*
            if (m_checked_idx + 1 == m_read_idx) {
                return LINE_OPEN;
            }

            // 如果下一个和字符是'\n'，则说明我们成功读取到一个完整的行
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 后面两个都变成'\0'
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            // 否则的话，说明客户发送的HTTP请求存在语法问题
            return LINE_BAD;
        }

        // 如果当前的字节是'\n'，即换行符，则也说明可能读取到一个完整的行，此时的情况就是两次读是分开的
        else if (temp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                // 后面两个都变成'\0'
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx ++ ] = '\0';
                return LINE_OK;
            }

            // 否则\n之前不是\r就是语法错误
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
} 

// 主状态机CHECK_STATE_REQUESTLINE的处理函数，解析请求首行，获得请求方法，目标URL，HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    // m_url将会指向temp中第一个出现空格或制表符的位置
    m_url = strpbrk(text, " \t");           // 检索" \t"
    
    // 如果请求行中没有空白字符或"\t"字符，则HTTP请求必有问题
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';                                    // 将url指向的位置设置为字符'\0'，通过url++将url指针向后移动一位，此时的m_url就是GET\0/index.html HTTP/1.1

    char* method = text;                                // 判断请求方法，目前支持"GET"和"POST"
    if (strcasecmp(method, "GET") == 0) {               // 判断method是不是等于"GET"
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0) {         // "POST"
        m_method = POST;
        cgi = 1;                                        // POST标志位
    }
    else {
        return BAD_REQUEST;
    }
    m_url += strspn(m_url, " \t");                      // 这句话的作用就是跳过url之前的前导空格（如果有多的话）

    // /index.html HTTP/1.1
    // 下面就是一样的操作
    m_version = strpbrk(m_url, " \t");                  // HTTP/1.1
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';                    
    m_version += strspn(m_version, " \t");

    // 仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 检查URL是否合法
    // /index.html\0HTTP/1.1
    // 有的可能是http://192.168.1.1:10000/index.html
    // 检查http://
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;                        // 192.168.1.1:10000/index.html
        m_url = strchr(m_url, '/');        // 该函数的作用是在url中寻找'/'，并返回位置，找不到就返回空，这里返回是/index.html
    }

    // 检查https://
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1) {
        strcat(m_url, "judge.html");
    }

    // HTTP请求行处理完毕，转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}   

// 主状态机CHECK_STATE_HEADER的处理函数，解析头部字段
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {

    // 遇到空行， 表示头部字段解析完毕
    if (text[0] == '\0') {
        // 如果HTTP请求有消息体， 则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }

    // 处理Connection头部字段    Connection: keep-alive
    else if (strncasecmp(text, "Connection:", 11)  == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }

    // 处理Content-Length头部字节
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }

    // 处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;  
    }

    else {
        LOG_INFO("oop!unknow header: %s", text);
    }
    
    return NO_REQUEST;
}

// 主状态机CHECK_STATE_CONTENT的处理函数，解析请求体
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length]= '\0';
        m_string = text;                  // POST请求中最后为输入的用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
} 

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);                                          // doc_root表示网站的根目录
    int len = strlen(doc_root); 
    const char *p = strrchr(m_url, '/');                                    // 找到m_url中/的位置

    // 是POST请求时cgi == 1, 根据m_url最后一个'/'后方跟的是2还是3来区分是登录检测还是注册检测
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // /2CGISQL.cgi:登录校验，/3CGISQL.cgi:注册校验

        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);                                      // m_url_real: /CGISQL.cgi
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);     // 项目根路径+请求资源的真实路径
        free(m_url_real);

        // 将用户名和密码提取出来，user=123&passwd=123
        char name[100], password[100];
        int i;
        for (int i = 5; m_string[i] != '&'; ++i) {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';                                                 //字符末尾补上字符串结束符

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j) {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        // 注册，先检测数据库中是否有重名的，没有重名的，进行增加数据
        if (*(p + 1) == '3') {
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) {
                // 数据库操作加锁，只能有一个线程访问
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                // 以覆盖的形式修正m_url，不管数据库中有没有要查询的用户名信息，只要select语句执行成功，mysql_query()的返回值就都为0
                if (!res) {
                    strcpy(m_url, "/log.html");                             // 注册成功，进入登录页面
                }
                else {
                    strcpy(m_url, "/registerError.html");                   // 出现错误，注册失败
                }
            }
            else {
                strcpy(m_url, "/registerError.html");                       // 用户已经存在，注册失败
            }
        }

        // 登录，直接判断，若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2') {
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");                             // 密码匹配，登录成功
            }
            else {
                strcpy(m_url, "/logError.html");                            // 否则登录失败
            }
        }
    }

    // 如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }

    // 如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }

    // 如果请求资源为/5，表示图片请求页面
    else if (*(p + 1) == '5') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }

    // 如果请求资源为/6，表示视频请求页面
    else if (*(p + 1) == '6') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }

    // 如果请求资源为/7，表示关注页面
    else if (*(p + 1) == '7') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }

    else {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
        
    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 往写缓冲中写入待发送数据
bool http_conn::add_response(const char *format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

// 对内存映射区执行munmap操作，简而言之就是释放资源，取消内存映射
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 响应报文写入函数
bool http_conn::write() {
    int temp = 0;
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);             // 将要发送的字节为0，这一次的响应结束，由于设置了ONESHOT，需要重新添加事件
        init();                                                      // 注意：这一次的响应已经结束了，准备下一次接收，所有的数值都重新初始化
        return true;
    } 

    while (1) {
        // 分散写，将多块内存一起写
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0) {
            // 如果TCP写缓冲没有空间，则等待一下轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到同一个客户的下一个请求，但这可以保证连接的完整性
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();                                                 // 释放内存映射
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len) {
            // 写内存重新对齐
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

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

// 添加状态行（对应请求行）
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加头部信息
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

// 添加消息体长度
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

// 添加相应数据类型
bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}

// 添加HTTP是否保持连接
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true)?"keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 添加消息体
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch(ret) {
        case INTERNAL_ERROR: {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                // 发送错误信息
                return false;
            }
            break;
        }
        
        case BAD_REQUEST: {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }

        case NO_RESOURCE: {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }

        case FORBIDDEN_REQUEST: {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }

        // 正常的情况，把请求的文件准备好
        case FILE_REQUEST: {
            add_status_line(200, ok_200_title);
            // 这里请求头部（状态行和头部信息）和请求体是分开的。对于错误信息的请求体，比较短，就和buf放在一起了
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else {  
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) {
                    return false;
                }
            }
        }
        default: {
            return false;
        }
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数。子线程就是处理这里，做相应的业务逻辑
void http_conn::process() {
    // 解析HTTP请求，已经是读到数据了
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);     // 如果请求不完整，需要继续读取，由于使用了ONESHOT事件，每一次操作完之后需要重新添加事件
        return;
    }

    // 生成响应，把准备写的数据准备好
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);        // 注册并监听写事件
}

