// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//
#include "fs/defs.h"
#include "fs/vfs.h"
#include "types.h"
#include "riscv.h"
#include "../defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "xv6fs/file.h"
#include "defs.h"
#include "xv6_fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  // printf("entering sys_dup\n");
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  // printf("exiting sys_dup\n");
  return fd;
}

uint64
sys_read(void)
{
  // printf("entering sys_read\n");
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  uint64 ret = fileread(f, p, n);
  return ret;
}

uint64
sys_write(void)
{
  // printf("entering sys_write\n");
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  uint64 ret = filewrite(f, p, n);
  // printf("exiting sys_write\n");
  return ret;
}

uint64
sys_close(void)
{
  #ifdef LINK
  printf("entering sys_close\n");
  #endif
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  #ifdef LINK
  printf("exiting sys_close\n");
  #endif
  return 0;
}

uint64
sys_fstat(void)
{
  // printf("entering sys_fstat\n");
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  uint64 ret = filestat(f, st);
  // printf("exiting sys_fstat\n");
  return ret;
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  #ifdef LINK
    printf("entering sys_link\n");
  #endif
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  if((ip = namei(old)) == 0){
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    return -1;
  }

  ip->nlink++;
  ip->op->write_inode(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0) {
    printf("go to bad from line 162\n");
    goto bad;
  }
  ilock(dp);
  struct dentry *de = kalloc();
  de->parent = dp;
  de->inode = ip;
  strncpy(de->name, name, DIRSIZ);
  if(dp->dev != ip->dev || dp->op->link(de) < 0){
    iunlockput(dp);
    kfree(de);
    printf("go to bad from line 172\n");
    goto bad;
  }
  kfree(de);
  #ifdef LINK
    printf("link success\n");
  #endif
  iunlockput(dp);
  iput(ip);

#ifdef LINK
  printf("exiting sys_link\n");
#endif
  return 0;

bad:
#ifdef LINK
  printf("this is bad\n");
#endif
  ilock(ip);
  ip->nlink--;
  ip->op->write_inode(ip);
  iunlockput(ip);
  return -1;
}

uint64
sys_unlink(void)
{
  #ifdef LINK
    printf("entering sys_unlink\n");
  #endif
  struct inode *ip, *dp;
  char name[DIRSIZ], path[MAXPATH];
  // uint off = 0;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  if((dp = nameiparent(path, name)) == 0){
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  struct dentry *d = dp->op->dirlookup(dp, name);
  if (d == 0 || (ip = d->inode) == 0) 
    goto bad;
  ilock(ip);

  #ifdef LINK
    printf("ref: %d, nlink: %d\n", ip->ref, ip->nlink);
  #endif

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !dp->op->isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  // memset(&de, 0, sizeof(de));
  // if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
  //   panic("unlink: writei");
  struct dentry *de = kalloc();
  de->parent = dp;
  de->inode = ip;
  strncpy(de->name, name, DIRSIZ);
  dp->op->unlink(de);
  kfree(de);

  if(ip->type == T_DIR){
    dp->nlink--;
    dp->op->write_inode(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  ip->op->write_inode(ip);
  iunlockput(ip);
#ifdef LINK
  printf("exiting sys_unlink\n");
#endif
  return 0;

bad:
  iunlockput(dp);
  #ifdef LINK
    printf("exiting sys_unlink\n");
  #endif
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  // printf("entering create\n");
  // printf("path: %s, type: %d, major: %d, minor: %d\n", path, type, major, minor);
  struct inode *ip = 0, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  // printf("gogogo\n");

  struct dentry *d = dp->op->dirlookup(dp, name);
  // printf("d = %p\n", d);
  if(d != 0 && (ip = d->inode) != 0) {
    // printf("create: file already exists\n");
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = dp->op->alloc_inode(root)) == 0) {
    // printf("create: alloc_inode failed\n");
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->nlink = 1;
  ip->type = type;
  #ifdef REF
    printf("create inode: %d\n", ip->inum);
    printf("create: type = %d\n", type);
  #endif
  ip->op->write_inode(ip);

  if (type == T_DIR) {
    #ifdef REF
      printf("create: type is T_DIR\n");
    #endif
    struct dentry *cur_dir = kalloc();
    cur_dir->parent = ip;
    cur_dir->inode = ip;
    strncpy(cur_dir->name, ".", DIRSIZ);
    if (ip->op->link(cur_dir) < 0) {
      kfree(cur_dir);
      goto fail;
    }
    #ifdef REF
      printf("link success in \".\"\n");
    #endif
    struct dentry *parent_dir = kalloc();
    parent_dir->parent = ip;
    parent_dir->inode = dp;
    strncpy(parent_dir->name, "..", DIRSIZ);
    if (ip->op->link(parent_dir) < 0) {
      kfree(parent_dir);
      goto fail;
    }
    #ifdef REF
      printf("link success in \"..\"\n");
    #endif
  }

  struct dentry *de = kalloc();
  de->inode = ip;
  de->parent = dp;
  strncpy(de->name, name, DIRSIZ);
  if (dp->op->link(de) < 0) {
    kfree(de);
    goto fail;
  }
  #ifdef REF
      printf("link success in myself\n");
    #endif
  if (dp->op->create(dp, de, type, major, minor) < 0) {
    kfree(de);
    goto fail;
  }
  kfree(de);

  if(type == T_DIR){
    // printf("create: type is T_DIR, now success\n");
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    dp->op->write_inode(dp);
  }

  iunlockput(dp);

  // printf("exiting create\n");
  return ip;

 fail:
  // something went wrong. de-allocate ip.
  // printf("create: fail\n");
  ip->nlink = 0;
  ip->op->write_inode(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  #ifdef LINK
  printf("entering sys_open\n");
  #endif
  char path[MAXPATH];
  int fd = 0, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  // printf("sys_open: path = %s\n", path);
  // printf("sys_open: omode = %d\n", omode);

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      return -1;
    }
  }

  if ((f = ip->op->open(ip, omode)) == 0 || (fd = fdalloc(f)) < 0) {
    if (f) {
      fileclose(f);
    }
    iunlockput(ip);
    return -1;
  }

  if (ip->type != T_DEVICE) {
    f->off = 0;
  }

  f->inode = ip;
  // printf("inode type: %d\n", ip->type);
  f->op = ip->op;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    // printf("%truncating file\n");
    ip->op->trunc(ip);
  }

  iunlock(ip);
#ifdef LINK
  printf("exiting sys_open\n");
#endif
  return fd;
}

uint64
sys_mkdir(void)
{
  // printf("entering sys_mkdir\n");
  char path[MAXPATH];
  struct inode *ip;

  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    return -1;
  }
  iunlockput(ip);
  // printf("exiting sys_mkdir\n");
  return 0;
}

uint64
sys_mknod(void)
{
  // printf("entering sys_mknod\n");
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    return -1;
  }
  iunlockput(ip);
  // printf("exiting sys_mknod\n");
  return 0;
}

uint64
sys_chdir(void)
{
  // printf("entering sys_chdir\n");
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  p->cwd = ip;
  // printf("exiting sys_chdir\n");
  return 0;
}

uint64
sys_exec(void)
{
  // printf("entering sys_exec\n");
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  // printf("finish exec\n");
  // printf("ret = %d\n", ret);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  // printf("exiting sys_exec\n");
  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  // printf("entering sys_pipe\n");
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  // printf("exiting sys_pipe\n");
  return 0;
}
