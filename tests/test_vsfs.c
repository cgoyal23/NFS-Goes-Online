#define _POSIX_C_SOURCE 200809L
#include "../fs/vsfs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    char path[]="/tmp/vsfs-test-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); close(fd); unlink(path);
    vsfs *f=vsfs_open(path,1); assert(f);
    assert(vsfs_mkdir(f,"/docs")==0);
    assert(vsfs_mkdir(f,"/docs/nested")==0);
    unsigned char input[]={0,1,255,0,42},out[VSFS_MAX_FILE]; size_t len=sizeof(out);
    assert(vsfs_put(f,"/docs/nested/data",input,sizeof(input))==0);
    assert(vsfs_get(f,"/docs/nested/data",out,&len)==0 && len==sizeof(input) && !memcmp(input,out,len));
    assert(vsfs_remove(f,"/docs")<0 && errno==ENOTEMPTY);
    assert(vsfs_put(f,"/../escape",input,1)<0 && errno==EINVAL);
    assert(vsfs_put(f,"/docs//bad",input,1)<0 && errno==EINVAL);
    assert(vsfs_put(f,"/docs",input,1)<0 && errno==EISDIR);
    assert(vsfs_put(f,"/missing/file",input,1)<0);
    assert(vsfs_put(f,"/big",input,VSFS_MAX_FILE+1)<0 && errno==EFBIG);
    assert(vsfs_put(f,"/empty",NULL,0)==0);
    len=0; assert(vsfs_get(f,"/empty",out,&len)==0 && len==0);
    unsigned char full[VSFS_MAX_FILE]; memset(full,0xA5,sizeof(full));
    assert(vsfs_put(f,"/maximum",full,sizeof(full))==0);
    assert(vsfs_put(f,"/docs/nested/data","new",3)==0);
    vsfs_close(f); f=vsfs_open(path,0); assert(f);
    len=sizeof(out); assert(vsfs_get(f,"/docs/nested/data",out,&len)==0 && len==3 && !memcmp(out,"new",3));
    len=sizeof(out); assert(vsfs_get(f,"/maximum",out,&len)==0 && len==sizeof(full) && !memcmp(out,full,len));
    char listing[100]; assert(vsfs_list(f,"/docs",listing,sizeof(listing))==0 && !strcmp(listing,"nested/\n"));
    assert(vsfs_remove(f,"/docs/nested/data")==0);
    assert(vsfs_remove(f,"/docs/nested")==0);
    assert(vsfs_remove(f,"/docs")==0);
    assert(vsfs_remove(f,"/")<0);
    assert(vsfs_remove(f,"/empty")==0); assert(vsfs_remove(f,"/maximum")==0);
    for(int i=0;i<127;i++) { char name[32]; snprintf(name,sizeof(name),"/f%d",i); assert(vsfs_put(f,name,"x",1)==0); }
    assert(vsfs_put(f,"/overflow","x",1)<0 && errno==ENOSPC);
    assert(vsfs_put(f,"/f0","replacement",11)==0);
    assert(vsfs_remove(f,"/f1")==0); assert(vsfs_put(f,"/reused","ok",2)==0);
    vsfs_close(f); f=vsfs_open(path,0); assert(f); vsfs_close(f); unlink(path);
    puts("VSFS persistence, binary data, limits, allocation reuse and directory checks passed"); return 0;
}
