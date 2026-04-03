/* J. David's webserver */
/* This is a simple webserver.
 * Created November 1999 by J. David Blackstone.
 * CSE 4344 (Network concepts), Prof. Zeigler
 * University of Texas at Arlington
 */
/* This program compiles for Sparc Solaris 2.6.
 * To compile for Linux:
 *  1) Comment out the #include <pthread.h> line.
 *  2) Comment out the line that defines the variable newthread.
 *  3) Comment out the two lines that run pthread_create().
 *  4) Uncomment the line that runs accept_request().
 *  5) Remove -lsocket from the Makefile.
 */
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdint.h>
#include "sds/sds.h"
#include "thpool.h"
#include <signal.h>
#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"
#define STDIN   0
#define STDOUT  1
#define STDERR  2

void accept_request(void *);
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, sds *);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startup(u_short *);
void unimplemented(int);

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client */
/**********************************************************************/
void accept_request(void *arg)
{
    // 这里是消费者端。之前在 main 函数里，为了防止多线程并发时抢夺同一个局部变量的 fd，
    // 用 malloc 给每个请求分配了独立堆内存。
    // 这里拿到真正的 socket 号后，第一件事就是赶紧 free 掉
    // 不然压测跑几万个并发，内存不出十分钟就会彻底爆掉。
int client = *(int *)arg;  // 从内存地址里取出真正的 socket 号
free(arg);                 // 释放 main 函数里 malloc 的内存
// 【底层安全改造：引入 SDS 防溢出】
    // 原版 TinyHTTPd 在这里搞了个 char buf[1024]，一旦攻击者发个 2000 字节的恶意 URL，
    // 整个服务器直接段错误崩溃。
    // 这里换成 Redis 的 sds（简单动态字符串），初始化为空。
    // 后续底层读取时如果超长，sds 内部会自动扩容（realloc），从根本上免疫了缓冲区的溢出攻击。
    sds buf = sdsempty(); // 初始化为空的 SDS
 ssize_t numchars;         // get_line 现在返回 int
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    struct stat st;
    int cgi = 0;      /* becomes true if server decides this is a CGI
                       * program */
    char *query_string = NULL;
// 获取 HTTP 请求的第一行（Request Line），传入 buf 的地址让其在内部动态扩容
    numchars = get_line(client, &buf);
    i = 0; j = 0;
// HTTP 协议解析：请求方法
    // 好处：sds 的内部结构确保了它的指针直接指向字符串数据的开头，
    // 所以这里无需改动原版逻辑，直接把 buf 当作普通数组用下标遍历即可，无缝兼容。
    while (!ISspace(buf[i]) && (i < sizeof(method) - 1))
    {
// 将合法的方法字符逐一拷贝入局部栈内存中
        method[i] = buf[i];
        i++;
    }
// 同步 j 游标，记录当前解析到的报文偏移量，为后续提取 URL 保存状态
    j=i;
    method[i] = '\0';
// 利用忽略大小写的字符串比对，拦截除 GET 和 POST 以外的不合法或不支持的请求方法
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unimplemented(client);
        return;
    }
// HTTP 协议规范约束：POST 请求用于提交实体数据，强制触发动态 CGI 脚本处理逻辑
    if (strcasecmp(method, "POST") == 0)
        cgi = 1;

    i = 0;
    while (ISspace(buf[j]) && (j < (size_t)numchars))
        j++;
// 开始提取 URL，开启双重安全防护：既不越过 url 栈数组上限，也不越过实际报文长度
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < (size_t)numchars))
    {
        url[i] = buf[j];
        i++; j++;
    }
// 同样执行手动封口，截断 URL 字符串
    url[i] = '\0';

    if (strcasecmp(method, "GET") == 0)
    {
        query_string = url;
        while ((*query_string != '?') && (*query_string != '\0'))
            query_string++;
        if (*query_string == '?')
        {
// 只要带参数，说明请求的是动态计算接口，强行置起 CGI 标志
            cgi = 1;
// 【核心算法：原位切割】将 '?' 强行替换为 '\0'，将一块连续内存瞬间截断为两半
            *query_string = '\0';
            query_string++;
        }
    }

    sprintf(path, "htdocs%s", url);
    if (path[strlen(path) - 1] == '/')
        strcat(path, "index.html");
    if (stat(path, &st) == -1) {
        // SDS 与 strcmp 兼容，所以逻辑不变，只改函数调用
// 【网络排错踩坑点：清空接收缓冲区】
        // 如果文件不存在（404）， 不会close socket 直接结束。
        // 客户端可能还在继续发 HTTP 头，如果直接关连接，客户端会收到 TCP RST 错误。
        // 所以这里利用改写后的 get_line 配合 sds，把剩下的协议头空读一遍，保证连接能稳定断开。
while ((numchars > 0) && strcmp("\n", buf)) {
// 必须读完残留报文，否则单方面 close socket 会导致客户端触发 TCP RST (连接被重置) 异常
    numchars = get_line(client, &buf);
}
        not_found(client);
    }
    else
    {
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            strcat(path, "/index.html");
// 【核心路由判断】利用位掩码 (Bitmask) 嗅探该文件的权限属性
        // 只要文件所有者、所在组或其他人具有可执行权限 (x)
        if ((st.st_mode & S_IXUSR) ||
                (st.st_mode & S_IXGRP) ||
                (st.st_mode & S_IXOTH)    )
            cgi = 1;
        if (!cgi)
            serve_file(client, path);
        else
            execute_cgi(client, path, method, query_string);
    }

   close(client);
// 【内存管理：闭环回收 2】
    // Worker 线程准备处理下一个任务了，上一个就不能留存没用的东西。
    // SDS 的内存在堆上，使用完毕必须调用 sdsfree 彻底销毁，
    // 配合开头的 free(arg)，实现了常驻线程在高并发冲击下极强的稳定性
sdsfree(buf);
}

/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
void bad_request(int client)
{
     sds buf = sdsempty();

    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
sdsfree(buf);
}

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat */
/**********************************************************************/
void cat(int client, FILE *resource)
{
    sds buf = sdsempty();

    fgets(buf, sizeof(buf), resource);
    while (!feof(resource))
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resource);
    }
sdsfree(buf);
}

/**********************************************************************/
/* Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor. */
/**********************************************************************/
void cannot_execute(int client)
{
    sds buf = sdsempty();

    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
sdsfree(buf);
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}

/**********************************************************************/
/* Execute a CGI script.  Will need to set environment variables as
 * appropriate.
 * Parameters: client socket descriptor
 *             path to the CGI script */
/**********************************************************************/
void execute_cgi(int client, const char *path,
        const char *method, const char *query_string)
{
    sds buf = sdsempty();
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    int i;
    char c;
    int numchars = 1;
    int content_length = -1;

    buf[0] = 'A'; buf[1] = '\0';
    if (strcasecmp(method, "GET") == 0)
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
            numchars = get_line(client, &buf);
    else if (strcasecmp(method, "POST") == 0) /*POST*/
    {
        numchars = get_line(client, &buf);
        while ((numchars > 0) && strcmp("\n", buf))
        {
            buf[15] = '\0';
            if (strcasecmp(buf, "Content-Length:") == 0)
                content_length = atoi(&(buf[16]));
            numchars = get_line(client, &buf);
        }
        if (content_length == -1) {
            bad_request(client);
            return;
        }
    }
    else/*HEAD or other*/
    {
    }


    if (pipe(cgi_output) < 0) {
        cannot_execute(client);
        return;
    }
    if (pipe(cgi_input) < 0) {
        cannot_execute(client);
        return;
    }

    if ( (pid = fork()) < 0 ) {
        cannot_execute(client);
        return;
    }
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    if (pid == 0)  /* child: CGI script */
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];

        dup2(cgi_output[1], STDOUT);
        dup2(cgi_input[0], STDIN);
        close(cgi_output[0]);
        close(cgi_input[1]);
        sprintf(meth_env, "REQUEST_METHOD=%s", method);
        putenv(meth_env);
        if (strcasecmp(method, "GET") == 0) {
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        }
        else {   /* POST */
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }
        execl(path, path, (char *)NULL);
        exit(0);
    } else {    /* parent */
        close(cgi_output[1]);
        close(cgi_input[0]);
        if (strcasecmp(method, "POST") == 0)
            for (i = 0; i < content_length; i++) {
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);
            }
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);

        close(cgi_output[0]);
        close(cgi_input[1]);
        waitpid(pid, &status, 0);
    }
sdsfree(buf);
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the bffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
/*******************/
int get_line(int sock, sds *out_sds) {
// 传入 sds 的二级指针 (*out_sds)。
    // 因为内部触发扩容(realloc)时，如果在原地无法扩展，系统会在堆区开辟新内存并将数据迁移，
    // 原指针地址会失效。通过二级指针才能将更新后的内存地址同步回传给调用方。
int i = 0;
    char c = '\0';
    int n;

    // 1. 初始化 SDS：如果传入的是空指针，先分配内存
    if (*out_sds == NULL) {
        *out_sds = sdsempty();
    } else {
        // 如果 sds 已经分配过内存，sdsclear 只是将头部的 len 属性置为 0，
        // 并不真正 free 底层空间。这样极大减少了高并发长连接下频繁系统调用的开销。
        sdsclear(*out_sds); // 如果已有内容，先清空
    }

    // 2. 核心循环：读取字符直至换行符
    while (c != '\n') {
        n = recv(sock, &c, 1, 0);
        if (n > 0) {
            if (c == '\r') {
// 网络协议嗅探：使用 MSG_PEEK 标志位“偷看” TCP 接收缓冲区中的下一个字符，
                // 确认是不是 \n，且不会把该字符从缓冲区中真正消耗掉，以此兼容各种操作系统的换行符。
                n = recv(sock, &c, 1, MSG_PEEK);
                if ((n > 0) && (c == '\n'))
                    recv(sock, &c, 1, 0);
                else
                    c = '\n';
            }
            // 核心替换：不再使用 buf[i] = c
            // 而是使用 sdscatlen 动态追加，自动处理扩容
// 废弃原版 buf[i] = c 的危险指针偏移操作。
            // sdscatlen 内部以 O(1) 复杂度校验剩余容量，空间不足时会依据 sdsMakeRoomFor 算法自动扩容。
            // 这一步从根本上免疫了黑客发送超长恶意的 HTTP Header 导致的栈溢出 (Stack Overflow) 攻击。
            *out_sds = sdscatlen(*out_sds, &c, 1);
            i++;
        } else {
            c = '\n';
        }
    }
    
    // SDS 内部自带 '\0'，不需要像原版那样手动添加 buf[i] = '\0'
// 二进制安全与向下兼容：SDS 每次追加数据后，内部会自动在有效数据末尾维护一个 '\0'，
    // 因此无需像原版那样手动封口，且能完美兼容后续调用的 strcasecmp 等标准 C 库函数。
    return i; 
}
void headers(int client, const char *filename)
{
    char buf[1024];
    (void)filename;  /* could use filename to determine file type */

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
void not_found(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
void serve_file(int client, const char *filename)
{
    FILE *resource = NULL;
    int numchars = 1;
    sds buf = sdsempty();

    buf[0] = 'A'; buf[1] = '\0';
    while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
        numchars = get_line(client, &buf);

    resource = fopen(filename, "r");
    if (resource == NULL)
        not_found(client);
    else
    {
        headers(client, filename);
        cat(client, resource);
    }
    fclose(resource);
sdsfree(buf);
}

/**********************************************************************/
/* This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 * Returns: the socket */
/**********************************************************************/
int startup(u_short *port)
{
    int httpd = 0;
    int on = 1;
    struct sockaddr_in name;

    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1)
        error_die("socket");
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(*port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0)  
    {  
        error_die("setsockopt failed");
    }
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
        error_die("bind");
    if (*port == 0)  /* if dynamically allocating a port */
    {
        socklen_t namelen = sizeof(name);
        if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
            error_die("getsockname");
        *port = ntohs(name.sin_port);
    }
    if (listen(httpd, 5) < 0)
        error_die("listen");
    return(httpd);
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
/**********************************************************************/
void unimplemented(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void)
{
    // 忽略 SIGPIPE 信号。
    // 压测的时候发现，如果并发太高客户端突然断开连接，
    // 服务器继续往 socket 写数据会触发 SIGPIPE 导致整个进程默默退出。
    // 加上这行能防止单个异常连接拖垮整个服务器。
signal(SIGPIPE, SIG_IGN);
    int server_sock = -1;
    u_short port = 4000;
    int client_sock = -1;
    struct sockaddr_in client_name;
    socklen_t  client_name_len = sizeof(client_name);

    server_sock = startup(&port);
    printf("httpd running on port %d\n", port);
// 初始化 8 个线程的线程池
    // 之前原版是来一个请求就 pthread_create 一次，1000 并发压测时直接死机了。
    // 改用线程池可以复用线程，减少上下文切换开销，还能起到限流的作用。
threadpool thpool = thpool_init(8);
    while (1)
    {
        client_sock = accept(server_sock,
                (struct sockaddr *)&client_name,
                &client_name_len);
// 把原来的 error_die 换成了 perror + continue
        // 压测时如果瞬间并发太多（比如超出了 ulimit 文件描述符限制），accept 可能会失败。
        // 这时候不能让服务器直接退出，打印错误后继续等下一个请求就行。
if (client_sock == -1) {
            // 不要用 error_die 让整个服务器结束进程
            perror("accept failed");
            continue; // 忽略这个错误连接，继续循环等下一个
        }
// 这里必须用 malloc 动态分配内存来传参
        // 因为 while 循环跑得极快，如果直接传 &client_sock 的地址，
        // 线程池还没来得及处理，client_sock 的值可能就被下一次 accept 的新连接覆盖了。
        // 这会导致多个线程串行处理同一个 fd。用 malloc 隔离内存就能解决这个问题。
        // (对应的 free 内存释放操作放在了 accept_request 函数的末尾)
int *arg = (int *)malloc(sizeof(int));
        *arg = client_sock;
        thpool_add_work(thpool, (void (*)(void *))accept_request, (void *)arg);
    }

    close(server_sock);

    return(0);
}

