// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "fs/xv6fs/fs.h"
// #include "fs/xv6fs/file.h"
#include "types.h"
#include "riscv.h"
#include "kernel/defs.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs/vfs.h"
#include "buf.h"


#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct super_block root;
extern struct filesystem_type xv6fs;

// Init fs
void
fsinit(int dev) {
  // printf("entering fsinit\n");
  
  // struct filesystem_type fs_type = xv6fs; // suppose we've made one
  xv6fs.op->init();
  root.type = &xv6fs;
  root.op = xv6fs.op;
  root.op->mount("yuy");
  // printf("fsinit done\n");
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct inode inode[NINODE];
} itable;

struct {
  struct dentry dentry[NDENTRY];
} dtable;

void
iinit()
{
  // printf("entering iinit\n");
  
  int i = 0;
  
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
  // printf("iinit done\n");
}

struct dentry*
dgetblank()
{
  // printf("entering dgetblank\n");
  
  int i = 0;
  for(i = 0; i < NDENTRY; i++) {
    if (dtable.dentry[i].ref == 0) {
      dtable.dentry[i].ref = 1;
      // printf("dgetblank done\n");
      return &dtable.dentry[i];
    }
  }
  // printf("dgetblank done\n");
  return 0;
}

void
dfree(struct dentry *de)
{
  // printf("entering dfree\n");
  
  de->ref = 0;
  de->inode = 0;
  de->parent = 0;
  de->op = 0;
  memset(de->name, 0, DIRSIZ);
  de->ismount = 0;
  de->deleted = 0;
  de->private = 0;
  // printf("dfree done\n");
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
struct inode*
iget(uint dev, uint inum)
{
  // printf("entering iget\n");
  
  struct inode *ip, *empty;

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      #ifdef LINK
        printf("&node %d, ref++ in iget: %d\n", ip->inum, ip->ref);
      #endif
      // printf("iget done\n");
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->private = 0;
  #ifdef LINK
    printf("&node %d, ref=1 in iget: %d\n", ip->inum, ip->ref);
  #endif
  // printf("iget done\n");
  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  #ifdef DEBUG
    printf("entering idup\n");
  #endif
  ip->ref++;
  #ifdef LINK
    printf("&node %d, ref++ in idup: %d\n", ip->inum, ip->ref);
    printf("idup done\n");
  #endif
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  #ifdef LINK
    printf("entering ilock for node %d\n", ip->inum);
    printf("ref cnt for node %d: %d\n", ip->inum, ip->ref);
  #endif
  #ifdef LINK
    printf("entering ilock for node %d\n", ip->inum);
    printf("ref cnt for node %d: %d\n", ip->inum, ip->ref);
  #endif
  if(ip == 0 || ip->ref < 1) {
    panic("ilock");
  }

  acquiresleep(&ip->lock);
  // printf("ip->op: %p\n", ip->op);
  if (ip->private == 0) {
    ip->op->update_lock(ip);
  }
  // printf("ilock done\n");
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  #ifdef LINK
    printf("entering iunlock for node %d\n", ip->inum);
  #endif

  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) {
    if (ip == 0) {
      panic("iunlock: no inode");
    }if (holdingsleep(&ip->lock)) {
      panic("iunlock: no lock");
    } else if (ip->ref < 1) {
      panic("iunlock: no ref");
    }
    
    panic("iunlock");
  }

  releasesleep(&ip->lock);
  // printf("iunlock done\n");
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  // printf("entering iput\n");
  if (ip->private == 0) return;
  
  if(ip->ref == 1  && ip->nlink == 0) {
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    ip->type = 0;
    
    ip->op->trunc(ip);
    ip->op->write_inode(ip);

    ip->op->free_inode(ip);

    releasesleep(&ip->lock);
  }

  
  if (ip->ref == 1 && ip->nlink > 0) {
    acquiresleep(&ip->lock);
    ip->op->write_inode(ip);

    ip->op->release_inode(ip);

    releasesleep(&ip->lock);
  }

  ip->ref--;
  #ifdef LINK
    printf("&node %d, ref-- in iput: %d\n", ip->inum, ip->ref);
  #endif
  // printf("iput done\n");
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  // printf("entering iunlockput\n");
  
  iunlock(ip);
  iput(ip);
  // printf("iunlockput done\n");
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  // printf("entering stati\n");
  
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
  struct xv6fs_inode *xv6fs_ip = (struct xv6fs_inode *)ip->private;
  printf("stat for inode %p: dev %d, ino %d, type %d, nlink %d, size %d\n", ip->inum, st->dev, st->ino, st->type, st->nlink, st->size);
  // print addr for xv6fs_inode
  printf("xv6fs_inode addr: %p\n", xv6fs_ip);
  // printf("stati done\n");
}

// Directories

int
namecmp(const char *s, const char *t)
{
  // printf("entering namecmp\n");
  
  return strncmp(s, t, DIRSIZ);
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  // printf("entering skipelem\n");
  // printf("path: %s\n", path);
  // printf("name: %s\n", name);
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  // printf("skipelem result: %s\n", path);
  // printf("name: %s\n", name);
  // printf("skipelem done\n");
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  // printf("entering namex\n");
  // printf("path: %s\n", path);
  
  struct inode *ip, *next;

  if(*path == '/') {
    ip = iget(ROOTDEV, ROOTINO);
  }
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    // printf("path: %s\n", path);
    // printf("nameiparent: %d\n", nameiparent);
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      // printf("has mother\n");
      iunlock(ip);
      return ip;
    }
    struct dentry *de = ip->op->dirlookup(ip, name);
    if (de == 0 || (next = de->inode) == 0) {
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  // printf("entering namei\n");
  
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  // printf("entering nameiparent\n");
  
  return namex(path, 1, name);
}
