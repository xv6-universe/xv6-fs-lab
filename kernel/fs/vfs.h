#pragma once

#include "sleeplock.h"
#include "stat.h"
#include "types.h"

struct filesystem_type {
  const char *type;
  struct filesystem_operations *op;
};

#define DEVSIZ 32
#define MAXMNT 32

struct super_block {
  struct filesystem_type *type;
  struct filesystem_operations *op;
  struct super_block *parent;
  struct inode *root;
  struct dentry *mountpoint;
  char device[DEVSIZ];
  struct super_block *mounts[MAXMNT];
  void *private;
};

struct file {
  struct filesystem_operations *op;
  int ref; // reference count
  int off;
  char readable;
  char writable;
  struct inode *inode;
  void *private;
};

struct inode {
  struct filesystem_operations *op;
  struct super_block *sb;
  uint inum;             // Inode number
  int ref;               // Reference count
  struct sleeplock lock; // protects everything below here
  short type;
  uint dev;
  uint size;
  short nlink;
  void *private;
};

#define DIRSIZ 14

struct dentry {
  struct filesystem_operations *op;
  struct inode *parent;
  char name[DIRSIZ];
  struct inode *inode;
  char ismount;
  char deleted;
  int ref;
  void *private;
};

struct filesystem_operations {
  struct super_block *(*mount) (const char *source);
  int (*umount) (struct super_block *sb);
  struct inode *(*alloc_inode) (struct super_block *sb);
  void (*write_inode) (struct inode *ino);
  void (*release_inode) (struct inode *ino);
  void (*free_inode) (struct inode *ino);
  void (*trunc) (struct inode *ino);
  struct file *(*open) (struct inode *ino, uint mode);
  void (*close) (struct file *f);
  int (*read) (struct inode *ino, char dst_is_user, uint64 dst, uint off, uint n);
  int (*write) (struct inode *ino, char src_is_user, uint64 src, uint off, uint n);
  int (*create) (struct inode *dir, struct dentry *target, short type, short major, short minor);
  int (*link) (struct dentry *target);
  int (*unlink) (struct dentry *d);
  struct dentry *(*dirlookup) (struct inode *dir, const char *name);
  void (*release_dentry) (struct dentry *de);
  // Is the directory dp empty except for "." and ".." ?
  int (*isdirempty) (struct inode *dir);
  // initialize filesystem type
  void (*init) (void);
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
};

extern struct super_block *root;
extern struct devsw devsw[];
