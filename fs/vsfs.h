#ifndef VSFS_H
#define VSFS_H
#include <stddef.h>
#define VSFS_MAX_FILE 4096
typedef struct vsfs vsfs;
/* All functions return 0 on success and -1 with errno on failure.
 * Callers must serialize operations on one image. */
vsfs *vsfs_open(const char *path, int create);
void vsfs_close(vsfs *fs);
int vsfs_put(vsfs *fs, const char *path, const void *data, size_t len);
int vsfs_get(vsfs *fs, const char *path, void *data, size_t *len);
int vsfs_mkdir(vsfs *fs, const char *path);
int vsfs_list(vsfs *fs, const char *path, char *out, size_t cap);
int vsfs_remove(vsfs *fs, const char *path);
#endif
