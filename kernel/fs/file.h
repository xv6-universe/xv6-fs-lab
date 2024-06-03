#pragma once

#include "param.h"
#include "xv6fs/file.h"
#include "vfs.h"

// struct devsw devsw[NDEV];

void fileinit(void);
struct file *filealloc(void);
struct file *filedup(struct file *f);
void fileclose(struct file *f);
int filestat(struct file *f, struct stat *st);
int fileread(struct file *f, uint64 addr, int n);
int filewrite(struct file *f, uint64 addr, int n);