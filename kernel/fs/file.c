//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "kernel/defs.h"
#include "defs.h"
#include "fs/xv6fs/file.h"
#include "param.h"
#include "buf.h"
#include "vfs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs/xv6fs/defs.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];


struct {
  struct file file[NFILE];
} ftable;


void
fileinit(void)
{ // initialization of the file table
  for(int i = 0; i < NFILE; i++){
    ftable.file[i].private = 0;
    ftable.file[i].ref = 0;
  }
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      return f;
    }
  }
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  f->op->close(f); // todo: Just this?
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  ilock(f->inode);
  stati(f->inode, &st);
  iunlock(f->inode);
  if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
    return -1;
  return 0;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  // printf("entering fileread\n");
  int r = 0;

  if(f->readable == 0)
    return -1;

  // if(f->type == FD_PIPE){
  //   r = piperead(f->pipe, addr, n);
  // } else if(f->type == FD_DEVICE){
  //   if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
  //     return -1;
  //   r = devsw[f->major].read(1, addr, n);
  // if(f->inode->type == FD_INODE){
  //   ilock(f->ip);
  //   if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
  //     f->off += r;
  //   iunlock(f->ip);
  // } else {
  //   panic("fileread");
  // }
  // to consider 
  // printf("to consider\n");
  if (f->inode->type == FD_DEVICE) {
    // printf("Yixing Chen\n");
    r = devsw[CONSOLE].read(1, addr, n);
  } else {
    ilock(f->inode);
    // printf("all parameters: %d %d %d %d\n", 1, addr, f->off, n);
    if((r = f->inode->op->read(f->inode, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->inode);
  }

  // printf("exiting fileread\n");
  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;
  
  if (f->inode->type == FD_DEVICE) {
    ret = devsw[CONSOLE].write(1, addr, n);
  } else {
    // printf("##filewrite\n");
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      ilock(f->inode);
      if ((r = f->inode->op->write(f->inode, 1, addr + i, f->off, n1)) > 0) {
        f->off += r;
        // printf("acturally wrote %d bytes\n", r);
      }
      iunlock(f->inode);

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  }
  // } else {
  //   panic("filewrite");
  // }

  return ret;
}

