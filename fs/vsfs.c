#define _POSIX_C_SOURCE 200809L
#include "vsfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { N = 128, NAME = 48 };
typedef struct { uint32_t used, dir, parent, block, size; char name[NAME]; } inode;
typedef struct {
    char magic[8]; uint32_t version;
    unsigned char inode_bits[N/8], block_bits[N/8];
    inode nodes[N];
} metadata;
struct vsfs { int fd; metadata m; };
static int fail(int e) { errno=e; return -1; }
static int bit(unsigned char *b,int i) { return (b[i/8]>>(i%8))&1; }
static void set(unsigned char *b,int i,int v) {
    if(v) b[i/8]|=(unsigned char)(1u<<(i%8));
    else b[i/8]&=(unsigned char)~(1u<<(i%8));
}
static int transfer(int fd,void *buf,size_t len,off_t off,int write_op) {
    unsigned char *p=buf;
    while(len) {
        ssize_t n=write_op?pwrite(fd,p,len,off):pread(fd,p,len,off);
        if(n<0) { if(errno==EINTR) continue; return -1; }
        if(!n) return fail(EIO);
        p+=n; len-=(size_t)n; off+=n;
    }
    return 0;
}
static off_t block_offset(int b) { return (off_t)sizeof(metadata)+(off_t)b*VSFS_MAX_FILE; }
static int sync_meta(vsfs *f) {
    if(transfer(f->fd,&f->m,sizeof(f->m),0,1)<0) return -1;
    return fsync(f->fd);
}
static int child(vsfs *f,int p,const char *name) {
    for(int i=1;i<N;i++) if(f->m.nodes[i].used && f->m.nodes[i].parent==(uint32_t)p && !strcmp(f->m.nodes[i].name,name)) return i;
    return fail(ENOENT);
}
static int resolve(vsfs *f,const char *path,int parent,char *leaf) {
    if(!path || path[0]!='/' || strlen(path)>1023) return fail(EINVAL);
    if(!strcmp(path,"/")) return parent?fail(EINVAL):0;
    char copy[1024]; strcpy(copy,path+1);
    if(copy[strlen(copy)-1]=='/' || strstr(copy,"//")) return fail(EINVAL);
    int at=0; char *save=NULL,*part=strtok_r(copy,"/",&save);
    while(part) {
        if(strlen(part)>=NAME || !strcmp(part,".") || !strcmp(part,"..")) return fail(EINVAL);
        for(const unsigned char *s=(unsigned char *)part;*s;s++)
            if(*s<=32 || *s>=127) return fail(EINVAL);
        char *next=strtok_r(NULL,"/",&save);
        if(!f->m.nodes[at].dir) return fail(ENOTDIR);
        if(parent && !next) { strcpy(leaf,part); return at; }
        at=child(f,at,part); if(at<0) return -1;
        part=next;
    }
    return at;
}
vsfs *vsfs_open(const char *path,int create) {
    int flags=O_RDWR|(create?(O_CREAT|O_EXCL):0);
    int fd=open(path,flags,0600); if(fd<0) return NULL;
    struct flock lock={0}; lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET;
    if(fcntl(fd,F_SETLK,&lock)<0) { close(fd); return NULL; }
    vsfs *f=calloc(1,sizeof(*f)); if(!f) { close(fd); return NULL; } f->fd=fd;
    if(create) {
        memcpy(f->m.magic,"VSFS001",8); f->m.version=1;
        f->m.nodes[0].used=1; f->m.nodes[0].dir=1; set(f->m.inode_bits,0,1);
        if(ftruncate(fd,block_offset(N))<0 || sync_meta(f)<0) goto bad;
    } else {
        struct stat st;
        if(fstat(fd,&st)<0 || st.st_size!=block_offset(N) || transfer(fd,&f->m,sizeof(f->m),0,0)<0) { errno=EINVAL; goto bad; }
        if(memcmp(f->m.magic,"VSFS001",8) || f->m.version!=1) { errno=EINVAL; goto bad; }
        unsigned char seen[N/8]={0};
        for(int i=0;i<N;i++) {
            inode *n=&f->m.nodes[i];
            if(n->used>1 || n->used!=(uint32_t)bit(f->m.inode_bits,i)) { errno=EINVAL; goto bad; }
            if(!n->used) continue;
            if(n->dir>1 || n->parent>=N || !f->m.nodes[n->parent].used || !f->m.nodes[n->parent].dir || !memchr(n->name,0,NAME)) { errno=EINVAL; goto bad; }
            if(!n->dir) {
                if(n->block>=N || n->size>VSFS_MAX_FILE || bit(seen,(int)n->block)) { errno=EINVAL; goto bad; }
                set(seen,(int)n->block,1);
            }
        }
        if(!f->m.nodes[0].used || !f->m.nodes[0].dir || memcmp(seen,f->m.block_bits,sizeof(seen))) { errno=EINVAL; goto bad; }
    }
    return f;
bad: { int e=errno; close(fd); free(f); errno=e; return NULL; }
}
void vsfs_close(vsfs *f) { if(f) { close(f->fd); free(f); } }
static int make_node(vsfs *f,const char *path,int dir) {
    char name[NAME]; int p=resolve(f,path,1,name); if(p<0) return -1;
    if(child(f,p,name)>=0) return fail(EEXIST);
    int i; for(i=1;i<N && bit(f->m.inode_bits,i);i++) { }
    if(i==N) return fail(ENOSPC);
    inode *n=&f->m.nodes[i]; memset(n,0,sizeof(*n));
    n->used=1; n->dir=(uint32_t)dir; n->parent=(uint32_t)p; strcpy(n->name,name); set(f->m.inode_bits,i,1);
    return i;
}
int vsfs_mkdir(vsfs *f,const char *path) { if(make_node(f,path,1)<0) return -1; return sync_meta(f); }
int vsfs_put(vsfs *f,const char *path,const void *data,size_t len) {
    if(len>VSFS_MAX_FILE) return fail(EFBIG);
    int i=resolve(f,path,0,NULL),fresh=0;
    if(i<0) { if(errno!=ENOENT) return -1; i=make_node(f,path,0); if(i<0) return -1; fresh=1; }
    inode *n=&f->m.nodes[i]; if(n->dir) return fail(EISDIR);
    int b; for(b=0;b<N && bit(f->m.block_bits,b);b++) { }
    if(b==N) { if(fresh) { memset(n,0,sizeof(*n)); set(f->m.inode_bits,i,0); } return fail(ENOSPC); }
    unsigned char buf[VSFS_MAX_FILE]={0}; if(len) memcpy(buf,data,len);
    if(transfer(f->fd,buf,sizeof(buf),block_offset(b),1)<0 || fsync(f->fd)<0) {
        if(fresh) { memset(n,0,sizeof(*n)); set(f->m.inode_bits,i,0); } return -1;
    }
    if(!fresh) set(f->m.block_bits,(int)n->block,0);
    set(f->m.block_bits,b,1); n->block=(uint32_t)b; n->size=(uint32_t)len;
    return sync_meta(f);
}
int vsfs_get(vsfs *f,const char *path,void *data,size_t *len) {
    int i=resolve(f,path,0,NULL); if(i<0) return -1; inode *n=&f->m.nodes[i];
    if(n->dir) return fail(EISDIR);
    if(*len<n->size) return fail(ENOBUFS);
    if(transfer(f->fd,data,n->size,block_offset((int)n->block),0)<0) return -1;
    *len=n->size; return 0;
}
int vsfs_list(vsfs *f,const char *path,char *out,size_t cap) {
    int p=resolve(f,path,0,NULL); if(p<0) return -1;
    if(!f->m.nodes[p].dir) return fail(ENOTDIR);
    size_t used=0; if(!cap) return fail(ENOBUFS); out[0]=0;
    for(int i=1;i<N;i++) { inode *n=&f->m.nodes[i]; if(!n->used || n->parent!=(uint32_t)p) continue;
        int k=snprintf(out+used,cap-used,"%s%s\n",n->name,n->dir?"/":"");
        if(k<0 || (size_t)k>=cap-used) return fail(ENOBUFS);
        used+=(size_t)k;
    } return 0;
}
int vsfs_remove(vsfs *f,const char *path) {
    int i=resolve(f,path,0,NULL); if(i<0) return -1; if(!i) return fail(EBUSY);
    inode *n=&f->m.nodes[i];
    if(n->dir) { for(int j=1;j<N;j++) if(f->m.nodes[j].used && f->m.nodes[j].parent==(uint32_t)i) return fail(ENOTEMPTY); }
    else set(f->m.block_bits,(int)n->block,0);
    memset(n,0,sizeof(*n)); set(f->m.inode_bits,i,0); return sync_meta(f);
}
