#pragma once

#include "types.h"

struct stat;
struct xv6fs_file;
struct xv6fs_inode;

// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

// file.c
struct xv6fs_file* xv6fs_filealloc(void);
void               xv6fs_fileclose(struct xv6fs_file*);
struct xv6fs_file* xv6fs_filedup(struct xv6fs_file*);
void               xv6fs_fileinit(void);
int                xv6fs_fileread(struct xv6fs_file*, uint64, int n);
int                xv6fs_filestat(struct xv6fs_file*, uint64 addr);
int                xv6fs_filewrite(struct xv6fs_file*, uint64, int n);

// fs.c
void                xv6fs_fsinit(int);
int                 xv6fs_dirlink(struct xv6fs_inode*, char*, uint);
struct xv6fs_inode* xv6fs_dirlookup(struct xv6fs_inode*, char*, uint*);
struct xv6fs_inode* xv6fs_ialloc(uint, short);
struct xv6fs_inode* xv6fs_idup(struct xv6fs_inode*);
void                xv6fs_iinit();
void                xv6fs_ilock(struct xv6fs_inode*);
void                xv6fs_iput(struct xv6fs_inode*);
void                xv6fs_iunlock(struct xv6fs_inode*);
void                xv6fs_iunlockput(struct xv6fs_inode*);
void                xv6fs_iupdate(struct xv6fs_inode*);
int                 xv6fs_namecmp(const char*, const char*);
struct xv6fs_inode* xv6fs_namei(char*);
struct xv6fs_inode* xv6fs_nameiparent(char*, char*);
int                 xv6fs_readi(struct xv6fs_inode*, int, uint64, uint, uint);
void                xv6fs_stati(struct xv6fs_inode*, struct stat*);
int                 xv6fs_writei(struct xv6fs_inode*, int, uint64, uint, uint);
void                xv6fs_itrunc(struct xv6fs_inode*);
