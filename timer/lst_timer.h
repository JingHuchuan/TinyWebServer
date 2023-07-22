#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <time.h>

#include "../log/log.h"

class util_timer;                                                               // 类交叉定义，需要前向声明

// 用户数据结构
struct client_data {
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

// 定时器类
class util_timer {
public:
    util_timer() : prev(NULL), next(NULL){}

public:
    time_t expire;                                                              // 任务超时时间，这里使用绝对时间
    void (*cb_func)( client_data* );                                            // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    client_data* user_data; 
    util_timer* prev;                                                           // 指向前一个定时器
    util_timer* next;                                                           // 指向后一个定时器
};

// 定时器链表（定时器容器），它是一个升序、双向链表，且带有头节点和尾节点。
class sort_timer_lst {
    public:
		sort_timer_lst();           		
		~sort_timer_lst();                                                      // 链表被销毁时，删除其中所有的定时器
   		void add_timer(util_timer* timer);                                      // 加入一个目标定时器，其中可能会调用属于private的一个重载函数
        void adjust_timer(util_timer* timer);                                   // 当一个定时任务发生改变时，调用这个，以调整其在链表中的位置。只考虑向后移动即时间延长的情况
    	void del_timer(util_timer* timer);                                      // 删除一个定时器
        void tick();                                                            // 每次SIGALRM信号被触发时，在其信号处理函数或是主函数（统一事件源时）中执行一次tick()，以处理其中过期的任务。
    private:
	// 一个重载的辅助函数，它被公有的add_timer函数和adjust_timer函数调用。该函数表示将目标定时器timer添加到节点lst_head之后的部分链表中
	void add_timer(util_timer* timer, util_timer* lst_head);
    util_timer* head;                                                           // 头节点
	util_timer* tail;                                                           // 尾节点
};

class Utils {
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);                                                    
    int setnonblocking(int fd);                                                 // 对文件描述符设置非阻塞
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);               // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    static void sig_handler(int sig);                                           // 信号处理函数
    void addsig(int sig, void(handler)(int), bool restart = true);              // 设置信号函数
    void timer_handler();                                                       // 定时处理任务，重新定时以不断触发SIGALRM信号
    void show_error(int connfd, const char *info);                              // 展示错误

public:
    static int *u_pipefd;                                                       // 指向WebServer的m_pipefd
    sort_timer_lst m_timer_lst;
    static int u_epollfd;                                                       // WebServer正在监听的epoll句柄m_epollfd
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);                                           // 回调函数

#endif