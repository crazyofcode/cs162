#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define MAX_INDIRECT_SECTOR 128
#define MAX_DIRECT_SECTOR 119

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct[119];
  block_sector_t indirect[6]; /* First data sector. */
  off_t length;         /* File size in bytes. */
  uint32_t is_dir;      /* 1 for directory, 0 for file. */
  unsigned magic;       /* Magic number. */
};

struct buf {
  int cnt;
  bool dirty;
  struct block *dev;
  block_sector_t blockno;
  struct list_elem elem;
  uint8_t data[BLOCK_SECTOR_SIZE];
};


/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

static void bunpin(struct buf *b);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos < inode->data.length) {
    size_t sector_idx = pos / BLOCK_SECTOR_SIZE;
    if (sector_idx < MAX_DIRECT_SECTOR)
      return inode->data.direct[sector_idx];
    else {
      sector_idx -= MAX_DIRECT_SECTOR;
      size_t indirect_idx = sector_idx / MAX_INDIRECT_SECTOR;
      size_t indirect_off = sector_idx % MAX_INDIRECT_SECTOR;
      struct buf *b = bread(fs_device, inode->data.indirect[indirect_idx]);
      block_sector_t *data = (block_sector_t *)&b->data;
      block_sector_t ret = data[indirect_off];
      bunpin(b);
      return ret;
    }
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock open_inodes_lock;
static struct list bcache_list;
static struct lock bcache_lock;

static void bunpin(struct buf *b) {
  lock_acquire(&bcache_lock);
  ASSERT(b->cnt > 0);
  b->cnt--;
  lock_release(&bcache_lock);
}

/* Initializes the inode module. */
void inode_init(void) { 
  list_init(&open_inodes);
  lock_init(&open_inodes_lock);
}

void binit(void) {
  struct buf *b = NULL;
  list_init(&bcache_list);
  lock_init(&bcache_lock);
  for (int i = 0; i < 64; i++) {
    b = malloc(sizeof(struct buf));
    if (b == NULL) PANIC("binit: out of memory");
    b->cnt = 0;
    b->dirty = false;
    b->dev = NULL;
    b->blockno = 0;
    list_push_back(&bcache_list, &b->elem);
  }
}

struct buf *bread(struct block *device, block_sector_t idx) {
  struct buf *b = NULL;
  struct list_elem *e;
  lock_acquire(&bcache_lock);

  /* Search for buffer in cache. */
  for (e = list_begin(&bcache_list); e != list_end(&bcache_list); e = list_next(e)) {
    b = list_entry(e, struct buf, elem);
    if (b->dev == device && b->blockno == idx) {
      b->cnt++;
      list_remove(&b->elem);
      list_push_front(&bcache_list, &b->elem);
      lock_release(&bcache_lock);
      return b;
    }
  }

  /* Not found.  Evict. */
  /* Find a buffer with cnt == 0 from the back (LRU). */
  for (e = list_rbegin(&bcache_list); e != list_rend(&bcache_list); e = list_prev(e)) {
    b = list_entry(e, struct buf, elem);
    if (b->cnt == 0)
      break;
  }

  /* If all buffers are busy, we can't evict. */
  if (e == list_rend(&bcache_list))
    PANIC("bread: buffer cache exhausted");

  /* Write back if dirty. */
  if (b->dirty) {
    block_write(b->dev, b->blockno, b->data);
    b->dirty = false;
  }

  /* Reassign buffer. */
  b->dev = device;
  b->blockno = idx;
  b->cnt = 1;
  b->dirty = false;

  /* Move to front. */
  list_remove(&b->elem);
  list_push_front(&bcache_list, &b->elem);
  
  /* Read data from disk into buffer (while holding lock to prevent others from stealing it, 
     though strictly reading could be done without lock if we marked it as "busy/reading"). 
     For simplicity in this codebase, we hold the lock or rely on cnt=1. 
     Since block_read is synchronous, we can just do it. */
  block_read(device, idx, b->data);
  
  lock_release(&bcache_lock);
  return b;
}

void bwrite(struct block *device, block_sector_t idx, off_t off, off_t length, const uint8_t *data) {
  struct buf *b = bread(device, idx);
  ASSERT(b != NULL);

  /* Modify data. */
  /* Note: bread returns a pinned buffer (cnt incremented). 
     We can safely modify it because we effectively own this reference. 
     However, other readers might be reading it concurrently if they also called bread.
     Pintos usually ignores this reader-writer race for the buffer cache project,
     or assumes external synchronization (filesys lock). */
  memcpy(b->data + off, data, length);
  b->dirty = true;

  /* Unpin. */
  bunpin(b);
}

void bflush(void) {
  struct buf *b = NULL;
  struct list_elem *e;

  lock_acquire(&bcache_lock);
  for (e = list_begin(&bcache_list); e != list_end(&bcache_list); e = list_next(e)) {
    b = list_entry(e, struct buf, elem);
    if (b->dirty && b->dev == fs_device) {
      block_write(b->dev, b->blockno, b->data);
      b->dirty = false;
      printf("bflush: wrote block %d\n", (int)b->blockno);
    }
  }
  lock_release(&bcache_lock);
}

static bool multi_free_map_allocate(size_t sectors, struct inode_disk *disk_inode) {
  bool success = true;
  size_t idx = 0;
  size_t cur_sectors = sectors < MAX_DIRECT_SECTOR ? sectors : MAX_DIRECT_SECTOR;
  if (success) {
    for (; idx < cur_sectors; idx ++) {
      success = free_map_allocate(1, &disk_inode->direct[idx]);
      if (!success) break;
    }
  }

  if (success && sectors > MAX_DIRECT_SECTOR) {
    size_t loop_times = DIV_ROUND_UP(sectors - MAX_DIRECT_SECTOR, MAX_INDIRECT_SECTOR);
    for (size_t i = 0; i < loop_times; i++) {
      success = free_map_allocate(1, &disk_inode->indirect[i]);
      if (!success) break;
      block_sector_t data[MAX_INDIRECT_SECTOR];
      for (size_t j = 0; j < MAX_INDIRECT_SECTOR; j++) {
        ++idx;
        success = free_map_allocate(1, &data[j]);
        if (idx >= sectors) break;
        if (!success) break;
      }
      bwrite(fs_device, disk_inode->indirect[i], 0, BLOCK_SECTOR_SIZE, (uint8_t *)data);
    }

  }
  return success;
}
static void multi_free_map_release(size_t sectors, struct inode_disk *disk_inode) {
  size_t idx = 0;
  for (; idx < MAX_DIRECT_SECTOR && idx < sectors; idx++) {
    free_map_release(disk_inode->direct[idx], 1);
  }
  for (size_t i = 0; idx < sectors; i++) {
    block_sector_t data[MAX_INDIRECT_SECTOR];
    struct buf *b = bread(fs_device, disk_inode->indirect[i]);
    memcpy(data, b->data, BLOCK_SECTOR_SIZE);
    bunpin(b);
    for (size_t j = 0; j < MAX_INDIRECT_SECTOR; j++) {
      ++idx;
      if (idx >= sectors) break;
      free_map_release(data[j], 1);
    }
  }
}
/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length, bool is_dir) {
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->is_dir = is_dir ? 1 : 0;
    if (multi_free_map_allocate(sectors, disk_inode)) {
      bwrite(fs_device, sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)disk_inode);
      if (sectors > 0) {
        static char zeros[BLOCK_SECTOR_SIZE];
        size_t i;

        if (sectors < MAX_DIRECT_SECTOR) {
          for (i = 0; i < sectors; i++)
            bwrite(fs_device, disk_inode->direct[i], 0, BLOCK_SECTOR_SIZE, (uint8_t *)zeros);
        } else {
          for (i = 0; i < MAX_DIRECT_SECTOR; i++)
            bwrite(fs_device, disk_inode->direct[i], 0, BLOCK_SECTOR_SIZE, (uint8_t *)zeros);
          for (size_t j = 0; ; j++) {
            struct buf *b = bread(fs_device, disk_inode->indirect[j]);
            block_sector_t *data = (block_sector_t *)b->data;
            for (size_t k = 0; k < MAX_INDIRECT_SECTOR; k++) {
              bwrite(fs_device, data[k], 0, BLOCK_SECTOR_SIZE, (uint8_t *)zeros);
              ++i;
              if (i >= sectors) break;
            }
            bunpin(b);
            if (i >= sectors) break;
          }
        }
      }
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  lock_acquire(&open_inodes_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode->open_cnt++;
      lock_release(&open_inodes_lock);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL) {
    lock_release(&open_inodes_lock);
    return NULL;
  }

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  lock_release(&open_inodes_lock);

  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  // block_read(fs_device, inode->sector, &inode->data);
  struct buf *b = bread(fs_device, inode->sector);
  memcpy(&inode->data, b->data, BLOCK_SECTOR_SIZE);
  bunpin(b);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL) {
    lock_acquire(&open_inodes_lock);
    inode->open_cnt++;
    lock_release(&open_inodes_lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire(&open_inodes_lock);
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);
    lock_release(&open_inodes_lock);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      size_t sectors = bytes_to_sectors(inode->data.length);
      multi_free_map_release(sectors, &inode->data);
    } else {
      bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
    }
    
    free(inode);
  } else {
    lock_release(&open_inodes_lock);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  // uint8_t* bounce = NULL;

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    // if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      // block_read(fs_device, sector_idx, buffer + bytes_read);
    //   struct buf *b = bread(fs_device, sector_idx);
    //   memcpy(buffer + bytes_read, b->data, BLOCK_SECTOR_SIZE);
    //   bunpin(b);
    // } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      // if (bounce == NULL) {
      //   bounce = malloc(BLOCK_SECTOR_SIZE);
      //   if (bounce == NULL)
      //     break;
      // }
      // block_read(fs_device, sector_idx, bounce);
    struct buf *b = bread(fs_device, sector_idx);
    memcpy(buffer + bytes_read, (const void *)&b->data[sector_ofs], chunk_size);
    bunpin(b);
    // }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  // free(bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  if (offset + size > inode_length(inode)) {
    if (!inode_resize(inode, offset + size - inode_length(inode)))
      return 0;
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    bwrite(fs_device, sector_idx, sector_ofs, chunk_size, buffer + bytes_written);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { 
  struct inode_disk data;
  struct buf *b = bread(fs_device, inode->sector);
  data = *(struct inode_disk *)b->data;
  bunpin(b);
  return data.length;
}

bool inode_isdir(struct inode *inode) {
  return inode->data.is_dir == 1;
}

void inode_set_dir(struct inode *inode, bool is_dir) {
  inode->data.is_dir = is_dir ? 1 : 0;
  bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
}

static bool inode_resize_helper(off_t newsz, size_t old_sectors, size_t sectors, struct inode *inode) {
  static char zeros[BLOCK_SECTOR_SIZE];
  bool success = true;
  if (old_sectors == 0) {
    success = multi_free_map_allocate(sectors, &inode->data);
    if (success) {
      for (size_t i = 0; i < sectors; i++) {
        // Zero out newly allocated sectors
        if (i < MAX_DIRECT_SECTOR)
          bwrite(fs_device, inode->data.direct[i], 0, BLOCK_SECTOR_SIZE, (uint8_t *)zeros);
        else {
          // This part is complex, multi_free_map_allocate already handles initial creation zeroing 
          // but if we are here via resize it might be different.
          // Actually inode_create uses multi_free_map_allocate and then zeroes.
        }
      }
      inode->data.length = newsz;
      bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
    }
    return success;
  }
  size_t idx = old_sectors;
  size_t cnt = 0;
  for (; cnt < sectors; cnt++, idx++) {
    if (idx >= MAX_DIRECT_SECTOR) break;
    success = free_map_allocate(1, &(inode->data.direct[idx]));
    if (success)
      bwrite(fs_device, inode->data.direct[idx], 0, BLOCK_SECTOR_SIZE, (uint8_t *)zeros);
    if (!success) break;
  }
  if (!success) return false;
  if (cnt >= sectors) {
    inode->data.length = newsz;
    bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
    return true;
  }

  while (cnt < sectors) {
    size_t indirect_idx = (idx - MAX_DIRECT_SECTOR) / MAX_INDIRECT_SECTOR;
    size_t indirect_off = (idx - MAX_DIRECT_SECTOR) % MAX_INDIRECT_SECTOR;
    
    if (indirect_off == 0) {
      success = free_map_allocate(1, &inode->data.indirect[indirect_idx]);
      if (!success) return false;
    }
    
    struct buf *b = bread(fs_device, inode->data.indirect[indirect_idx]);
    block_sector_t *data = (block_sector_t *)&b->data;
    for (; cnt < sectors && indirect_off < MAX_INDIRECT_SECTOR; indirect_off++, cnt++, idx++) {
      success = free_map_allocate(1, data + indirect_off);
      if (success)
        bwrite(fs_device, data[indirect_off], 0, BLOCK_SECTOR_SIZE, (uint8_t *)zeros);
      if (!success) break;
    }
    bwrite(fs_device, inode->data.indirect[indirect_idx], 0, BLOCK_SECTOR_SIZE, (uint8_t *)data);
    bunpin(b);
    if (!success) return false;
  }

  inode->data.length = newsz;
  bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
  return true;
}
bool inode_resize(struct inode *inode, off_t addsz) {
  off_t sz = inode_length(inode);
  size_t old_sectors = bytes_to_sectors(sz);
  off_t newsz = sz + addsz;
  off_t alloc_size = newsz - BLOCK_SECTOR_SIZE * old_sectors;
  size_t sectors = bytes_to_sectors(alloc_size);
  if (sectors == 0) {
    inode->data.length = newsz;
    bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&(inode->data));
    return true;
  }
  return inode_resize_helper(newsz, old_sectors, sectors, inode);
}

bool is_open_inode(block_sector_t sector) { 
  struct list_elem *e;
  struct inode *inode;
  lock_acquire(&open_inodes_lock);
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      lock_release(&open_inodes_lock);
      return true;
    }
  }
  lock_release(&open_inodes_lock);

  return false;
}
