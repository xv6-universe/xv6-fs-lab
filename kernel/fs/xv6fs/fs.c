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

#include "types.h"
#include "riscv.h"
#include "kernel/defs.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "../vfs.h"
#include "../defs.h"
#include "xv6_fcntl.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct xv6fs_super_block sb; 
struct filesystem_type xv6fs;
static struct filesystem_operations xv6fs_ops;
struct inode *xv6fs_geti(uint dev, uint inum, int inc_ref);

struct super_block *xv6fs_mount(const char *source) {
  struct super_block *root_block = kalloc();
  memset(root_block, 0, sizeof(*root_block));
  root_block->type = &xv6fs;
  root_block->root = xv6fs_geti(ROOTDEV, ROOTINO, 1);
  root_block->parent = 0;
  root_block->mountpoint = 0;
  root_block->op = &xv6fs_ops;
  root_block->root->op = &xv6fs_ops;
  // printf("root op: %p\n", root_block->op);
  root->private = &sb;
  return root_block;
}

int xv6fs_umount(struct super_block *sb) {
  // ignored at this stage
  return 0;
}

// Read the super block.
static void
readsb(int dev, struct xv6fs_super_block *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
xv6fs_fsinit() {
  readsb(ROOTDEV, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  bwrite(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// returns 0 if out of disk space.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        bwrite(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  bwrite(bp);
  brelse(bp);
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
//   files and current directories). xv6fs_geti() finds or
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
//   ip = xv6fs_geti(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from xv6fs_geti() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. xv6fs_geti() increments ip->ref so that the inode
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


// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode*
xv6fs_ialloc(struct super_block *root)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;
  struct inode *ip;
  struct xv6fs_inode *xv6fs_ip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(ROOTDEV, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = 3; // any problem?
      bwrite(bp);   // mark it allocated on the disk
      brelse(bp);
      ip = xv6fs_geti(ROOTDEV, inum, 1);
      // same to root, as in xv6 file system
      ip->op = root->op;
      // printf("ialloc: ip->op = %p\n", ip->op);
      ip->sb = root;
      // ip->nlink = dip->nlink; ??????
      if (ip->private == 0) { // do we really need it
        xv6fs_ip = kalloc();
        memset(xv6fs_ip, 0, sizeof(*xv6fs_ip)); // kalloc returns shit
        ip->private = xv6fs_ip;
      }

      return ip;
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void
xv6fs_iupdate(struct inode *inode)
{
  struct buf *bp;
  struct dinode *dip;
  struct xv6fs_inode *ip = inode->private;

  bp = bread(inode->dev, IBLOCK(inode->inum, sb));
  dip = (struct dinode*)bp->data + inode->inum%IPB;
  dip->type = inode->type;
  #ifdef REF
    printf("dip: %p\n", dip);
    printf("^iupdate! ino: %d has type %d\n", inode->inum, inode->type);
    printf("^iupdate! ino: %d has nlink %d\n", inode->inum, inode->nlink);
    printf("^iupdate! ino: %d has address %p\n", inode->inum, ip->addrs);
    printf("^iupdate! ino: %d has size: %d\n", inode->inum, inode->size);
  #endif
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = inode->nlink;
  dip->size = inode->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  bwrite(bp);
  brelse(bp);
}

// release inode in the memory
void xv6fs_release_inode(struct inode *ino) {
  #ifdef LINK
    printf("release inode %d\n", ino->inum);
  #endif
  if (ino->private != 0) {
    kfree(ino->private);
    ino->private = 0;
    ino->type = 0;
  }
}

// free the inode in both the memory and the disk
void xv6fs_free_inode(struct inode *ino) {
  #ifdef LINK
    printf("free inode %d\n", ino->inum);
  #endif
  if (ino->private != 0) {
    kfree(ino->private);
    ino->private = 0;
    ino->type = 0;
  }
}

// open a file
struct file *xv6fs_open(struct inode *ino, uint mode) {
  // printf("entering xv6fs_open()\n");
  struct xv6fs_inode *ip = ino->private;
  struct file *f;
  struct xv6fs_inode *xv6fs_f = 0;
  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    return 0;
  }

  if((f = filealloc()) == 0) {
    return 0;
  }

  xv6fs_f = kalloc();
  memset(xv6fs_f, 0, sizeof(*xv6fs_f));

  if(ip->type == T_DEVICE){
    xv6fs_f->type = FD_DEVICE;
    xv6fs_f->major = ip->major;
  } else {
    xv6fs_f->type = FD_INODE;
    f->off = 0;
  }
  f->inode = ino;
  f->private = xv6fs_f;
  f->readable = !(mode & O_WRONLY);
  f->writable = (mode & O_WRONLY) || (mode & O_RDWR);
  // printf("leaving xv6fs_open()\n");
  return f;
}

// close a file
void xv6fs_close(struct file *f) {
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    return;
  }
  
  f->ref = 0;

  if(f->private != 0 && f->inode == 0) { // this is a pipe
    struct pipe *pp = f->private;
    pipeclose(pp, f->writable);
  } else {
    iput(f->inode);
    kfree(f->private);
  }

}

// is this directory empty?
int xv6fs_isdirempty(struct inode *dir) {
  int off;
  struct xv6fs_dentry de;

  for(off=2*sizeof(de); off < dir->size; off+=sizeof(de)){
    if(xv6fs_readi(dir, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;

}

// release the dentry
void xv6fs_release_dentry(struct dentry *dentry) {
  // do nothing at this stage
}


// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
static uint
bmap(struct xv6fs_inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a[bn] = addr;
        bwrite(bp);
      }
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
xv6fs_itrunc(struct inode *ino)
{
  #ifdef REF
    printf("inode %d is truncated\n", ino->inum);
    printf("inode device number: %d\n", ino->dev);
  #endif
  struct xv6fs_inode *ip = ino->private;
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ino->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ino->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ino->dev, a[j]);
    }
    brelse(bp);
    bfree(ino->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ino->size = 0;
  xv6fs_iupdate(ino);
}



// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
xv6fs_readi(struct inode *ino, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ino->size || off + n < off) {
    printf("fuck yourself\n");
    return 0;
  }
  if(off + n > ino->size)
    n = ino->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ino->private, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ino->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
xv6fs_writei(struct inode *ino, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ino->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ino->private, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ino->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    bwrite(bp);
    brelse(bp);
  }

  if(off > ino->size)
    ino->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  xv6fs_iupdate(ino);

  return tot;
}

// Directories

int
xv6fs_namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct dentry*
xv6fs_dirlookup(struct inode *dp, const char *name)
{
  #ifdef LINK
    printf("entering dirlookup\n");
    printf("looking for %s\n", name);
  #endif
  uint off;
  struct xv6fs_dentry de;
  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    // printf("dirlookup: off = %d\n", off);
    if(xv6fs_readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    #ifdef LINK
    printf("dirlookup: name = %s\n", de.name);
    #endif
    if(de.inum == 0) {
      // printf("dirlookup: de.inum == 0\n");
      continue;
    }
    if(xv6fs_namecmp(name, de.name) == 0){
      // entry matches path element
      struct dentry *dentry = kalloc();
      struct inode *ino = xv6fs_geti(dp->dev, de.inum, 1);
      ino->op = dp->op;
      // printf("dirlookup: ino->op = %p\n", ino->op);
      dentry->op = dp->op;
      dentry->inode = ino;
      dentry->parent = dp;
      strncpy(dentry->name, name, DIRSIZ);
      #ifdef REF
        printf("dirlookup: I've found\n");
      #endif
      return dentry;
    }
  }

  #ifdef REF
    printf("dirlookup: not found\n");
  #endif

  return 0;
}

int xv6fs_link(struct dentry *target) {
  int off = 0;
  struct xv6fs_dentry de;
  struct inode *dp = target->parent;
  struct inode *son = target->inode;
  struct inode *ino = 0;
  char name[DIRSIZ];
  strncpy(name, target->name, DIRSIZ);
  #ifdef LINK
    printf("link: parent inode: %d\n", dp->inum);
    printf("link: son inode: %d\n", son->inum);
  #endif
  struct dentry *dd = xv6fs_dirlookup(dp, name);
  #ifdef LINK
    printf("link: dd = %p\n", dd->name);
  #endif
  if (dd != 0 && (ino = dd->inode) != 0) {
    iput(ino);
    return -1;
  }

  #ifdef LINK
    printf("link: file does not exist\n");
  #endif

  // look for an empty dentry
  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (xv6fs_readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) {
      panic("dirlink read");
    }
    if (de.inum == 0) {
      break;
    }
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = son->inum;
  if (xv6fs_writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) {
    return -1;
  }

  #ifdef LINK
    printf("link: link success\n");
  #endif

  return 0;
}

int xv6fs_unlink(struct dentry *d) {
  // now that dirlookup can't change off, we have to do it manually
  struct xv6fs_dentry de;
  struct inode *dp = d->parent;
  #ifdef REF
    printf("unlink: parent inode: %d\n", dp->inum);
  #endif
  char name[DIRSIZ];
  strncpy(name, d->name, DIRSIZ);
  uint off = 0;
  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (xv6fs_readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) {
      panic("dirlink read in unlink");
      return -1;
    }
    #ifdef REF
      printf("name: %s\n", de.name);
    #endif
    if (xv6fs_namecmp(name, de.name) == 0) {
      // panic("x");
      memset(&de, 0, sizeof(de));
      if (xv6fs_writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) {
        panic("unlink write");
        return -1;
      }
    }
  }

  return 0;
}


// create a file
int xv6fs_create(struct inode *dir, struct dentry *target, short type, short major, short minor) {
  struct inode *ino = target->inode;
  struct xv6fs_inode *ip = ino->private;
  ip->major = major;
  ip->minor = minor;
  return 0;
}

// get inode
struct inode *xv6fs_geti(uint dev, uint inum, int inc_ref) {
  // printf("entering xv6fs_geti\n");
  struct inode *ino = iget(dev, inum);
  if (!inc_ref) {
    ino->ref--;
  }
  #ifdef LINK
    printf("geti: ref cnt for ino %d: %d\n", inum, ino->ref);
  #endif
  // printf("xv6fs_geti: ino->private = %p\n", ino->private);
  if (ino->private == 0) { // first time, read from disk
    struct xv6fs_inode *ip = kalloc();
    memset(ip, 0, sizeof(*ip));
    ino->private = ip;
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    ip->dev = dev;
    ino->type = ip->type;
    ino->nlink = dip->nlink;
    ino->size = dip->size;
    ino->dev = dev;
    ino->ref = 1;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    #ifdef REF
      printf("dip: %p\n", dip);
      printf("geti: ino: %d has type %d\n", inum, ino->type);
      printf("geti: ino: %d has nlink %d\n", inum, ino->nlink);
      printf("geti: ino: %d has address %p\n", inum, ip->addrs);
    #endif
    brelse(bp);
  }

  return ino;
}

void xv6fs_update_lock(struct inode *ino) {
  struct xv6fs_inode *ip = kalloc();
    memset(ip, 0, sizeof(*ip));
    ino->private = ip;
    struct buf *bp = bread(ino->dev, IBLOCK(ino->inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + ino->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    ip->dev = ino->dev;
    ino->type = ip->type;
    ino->nlink = dip->nlink;
    ino->size = dip->size;
    ino->dev = ino->dev;
    ino->ref = 1;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    #ifdef REF
      printf("dip: %p\n", dip);
      printf("geti: ino: %d has type %d\n", inum, ino->type);
      printf("geti: ino: %d has nlink %d\n", inum, ino->nlink);
      printf("geti: ino: %d has address %p\n", inum, ip->addrs);
    #endif
    brelse(bp);
}

static struct filesystem_operations xv6fs_ops = {
  .mount = xv6fs_mount,
  .umount = xv6fs_umount,
  .alloc_inode = xv6fs_ialloc,
  .write_inode = xv6fs_iupdate,
  .release_inode = xv6fs_release_inode,
  .free_inode = xv6fs_free_inode,
  .trunc = xv6fs_itrunc,
  .open = xv6fs_open,
  .close = xv6fs_close,
  .read = xv6fs_readi,
  .write = xv6fs_writei,
  .create = xv6fs_create,
  .link = xv6fs_link,
  .unlink = xv6fs_unlink,
  .dirlookup = xv6fs_dirlookup,
  .release_dentry = xv6fs_release_dentry,
  .isdirempty = xv6fs_isdirempty,
  .init = xv6fs_fsinit,
  .geti = xv6fs_geti,
  .update_lock = xv6fs_update_lock,
};



struct filesystem_type xv6fs = {
  .type = "xv6fs",
  .op = &xv6fs_ops,
};