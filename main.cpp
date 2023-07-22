#include "config/config.h"

int main(int argc, char *argv[]) {
    printf("服务器正在运行......\n");

    // 需要修改的数据库信息，登录名，密码，库名
    string user = "debian-sys-maint";
    string passwd = "4CPALFBRYq2w4BbV";
    string databasename = "mydb";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode, config.sql_num, config.thread_num,
                config.close_log, config.actor_model);

    // 日志
    server.log_write();

    // 数据库
    server.sql_pool();

    // 线程池
    server.thread_pool();

    // 触发方式
    server.trig_mode();

    // 监听
    server.eventListen();

    // 运行，事件处理
    server.eventLoop();

    return 0;
}
