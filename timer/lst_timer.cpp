#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst() {
    head = NULL;
    tail = NULL;
}

// 链表被销毁时，删除其中所有的定时器
sort_timer_lst::~sort_timer_lst() {
    util_timer* tmp = head;
    while (tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 加入一个目标定时器，其中可能会调用属于private的一个重载函数
void sort_timer_lst::add_timer(util_timer* timer) {
    if (!timer) {
        return;                                         // 需要添加的为空，直接返回
    }

    if (!head) {
        head = tail = timer;                            // 如果链表为空，timer加入后既是头节点，也是尾节点
    }

    /*
        如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部,作为链表新的头节点
        否则就需要调用重载函数 add_timer(),把它插入链表中合适的位置，以保证链表的升序特性
    */
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

// 当一个定时任务发生改变时，调用这个，以调整其在链表中的位置。只考虑向后移动即时间延长的情况
void sort_timer_lst::adjust_timer(util_timer* timer) {
    if (!timer) {
        return;                                         // 需要调整的为空，直接返回            
    }   

    util_timer* tmp = timer->next;
    // 如果被调整的目标定时器处在链表的尾部，或者该定时器新的超时时间值仍然小于其下一个定时器的超时时间则不用调整
    if (!tmp || (timer->expire < tmp->expire)) {
        return;
    }

    // 如果目标定时器是链表的头节点，则将该定时器从链表中取出并重新插入链表
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer( timer, head );
    }
    else {
        // 如果目标定时器不是链表的头节点，则将该定时器从链表中取出，然后插入其原来所在位置后的部分链表中
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer( timer, timer->next );
    }
}

// 将目标定时器timer从链表中删除
void sort_timer_lst::del_timer(util_timer* timer) {
    if (!timer) {
        return;                                         // 需要删除的为空，直接返回            
    }  

    // 下面这个条件成立表示链表中只有一个定时器，即目标定时器
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }

    // 如果链表中至少有两个定时器，且目标定时器是链表的头节点，则将链表的头节点重置为原头节点的下一个节点，然后删除目标定时器
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }

    // 如果链表中至少有两个定时器，且目标定时器是链表的尾节点，则将链表的尾节点重置为原尾节点的前一个节点，然后删除目标定时器
    if (timer == tail) {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }

    // 如果目标定时器位于链表的中间，则把它前后的定时器串联起来，然后删除目标定时器
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// SIGALARM信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务
void sort_timer_lst::tick() {
    if (!head) {
        return;                                         // 链表为空，不执行操作
    }

    time_t cur = time(NULL);                            // 获取当前系统时间
    util_timer* tmp = head;

    // 从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
    while (tmp) {
        // 因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和系统当前时间，比较以判断定时器是否到期
        if(cur < tmp->expire) {
            break;                                      // 遇到了一个还没到期的定时器，由于是升序的，后面的肯定也还没到期，直接跳出
        }

        tmp->cb_func(tmp->user_data);                   // 调用定时器的回调函数，以执行定时任务
        head = tmp->next;                               // 执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点             
        if (head) {
            head->prev = NULL;
        }              
        delete tmp;
        tmp = head;
    }

}   

// 一个重载的辅助函数，它被公有的add_timer函数和adjust_timer函数调用该函数表示将目标定时器timer添加到节点lst_head之后的部分链表中
void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head) {
    util_timer* prev = lst_head;
    util_timer* tmp = prev->next;
    // 遍历 list_head 节点之后的部分链表，直到找到一个超时时间大于目标定时器的超时时间节点并将目标定时器插入该节点之前
    while (tmp) {
        if (timer->expire < tmp->expire) {
            prev->next = timer;         
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }

    // 如果遍历完 lst_head 节点之后的部分链表，仍未找到超时时间大于目标定时器的超时时间的节点，则将目标定时器插入链表尾部，并把它设置为链表新的尾节点。
    if (!tmp) {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    // ET
    if (TRIGMode == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }

    // LT
    else {
        event.events = EPOLLIN | EPOLLRDHUP;    
    }

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig) {
    int save_errno = errno;                                                 //为保证函数的可重入性，保留原来的errno，可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);                                  // 将信号值从管道写端写入，传输字符类型，而非整型，将信号发送给主循环，信号处理函数不要占用太多时间
    errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;                                                    // 创建sigaction结构体变量
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;                                                // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);                                                // 将所有信号添加到信号集中      
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() {
    m_timer_lst.tick();                                                     // 定时处理任务，实际上就是调用tick()函数
    alarm(m_TIMESLOT);                                                      // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号
}

void Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 静态变量类外初始化
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
// 定时器回调函数
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);       //删除非活动连接在socket上的注册事件
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}