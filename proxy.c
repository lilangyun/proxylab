#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* About Cache */
#define MAX_OBJECT_NUM 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *user_connect_hdr = "Connection: close\r\n";
static const char *user_proxy_hdr = "Proxy-Connection: close\r\n";

typedef struct {
    char hostname[MAXLINE];
    char port[MAXLINE];
    char path[MAXLINE];
}URI;  // 统一资源定位符

typedef struct {
    char uri[MAXLINE];
    char buf[MAX_OBJECT_SIZE];
    int n;  //buf的字节大小
    int timelamp;  // 时间戳：实现LRU算法
}Act;  // 每个活动

typedef struct {
    Act act[MAX_OBJECT_NUM];
    int num;  // 当前缓存多少活动
}Cache;

static Cache cache;
static sem_t mutex, w;
static int readcnt, timelamp;


void doit(int fd);
int read_requesthdrs(rio_t *rp);
void parse_uri(char *uri, URI *uri_data);
void build_header(char *server, URI* uri_data, rio_t *rio);

void *thread(void *connfdp);  // 线程例程函数

void init_cache(){
    cache.num = 0;
    for(int i = 0; i < MAX_OBJECT_NUM; i++){
        cache.act[i].n = 0;
    }
    Sem_init(&mutex, 0, 1);
    Sem_init(&w, 0, 1);
    readcnt = 0;  // 读者数为0
    timelamp = 0;
}

// 查看cache
int query_cache(int connfd, char *uri){
    P(&mutex);
    readcnt++;
    if(readcnt == 1){
        P(&w);
    }
    V(&mutex);

    /* 查找 */
    int hit_flag = 0; 
    for(int i = 0; i < MAX_OBJECT_NUM; i++){
        if(!strcmp(cache.act[i].uri, uri)){
            // 直接发给客户端
            P(&mutex);
            cache.act[i].timelamp = timelamp++;
            V(&mutex);
            Rio_writen(connfd, cache.act[i].buf, cache.act[i].n);
            hit_flag = 1;
            break;
        }
    }

    P(&mutex);
    readcnt--;
    if(readcnt == 0){
        V(&w);
    }
    V(&mutex);
    if(hit_flag){
        return 1;
    }
    return 0;  // 没查到返回0
}

/* 写cache */
void write_cache(char *uri, char *buf, int n){
    P(&w);
    if(cache.num < MAX_OBJECT_NUM){
        int id = cache.num;
        memcpy(cache.act[id].buf, buf, n);
        strcpy(cache.act[id].uri, uri);
        cache.act[id].n = n;
        P(&mutex);
        cache.act[id].timelamp = timelamp++;
        V(&mutex);
        cache.num ++;
    }
    else{
        // 采用LRU替换策略
        int longest_lamp = timelamp;
        int idx;
        for(int i = 0; i < MAX_OBJECT_NUM; i++){
            if(cache.act[i].timelamp < longest_lamp){
                longest_lamp = cache.act[i].timelamp;
                idx = i;
            }
        }
        memcpy(cache.act[idx].buf, buf, n);
        strcpy(cache.act[idx].uri, uri);
        cache.act[idx].n = n;
        P(&mutex);
        cache.act[idx].timelamp = timelamp++;
        V(&mutex);
    }
    V(&w);
}

int main(int argc, char **argv) 
{
    // 忽略SIGPIPE信号
    Signal(SIGPIPE, SIG_IGN);

    int listenfd, *connfdp;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;  // 线程id

    /* Check command line args */
    if (argc != 2) {
	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	exit(1);
    }

    /* cache初始化 */
    init_cache();

    listenfd = Open_listenfd(argv[1]);
    while (1) {
	    clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));  // 这里每次重新分配内存理论上应该可以避免race问题
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);  //line:netp:tiny:accept
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        // 分配线程给connfd去处理
        Pthread_create(&tid, NULL, thread, connfdp);
    }
}

/*
 * doit - receive and send one HTTP request/response transaction
 */
/* $begin doit */
void doit(int fd) 
{
    int is_static;  //表示是否为静态内容
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char server[MAXLINE];  //转发给server的数据
    rio_t rio, server_rio;
    char cache_buf[MAX_OBJECT_SIZE];

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);  // connfd是用户与proxy进行connect的套接字
    if (!Rio_readlineb(&rio, buf, MAXLINE))  //line:netp:doit:readrequest
        return;  //从客户端connfd读取一行数据，如果没有输入则返回
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    if (strcasecmp(method, "GET")) {                     //line:netp:doit:beginrequesterr
        printf("Proxy doesn't implement this method\n");
        return ;  //确保只运行GET事务
    }                                                    //line:netp:doit:endrequesterr

    /* 检查cache中是否存在用户所请求的 */
    if(query_cache(fd, uri)){
        return ;
    }

    // 解析uri
    URI *uri_data=Malloc(sizeof(URI));
    parse_uri(uri, uri_data);

    // 设置header
    build_header(server, uri_data, &rio);  //从用户connfd套接字读取数据

    // 连接服务器
    int serverfd = Open_clientfd(uri_data->hostname, uri_data->port);
    if(serverfd < 0){
        printf("Connected fail\n");
        return ;
    }

    // 转发给服务器
    Rio_readinitb(&server_rio, serverfd);
    Rio_writen(serverfd, server, strlen(server));

    // 回复给客户端
    size_t n, cnt = 0;
    while((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0){
        printf("Proxy received %d bytes, then send\n", (int)n);
        memcpy(cache_buf + cnt, buf, n);
        cnt += n;
        Rio_writen(fd, buf, n);
    }

    if(cnt < MAX_OBJECT_SIZE){
        write_cache(uri, cache_buf, cnt);
    }

    // 关闭与server连接的proxy自己的描述符/文件句柄
    Close(serverfd);

    Free(uri_data);
}
/* $end doit */

/* $begin parse_uri */
void parse_uri(char *uri, URI *uri_data) 
{
    memset(uri_data, 0, sizeof(URI));
    char *hostname = strstr(uri, "//");
    if(!hostname){
        // 没找到主机名
        char *path = strstr(uri, "/");
        if(path){
            strcpy(uri_data->path, path);
        }
        return ;
    }
    else{
        // 找到主机名
        char *port = strstr(hostname+2, ":");
        if(port){
            int tmp;
            sscanf(port+1, "%d %s", &tmp, uri_data->path);
            sprintf(uri_data->port, "%d", tmp);
            *port = '\0';
        }
        else{
            char *path = strstr(hostname+2, "/");
            if(path){
                strcpy(uri_data->path, path);
                strcpy(uri_data->port, "80");
                *path = '\0';
            }
        }
        strcpy(uri_data->hostname, hostname+2);
    }
    return ;
}
/* $end parse_uri */

// 拼接成一个完整的http请求至server中
void build_header(char *server, URI* uri_data, rio_t *rio){
    char req_line[MAXLINE], host_hdr[MAXLINE];

    // GET请求行
    snprintf(req_line, MAXLINE, "GET %s HTTP/1.0\r\n", uri_data->path);

    // Host请求头
    snprintf(host_hdr, MAXLINE, "Host: %s\r\n", uri_data->hostname);

    // server--为完整的http请求
    sprintf(server, "%s%s%s%s%s\r\n", req_line, host_hdr, user_agent_hdr, user_connect_hdr, user_proxy_hdr);
}

/* Thread routine*/
void *thread(void *connfdp){  // v_argument_pointor 参数指针
    int connfd = *((int *)connfdp);
    Pthread_detach(Pthread_self());  // 线程分离
    Free(connfdp);
    doit(connfd);
    Close(connfd);
    return NULL;
}