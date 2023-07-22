#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "../threadpool/threadpool.h"
#include "../http/http_conn.h"

const int MAX_FD = 65536;                           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000;                 // 最大事件数
const int TIMESLOT = 5;                             // 最小超时单位

class WebServer {
public:
    WebServer();
    ~WebServer();

    /*
    <---------------------------------------------------->
        port:               端口号
        user:               用户名
        passWord:           密码
        databasename:       数据库名
        log_write:          日志写入方式，默认同步
        trigmode:           默认listenfd LT + connfd LT
        opt_linger:         优雅关闭链接，默认不使用
        sql_num:            数据库连接池数量，默认8
        thread_num:         线程池内的线程数量，默认8
        close_log:          关闭日志，默认不关闭
        actor_model:        并发模型,默认是proactor
    <---------------------------------------------------->
    */
    void init(int port , string user, string passWord, string databaseName,
            int log_write , int opt_linger, int trigmode, int sql_num,
            int thread_num, int close_log, int actor_model);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclinetdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    // socket通信基础变量声明
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;

    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;

    // 数据库相关
    connection_pool *m_connPool;
    string m_user;                                                     
    string m_passWord;                                                
    string m_databaseName;   
    int m_sql_num;                                        

    // 线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    // epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];
    int m_listenfd;
    int m_OPT_LINGER;                       // 优雅关闭连接
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    // 定时器相关
    client_data *users_timer;
    Utils utils;
};

#endif