#define _POSIX_C_SOURCE 200809L
#include "../fs/vsfs.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum { PACKET=10000, CACHE=256, QUEUE=64, WORKERS=4 };
typedef struct { struct sockaddr_in peer; size_t len; char data[PACKET+1]; } job;
typedef struct { int used; struct sockaddr_in peer; char id[33]; size_t len; char request[PACKET+1],response[PACKET+1]; } entry;
static job jobs[QUEUE]; static size_t head,tail,count;
static entry cache[CACHE]; static int stopping,sockfd;
static volatile sig_atomic_t interrupted;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER,fs_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t available=PTHREAD_COND_INITIALIZER;
static vsfs *image;
static void on_signal(int sig) { (void)sig; interrupted=1; }
static int hexval(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static void execute(char *request,char *out,size_t cap) {
    char *save=NULL,*id=strtok_r(request," ",&save),*op=strtok_r(NULL," ",&save),*path=strtok_r(NULL," ",&save),*data=strtok_r(NULL," ",&save);
    if(!id || !op || !path || strtok_r(NULL," ",&save)) { snprintf(out,cap,"ERR INVALID_REQUEST"); return; }
    int rc=-1; errno=EINVAL; unsigned char bytes[VSFS_MAX_FILE];
    if(!strcmp(op,"PUT") && data) {
        size_t n=strlen(data); if(!strcmp(data,"-")) n=0;
        if(n%2 || n>2*VSFS_MAX_FILE) { snprintf(out,cap,"%s ERR INVALID_HEX",id); return; }
        for(size_t i=0;i<n;i+=2) {
            int a=hexval(data[i]),b=hexval(data[i+1]);
            if(a<0 || b<0) { snprintf(out,cap,"%s ERR INVALID_HEX",id); return; }
            bytes[i/2]=(unsigned char)(a*16+b);
        } rc=vsfs_put(image,path,bytes,n/2);
    } else if(!data && !strcmp(op,"GET")) {
        size_t n=sizeof(bytes); rc=vsfs_get(image,path,bytes,&n);
        if(!rc) { size_t k=(size_t)snprintf(out,cap,"%s OK ",id); const char *hex="0123456789abcdef";
            if(!n) { strcpy(out+k,"-"); return; }
            for(size_t i=0;i<n;i++) { out[k++]=hex[bytes[i]>>4]; out[k++]=hex[bytes[i]&15]; } out[k]=0; return;
        }
    } else if(!data && !strcmp(op,"LIST")) {
        char listing[7000]; rc=vsfs_list(image,path,listing,sizeof(listing));
        if(!rc) { snprintf(out,cap,"%s OK\n%s",id,listing); return; }
    } else if(!data && !strcmp(op,"MKDIR")) rc=vsfs_mkdir(image,path);
    else if(!data && !strcmp(op,"DELETE")) rc=vsfs_remove(image,path);
    if(!rc) snprintf(out,cap,"%s OK",id);
    else snprintf(out,cap,"%s ERR %d %s",id,errno,strerror(errno));
}
static void respond(job *j) {
    char out[PACKET+1],copy[PACKET+1],id[33]; size_t n=0;
    while(n<j->len && j->data[n]!=' ' && n<32) { char c=j->data[n];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_')) break;
        id[n]=c; n++;
    }
    if(!n || n>=j->len || j->data[n]!=' ' || memchr(j->data,0,j->len)) {
        const char *bad="ERR INVALID_REQUEST"; (void)sendto(sockfd,bad,strlen(bad),0,(struct sockaddr *)&j->peer,sizeof(j->peer)); return;
    } id[n]=0;
    pthread_mutex_lock(&fs_lock);
    int slot=-1,found=-1;
    for(int i=0;i<CACHE;i++) {
        if(!cache[i].used) { if(slot<0) slot=i; continue; }
        if(cache[i].peer.sin_addr.s_addr==j->peer.sin_addr.s_addr && cache[i].peer.sin_port==j->peer.sin_port && !strcmp(cache[i].id,id)) { found=i; break; }
    }
    if(found>=0) {
        entry *e=&cache[found];
        if(e->len==j->len && !memcmp(e->request,j->data,j->len)) strcpy(out,e->response);
        else snprintf(out,sizeof(out),"%s ERR ID_REUSED",id);
    } else if(slot<0) snprintf(out,sizeof(out),"%s ERR SESSION_FULL",id);
    else {
        memcpy(copy,j->data,j->len+1); execute(copy,out,sizeof(out));
        entry *e=&cache[slot]; e->used=1; e->peer=j->peer; strcpy(e->id,id);
        e->len=j->len; memcpy(e->request,j->data,j->len+1); strcpy(e->response,out);
    }
    pthread_mutex_unlock(&fs_lock);
    (void)sendto(sockfd,out,strlen(out),0,(struct sockaddr *)&j->peer,sizeof(j->peer));
}
static void *worker(void *arg) {
    (void)arg;
    for(;;) {
        pthread_mutex_lock(&queue_lock);
        while(!count && !stopping) pthread_cond_wait(&available,&queue_lock);
        if(!count && stopping) { pthread_mutex_unlock(&queue_lock); break; }
        job j=jobs[head]; head=(head+1)%QUEUE; count--;
        pthread_mutex_unlock(&queue_lock); respond(&j);
    } return NULL;
}
int main(int argc,char **argv) {
    if(argc!=3) { fprintf(stderr,"Usage: %s IMAGE PORT (loopback only)\n",argv[0]); return 2; }
    char *end; long port=strtol(argv[2],&end,10);
    if(*end || port<1 || port>65535) { fprintf(stderr,"Invalid port\n"); return 2; }
    image=vsfs_open(argv[1],0);
    if(!image && errno==ENOENT) image=vsfs_open(argv[1],1);
    if(!image) { perror("image"); return 1; }
    sockfd=socket(AF_INET,SOCK_DGRAM,0); if(sockfd<0) { perror("socket"); vsfs_close(image); return 1; }
    struct timeval timeout={0,200000};
    if(setsockopt(sockfd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0) { perror("timeout"); return 1; }
    struct sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(0x7f000001u); address.sin_port=htons((unsigned short)port);
    if(bind(sockfd,(struct sockaddr *)&address,sizeof(address))<0) { perror("bind"); close(sockfd); vsfs_close(image); return 1; }
    struct sigaction sa={0}; sa.sa_handler=on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    pthread_t threads[WORKERS]; int started=0;
    for(;started<WORKERS;started++) if(pthread_create(&threads[started],NULL,worker,NULL)) { interrupted=1; break; }
    fprintf(stderr,"UDP file server on 127.0.0.1:%ld (%d workers)\n",port,started);
    while(!interrupted) {
        job j; socklen_t len=sizeof(j.peer);
        ssize_t n=recvfrom(sockfd,j.data,sizeof(j.data),0,(struct sockaddr *)&j.peer,&len);
        if(n<0) { if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK) continue; perror("recvfrom"); break; }
        if(n>PACKET) continue;
        j.len=(size_t)n; j.data[n]=0;
        pthread_mutex_lock(&queue_lock);
        if(count<QUEUE) { jobs[tail]=j; tail=(tail+1)%QUEUE; count++; pthread_cond_signal(&available); }
        pthread_mutex_unlock(&queue_lock);
    }
    pthread_mutex_lock(&queue_lock); stopping=1; pthread_cond_broadcast(&available); pthread_mutex_unlock(&queue_lock);
    for(int i=0;i<started;i++) pthread_join(threads[i],NULL);
    close(sockfd); vsfs_close(image); return started==WORKERS?0:1;
}
