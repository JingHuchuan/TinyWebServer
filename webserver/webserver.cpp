#include "webserver.h"

WebServer::WebServer() {
    // 创建一个数组用于保存所有的客户端信息（预先为每个可能的客户连接分配一个http_conn对象）
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);           // 获取当前的工作路径，存到server_path中
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

// 初始化
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
    int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model) {
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

// listenfd和connfd的联合触发方式
void WebServer::trig_mode() {
    // LT + LT
    if (m_TRIGMode == 0) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }

    // LT + ET
    else if (m_TRIGMode == 1) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }

    // ET + LT
    else if (m_TRIGMode == 2) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }

    // ET + ET
    else if (m_TRIGMode == 3) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 开启日志与写日志
void WebServer::log_write() {
    // m_close_log关闭日志，默认为0不关闭
    if (m_close_log == 0) {
        // 初始化日志1异步, 0同步, 异步需要有缓冲队列
        if (m_log_write == 1) {
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        }
        else {
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
        }
    }
}

// 数据库连接池
void WebServer::sql_pool() {
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

// 线程池
void WebServer::thread_pool() {
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// 网络编程基础与epoll事件监听
void WebServer::eventListen() {
    // 网络变成基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (m_OPT_LINGER == 0) {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (m_OPT_LINGER == 1) {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    // 设置端口复用
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // 绑定
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    // 监听
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // 初始化定时器最小超时单位
    utils.init(TIMESLOT);
    
    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);                                        // 5没有任何意义
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);        // 将m_listenfd注册到epoll内核事件表中，使用utils的和http_conn的addfd都可以
    http_conn::m_epollfd = m_epollfd;                                   // 统一事件源，定时器事件和I/O事件都用同一个epoll

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);                // 创建管道，定时器处理函数发送信号，m_pipefd[1]写数据，m_pipefd[0]读数据
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);                                  // 设置为非阻塞，否则缓冲区如果满了，会进一步增加信号处理函数的事件
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);                      // epoll监听m_pipefd[0]

    // 注册信号
    utils.addsig(SIGPIPE, SIG_IGN);                                     // 接到SIGPIPE信号时, 不退出进程, 而是将其忽略
    utils.addsig(SIGALRM, utils.sig_handler, false);                    // 接到SIGALRM和SIGTERM信号时, 采用统一的信号处理函数处理
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);                                                    // 定时,5秒后产生SIGALARM信号

    // 工具类
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 将新来的客户端加入到监听列表，新的客户的数据初始化，放到数组中
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;                                 // 当前时间开始，3个TIMESLOT不操作就视作非活跃连接
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);                                 // 添加到定时器链表中
}

// 若有数据传输，则将定时器往后延迟3个TIMESLOT
void WebServer::adjust_timer(util_timer *timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

// 超时任务处理，删除相关定时器，关闭相应套接字
void WebServer::deal_timer(util_timer *timer, int sockfd) {
    timer->cb_func(&users_timer[sockfd]);
    if (timer) {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// 处理用户数据
bool WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (m_LISTENTrigmode == 0) {                                        // LT 水平触发，IO事件就绪时一直通知直到被处理
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) {
            // 超过最大连接数, 向客户端发送错误信息
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);                                  // 创建新来的用户的定时器，其中包括将新来的客户端加入到监听列表
    }

    else {                                                              // ET边缘触发
        // 这里暂时没看懂while(1)的作用
        while (1) {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

// 处理信号，通过管道的方式来告知WebServer，管道由epoll监控
bool WebServer::dealwithsignal(bool& timeout, bool& stop_server) {
    int ret = 0;
    int sig;
    char signals[1024];

    // 从管道读端读出信号值，成功返回字节数，失败返回-1
    // 正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1) {
        return false;
    }
    else if (ret == 0) {
        return false;
    }
    else {
        // 处理信号值对应的逻辑
        for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
                case SIGALRM: {
                    timeout = true;                                      // 超时标志
                    break;
                }
                case SIGTERM: {
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

// 处理读，reactor由子线程读，proactor由主线程读
void WebServer::dealwithread(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (m_actormodel == 1) {
        // 检测到读，需要重新调整定时器
        if (timer) {
            adjust_timer(timer);
        }

        // 若监测到可读事件，将该事件放入请求队列
        // 若请求队列已满此次请求将丢失，客户端的表现形式就是浏览器访问后长时间没有响应，可以等待一段时间之后再发起请求
        m_pool->append(users + sockfd, 0);

        while (true) {
            // 0不需要移除timer，1需要移除timer(断开连接)
            if (users[sockfd].improv == 1) {
                if (users[sockfd].timer_flag == 1) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else {
        // proactor
        if (users[sockfd].read_once()) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer) {
                adjust_timer(timer);
            }
        }
        else {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (m_actormodel == 1) {
        if (timer) {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true) {
            if (users[sockfd].improv == 1) {
                if (users[sockfd].timer_flag == 1) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else {
        // proactor
        if (users[sockfd].write()) {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer) {
                adjust_timer(timer);
            }
        }
        else {
            deal_timer(timer, sockfd);
        }
    }
}

// 事件处理函数，主要处理三种事件：io事件，信号，新的连接，对应for循环中的三次判断
void WebServer::eventLoop() {
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server) {
        // 监测发生事件的文件描述符
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1); 
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 轮询有事件产生的文件描述符
        for (int i = 0; i < number; ++i) {
            int sockfd = events[i].data.fd;

            // case:1 处理新到的客户连接
            if (sockfd == m_listenfd) {
                bool flag = dealclinetdata();
                if (flag == false) {
                    continue;
                }
            }

            // 对方异常断开或错误等事件，直接关闭客户连接，移除对应的定时器
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }

            // case2: 处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
                bool flag = dealwithsignal(timeout, stop_server);
                if (flag == false) {
                    LOG_ERROR("%s", "dealclientdata failure");
                }
            }
            
            // case3: 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            }

            else if (events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }

        // 最后处理超超时事件，优先级比较低
        if (timeout) {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }
 }