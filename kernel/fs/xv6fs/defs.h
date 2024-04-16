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
struct xv6fs_file* filealloc(void);
void            fileclose(struct xv6fs_file*);
struct xv6fs_file* filedup(struct xv6fs_file*);
void            fileinit(void);
int             fileread(struct xv6fs_file*, uint64, int n);
int             filestat(struct xv6fs_file*, uint64 addr);
int             filewrite(struct xv6fs_file*, uint64, int n);

// fs.c
void            fsinit(int);
int             dirlink(struct xv6fs_inode*, char*, uint);
struct xv6fs_inode* dirlookup(struct xv6fs_inode*, char*, uint*);
struct xv6fs_inode* ialloc(uint, short);
struct xv6fs_inode* idup(struct xv6fs_inode*);
void            iinit();
void            ilock(struct xv6fs_inode*);
void            iput(struct xv6fs_inode*);
void            iunlock(struct xv6fs_inode*);
void            iunlockput(struct xv6fs_inode*);
void            iupdate(struct xv6fs_inode*);
int             namecmp(const char*, const char*);
struct xv6fs_inode* namei(char*);
struct xv6fs_inode* nameiparent(char*, char*);
int             readi(struct xv6fs_inode*, int, uint64, uint, uint);
void            stati(struct xv6fs_inode*, struct stat*);
int             writei(struct xv6fs_inode*, int, uint64, uint, uint);
void            itrunc(struct xv6fs_inode*);
