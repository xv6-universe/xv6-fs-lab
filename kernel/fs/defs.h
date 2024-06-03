#pragma once

#include "types.h"

struct stat;
struct file;
struct inode;

// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

// file.c
struct file* filealloc(void);
void               fileclose(struct file*);
struct file* filedup(struct file*);
void               fileinit(void);
int                fileread(struct file*, uint64, int n);
int                filestat(struct file*, uint64 addr);
int                filewrite(struct file*, uint64, int n);

// fs.c
void                fsinit(int);
int                 dirlink(struct inode*, char*, uint);
struct inode* dirlookup(struct inode*, char*, uint*);
struct inode* ialloc(uint, short);
struct inode* idup(struct inode*);
void                iinit();
void                ilock(struct inode*);
void                iput(struct inode*);
void                iunlock(struct inode*);
void                iunlockput(struct inode*);
void                iupdate(struct inode*);
struct inode*       iget(uint dev, uint inum);
int                 namecmp(const char*, const char*);
struct inode* namei(char*);
struct inode* nameiparent(char*, char*);
int                 readi(struct inode*, int, uint64, uint, uint);
void                stati(struct inode*, struct stat*);
int                 writei(struct inode*, int, uint64, uint, uint);
void                itrunc(struct inode*);

// #define REF
#define LINK