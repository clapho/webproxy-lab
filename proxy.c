#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1024000
#define MAX_OBJECT_SIZE 102400
#define LRU_MAGIC_NUMBER 9999
// Least Recently Used
// LRU: 가장 오랫동안 참조되지 않은 페이지를 교체하는 기법

#define CACHE_OBJS_COUNT 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *requestline_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";

static const char *host_key = "Host";
static const char *connection_key = "Connection";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *user_agent_key = "User-Agent";

void *thread(void *vargsp);
void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_endServer(char *hostname, int port, char *http_header);

// cache function
void cache_init();
int cache_find(char *url);
void cache_uri(char *uri, char *buf);

void readerPre(int i);
void readerAfter(int i);

typedef struct
{
  char cache_obj[MAX_OBJECT_SIZE];
  char cache_url[MAXLINE];
  int LRU;     // least recently used 가장 최근에 사용한 것의 우선순위를 뒤로 미룸(캐시에서 삭제할 때)
  int isEmpty; // 이 블럭에 캐시 정보가 들었는지 Empty인지 체크

  int readCnt;      // count of readers
  sem_t wmutex;     // protects accesses to cache 세마포어 타입 1: 사용가능, 0: 사용불가능
  sem_t rdcntmutex; // protects accesses to readcnt
} cache_block;      // 캐시 블럭 구조체로 선언

typedef struct
{
  cache_block cacheobjs[CACHE_OBJS_COUNT]; // ten cache blocks
  int cache_num;                           // 캐시(10개) 넘버 부여
} Cache;

Cache cache;

int main(int argc, char **argv)
{
  int listenfd, *connfdp;
  socklen_t clientlen;
  char hostname[MAXLINE], port[MAXLINE];
  pthread_t tid;
  struct sockaddr_storage clientaddr;

  cache_init();

  if (argc != 2)
  {
    // fprintf: 출력을 파일에다 씀. strerr: 파일 포인터
    fprintf(stderr, "usage: %s <port> \n", argv[0]);
    exit(1); // exit(1): 에러 시 강제 종료
  }
  Signal(SIGPIPE, SIG_IGN);
  // 특정 클라이언트가 종료되어있다고 해서 남은 클라이언트가에 영향가지않게 그 한쪽 종료됐다는 시그널을 무시해라.
  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr);

    /* pthread_create의 경우 argp 인자가 void* 이다.
    따라서 연결 식별자를 인자로 넣어줄 수 있게 안전하게 포인터를 만들어준다. */
    connfdp = Malloc(sizeof(int));
    *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen); // 포인터가 가리키는 값을 연결 식별자 값으로.

    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s %s).\n", hostname, port);

    // 첫 번째 인자 *thread: 쓰레드 식별자
    // 두 번째: 쓰레드 특성 지정 (기본: NULL)
    // 세 번째: 쓰레드 함수
    // 네 번째: 쓰레드 함수의 매개변수
    Pthread_create(&tid, NULL, thread, connfdp);
  }
  return 0;
}

void *thread(void *vargp)
{
  int connfd = *((int *)vargp);
  Pthread_detach(pthread_self()); // 자기 자신을 분리해준다.
  // 각각의 연결이 별도의 쓰레드에 의해서 독립적으로 처리 -> 서버가 명시적으로 각각의 피어 쓰레드 종료하는 것 불필요 -> detach
  // 메모리 누수를 방지하기 위해서 사용
  Free(vargp);  // 동적 할당한 파일 식별자 포인터를 free해준다.
  doit(connfd); // 클라이언트 요청을 파싱
  Close(connfd);
  return NULL;
}

void doit(int connfd)
{
  int end_serverfd;

  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char endserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];
  int port;

  // rio: client's rio / server_rio: endserver's rio
  rio_t rio, server_rio;

  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);
  sscanf(buf, "%s %s %s", method, uri, version); // read the client reqeust line

  if (strcasecmp(method, "GET"))
  {
    printf("Proxy does not implement the method");
    return;
  }

  char url_store[100];
  strcpy(url_store, uri); // doit으로 받아온 connfd가 들고 있는 uri를 넣어준다
                          // uri는 path를 생각하면될까?

  // the url is cached?
  int cache_index;
  // in cache then return the cache content
  // cache_index 정수 선언, url_store에 있는 uri에 대한 캐시 인덱스를 뒤짐(cache_find:10개의 캐시블럭) 탐색 후 인덱스가 -1이 아니면
  if ((cache_index = cache_find(url_store)) != -1)
  {
    readerPre(cache_index); // 캐시 뮤텍스를 풀어줌(0->1)
    // 캐시에서 찾은 값은 connfd에 쓰고, 캐시에서 그 값을 바로 보내게 됨
    Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj));
    readerAfter(cache_index); // 닫아줌 1->0 doit 끝
    return;
  }
  // 캐시에 없는 경우
  // parse the uri to get hostname, file path, port
  parse_uri(uri, hostname, path, &port);

  // build the http header which will send to the end server
  build_http_header(endserver_http_header, hostname, path, port, &rio);

  // connect to the end server
  end_serverfd = connect_endServer(hostname, port, endserver_http_header);
  if (end_serverfd < 0)
  {
    printf("connection failed\n");
    return;
  }

  Rio_readinitb(&server_rio, end_serverfd);

  // write the http header to endserver
  Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header));

  // recieve message from end server and send to the client
  char cachebuf[MAX_OBJECT_SIZE];
  int sizebuf = 0;
  size_t n; // 캐시에 없을 때 찾아주는 과정?
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0)
  {
    // printf("proxy received %ld bytes, then send\n", n);
    sizebuf += n;
    // proxy 거쳐서 서버에서 response가 오는데, 그 응답을 저장하고 클라이언트에 보냄
    if (sizebuf < MAX_OBJECT_SIZE) // 작으면 response 내용을 적어 놓는다.
      strcat(cachebuf, buf);       // cachebuf에 buf(response값) 다 이어 붙혀 놓음(캐시내용)
    Rio_writen(connfd, buf, n);
  }
  Close(end_serverfd);

  // store it
  if (sizebuf < MAX_OBJECT_SIZE)
  {
    cache_uri(url_store, cachebuf);
  }
}

void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];

  // request line
  sprintf(request_hdr, requestline_hdr_format, path);

  // get other request header for client rio and change it
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0)
  {
    if (strcmp(buf, endof_hdr) == 0)
      break; // EOF

    if (!strncasecmp(buf, host_key, strlen(host_key)))
    {
      strcpy(host_hdr, buf);
      continue;
    }

    if (!strncasecmp(buf, connection_key, strlen(connection_key)) && !strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key)) && !strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
    {
      strcat(other_hdr, buf);
    }
  }
  if (strlen(host_hdr) == 0)
  {
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s",
          request_hdr,
          host_hdr,
          conn_hdr,
          prox_hdr,
          user_agent_hdr,
          other_hdr,
          endof_hdr);
  return;
}

// Connect to the end server
inline int connect_endServer(char *hostname, int port, char *http_header)
{
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);
}

// parse the uri to get hostname, file path, port
void parse_uri(char *uri, char *hostname, char *path, int *port)
{
  *port = 80;
  char *pos = strstr(uri, "//");

  pos = pos != NULL ? pos + 2 : uri;

  char *pos2 = strstr(pos, ":");
  // sscanf(pos, "%s", hostname);
  if (pos2 != NULL)
  {
    *pos2 = '\0';
    sscanf(pos, "%s", hostname);
    sscanf(pos2 + 1, "%d%s", port, path);
  }
  else
  {
    pos2 = strstr(pos, "/");
    if (pos2 != NULL)
    {
      *pos2 = '\0'; // 중간에 끊으려고
      sscanf(pos, "%s", hostname);
      *pos2 = '/';
      sscanf(pos2, "%s", path);
    }
    else
    {
      scanf(pos, "%s", hostname);
    }
  }
  return;
}

void cache_init()
{
  cache.cache_num = 0; // 맨 처음이니까
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    cache.cacheobjs[i].LRU = 0;     // LRU : 우선 순위를 미는 것. 처음이니까 0
    cache.cacheobjs[i].isEmpty = 1; // 1이 비어있다는 뜻

    // Sem_init : 세마포어 함수
    // Sem_init 첫 번째 인자: 초기화할 세마포어의 포인터
    // 두 번째: 0 - 쓰레드들끼리 세마포어 공유, 그 외 - 프로세스 간 공유
    // 세 번째: 초기 값
    // 뮤텍스 만들 포인터 / 0 : 세마포어를 뮤텍스로 쓰려면 0을 써야 쓰레드끼리 사용하는거라고 표시하는 것이 됨 / 1 : 초기값
    // 세마포어는 프로세스를 쓰는 것. 지금 세마포어를 쓰레드에 적용하고 싶으니까 0을 써서 쓰레드에서 쓰는거라고 표시, 나머지 숫자를 프로세스에서 쓰는거라는 표시.
    Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);     // wmutex : 캐시에 접근하는 것을 프로텍트해주는 뮤텍스
    Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1); // read count mutex : 리드카운트에 접근하는 것을 프로텍트해주는 뮤텍스

    cache.cacheobjs[i].readCnt = 0; // read count를 0으로 놓고 init를 끝냄
  }
}

void readerPre(int i)
{ // i = 해당인덱스
  // 내가 받아온 index오브젝트의 리드카운트 뮤텍스를 P함수(recntmutex에 접근을 가능하게) 해준다
  /* rdcntmutex로 특정 readcnt에 접근하고 +1해줌. 원래 0으로 세팅되어있어서, 누가 안쓰고 있으면 0이었다가 1로 되고 if문 들어감 */
  P(&cache.cacheobjs[i].rdcntmutex); // P연산(locking):정상인지 검사, 기다림 (P함수 비정상이면 에러 도출되는 로직임)
  cache.cacheobjs[i].readCnt++;      // readCnt 풀고 들어감
                                     /* 조건문 들어오면 그때서야 캐쉬에 접근 가능. 그래서 만약 누가 쓰고있어도 P, readCnt까지는 할 수 있는데 +1이 되니까 1->2가 되고
                                      그러면 캐시에 접근을 못하게 됨. but readerAfter에서 -1 다시 내려주기때문에 0, 1, 0 에서만 움직임 */
  if (cache.cacheobjs[i].readCnt == 1)
    P(&cache.cacheobjs[i].wmutex);   // write mutex 뮤텍스를 풀고(캐시에 접근)
  V(&cache.cacheobjs[i].rdcntmutex); // V연산 풀기(캐시 쫒아냄) / read count mutex
}

void readerAfter(int i)
{
  P(&cache.cacheobjs[i].rdcntmutex);
  cache.cacheobjs[i].readCnt--;
  if (cache.cacheobjs[i].readCnt == 0)
    V(&cache.cacheobjs[i].wmutex);
  V(&cache.cacheobjs[i].rdcntmutex);
}

int cache_find(char *url)
{
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    readerPre(i);
    //  인덱스 i에서 캐시가 비어 있지 않고    &&  내가 받아온 url과 캐시에 있는 url이 같은지 확인
    if (cache.cacheobjs[i].isEmpty == 0 && strcmp(url, cache.cacheobjs[i].cache_url) == 0)
    {
      readerAfter(i);
      return i;
    }
    readerAfter(i);
  }
  return -1;
}

int cache_eviction()
{                             // 캐시 쫓아내기
  int min = LRU_MAGIC_NUMBER; // 초기 min = 9999
  int minindex = 0;           // 초기 minindex = 0
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  { // i = 0 ~ 9까지 for문을 돌린다
    readerPre(i);
    if (cache.cacheobjs[i].isEmpty == 1)
    {               // 비어 있으면
      minindex = i; // minindex를 i로 초기화한다 -> 여기에 넣으려고
      readerAfter(i);
      break;
    }
    if (cache.cacheobjs[i].LRU < min)
    {                               // index의 LRU 값이 min보다 작으면
      minindex = i;                 // minindex를 i로 초기화해주고
      min = cache.cacheobjs[i].LRU; // min값도 index의 LRU값으로 초기화해준다.
      readerAfter(i);
      continue;
    }
    readerAfter(i);
  }
  return minindex; // minindex를 리턴한다.
}

void writePre(int i)
{
  P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i)
{
  V(&cache.cacheobjs[i].wmutex);
}

// update the LRU number except the new cache one
void cache_LRU(int index)
{
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++)
  {
    if (i == index)
    {
      continue;
    } // 새로들어온 index
    writePre(i);
    if (cache.cacheobjs[i].isEmpty == 0)
    {
      cache.cacheobjs[i].LRU--;
    }
    writeAfter(i);
  }
}

// cache the uri and content in cache
void cache_uri(char *uri, char *buf)
{
  int i = cache_eviction(); // LRU로 교체해야할 minindex

  writePre(i);

  strcpy(cache.cacheobjs[i].cache_obj, buf);
  strcpy(cache.cacheobjs[i].cache_url, uri);
  cache.cacheobjs[i].isEmpty = 0;
  cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER;
  cache_LRU(i);

  writeAfter(i);
}
