#pragma once
#include <sys/stat.h>
#include <sys/types.h>
#include "esp_err.h"
typedef enum { ESP_LINE_ENDINGS_LF, ESP_LINE_ENDINGS_CR, ESP_LINE_ENDINGS_CRLF } esp_line_endings_t;
typedef struct {
ssize_t (*write)(int,const void *,size_t);
int (*open)(const char *,int,int);
int (*fstat)(int,struct stat *);
int (*close)(int);
ssize_t (*read)(int,void *,size_t);
int (*fcntl)(int,int,int);
int (*fsync)(int);
} esp_vfs_fs_ops_t;
#define ESP_VFS_FLAG_STATIC 1
esp_err_t esp_vfs_register_fs(const char *,const esp_vfs_fs_ops_t *,int,void *);
