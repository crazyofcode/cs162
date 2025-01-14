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
#define MAX_DIRECT_SECTOR 120

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct[120];
  block_sector_t indirect[6]; /* First data sector. */
  off_t length;         /* File size in bytes. */
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
  bool is_dir;
  struct inode_disk data; /* Inode content. */
};

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
      b->cnt--;
      return ret;
    }
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct list bcache_list;

/* Initializes the inode module. */
void inode_init(void) { 
  list_init(&open_inodes);
}

void binit(void) {
  struct buf *b = NULL;
  list_init(&bcache_list);
  for (int i = 0; i < 64; i++) {
    b = malloc( sizeof (struct buf) );
    b->cnt = 0;
    b->dirty = false;
    list_push_front(&bcache_list, &b->elem);
  }
}

struct buf *bread(struct block *device, block_sector_t idx) {
  struct buf *b = NULL;
  struct list_elem *e;
  for (e = list_begin(&bcache_list); e != list_end(&bcache_list); e = list_next(e)) {
    b = list_entry(e, struct buf, elem);
    if (b->dev == device && b->blockno == idx) {
      b->cnt++;
      list_remove(&b->elem);
      list_push_front(&bcache_list, &b->elem);
      return b;
    }
  }

  e = list_back(&bcache_list);
  b = list_entry(e, struct buf, elem);
  if (b->dirty) {
    block_write(b->dev, b->blockno, b->data);
    b->dirty = false;
  }
  b->dev = device;
  b->blockno = idx;
  b->cnt = 1;
  list_remove(&b->elem);
  list_push_front(&bcache_list, &b->elem);

  block_read(device, idx, b->data);
  return b;
}

void bwrite(struct block *device, block_sector_t idx, off_t off, off_t length, const uint8_t *data) {
  struct buf *b = NULL;
  struct list_elem *e;
  for (e = list_begin(&bcache_list); e != list_end(&bcache_list); e = list_next(e)) {
    b = list_entry(e, struct buf, elem);
    if (b->dev == device && b->blockno == idx) {
      list_remove(&b->elem);
      list_push_front(&bcache_list, &b->elem);
      b->dirty = true;
      memcpy(b->data + off, data, length);
      return;
    }
  }

  b = bread(device, idx);
  b->dirty = true;
  memcpy((void *)&b->data[off], data, length);
}

void bflush(void) {
  struct buf *b = NULL;
  struct list_elem *e;
  for (e = list_begin(&bcache_list); e != list_end(&bcache_list); e = list_next(e)) { b = list_entry(e, struct buf, elem);
    if (b->dirty) {
      b->dirty = false;
      block_write(b->dev, b->blockno, b->data);
    }
  }
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
      bwrite(fs_device, disk_inode->indirect[i], 0, BLOCK_SECTOR_SIZE, (const uint8_t *)data);
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
    b->cnt--;
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
bool inode_create(block_sector_t sector, off_t length) {
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
    if (multi_free_map_allocate(sectors, disk_inode)) {
      // block_write(fs_device, sector, disk_inode);
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
            b->cnt--;
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

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  // block_read(fs_device, inode->sector, &inode->data);
  struct buf *b = bread(fs_device, inode->sector);
  memcpy(&inode->data, b->data, BLOCK_SECTOR_SIZE);
  b->cnt--;
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    inode->open_cnt++;
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

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      size_t sectors = bytes_to_sectors(inode->data.length);
      multi_free_map_release(sectors, &inode->data);
    }

    free(inode);
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
    //   b->cnt--;
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
    b->cnt--;
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
  // uint8_t* bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

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

    // if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
    //   /* Write full sector directly to disk. */
    //   block_write(fs_device, sector_idx, buffer + bytes_written);
    // } else {
    //   /* We need a bounce buffer. */
    //   if (bounce == NULL) {
    //     bounce = malloc(BLOCK_SECTOR_SIZE);
    //     if (bounce == NULL)
    //       break;
    //   }
    //
    //   /* If the sector contains data before or after the chunk
    //          we're writing, then we need to read in the sector
    //          first.  Otherwise we start with a sector of all zeros. */
    //   if (sector_ofs > 0 || chunk_size < sector_left)
    //     block_read(fs_device, sector_idx, bounce);
    //   else
    //     memset(bounce, 0, BLOCK_SECTOR_SIZE);
    //   memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
    //   block_write(fs_device, sector_idx, bounce);
    // }
    bwrite(fs_device, sector_idx, sector_ofs, chunk_size, buffer + bytes_written);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  // free(bounce);

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
  b->cnt--;
  return data.length;
}

bool inode_isdir(struct inode *inode) {
  return inode->is_dir;
}

static bool inode_resize_helper(off_t newsz, size_t old_sectors, size_t sectors, struct inode *inode) {
  bool success;
  if (old_sectors == 0) {
    success = multi_free_map_allocate(sectors, &inode->data);
    if (success) {
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
  }
  if (cnt >= sectors) {
    if (success) {
      inode->data.length = newsz;
      bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
    }
    return success;
  }

  size_t indirect_idx = (old_sectors+cnt-MAX_DIRECT_SECTOR) / MAX_INDIRECT_SECTOR;
  size_t indirect_off = (old_sectors+cnt-MAX_DIRECT_SECTOR) % MAX_INDIRECT_SECTOR;
  struct buf *b = bread(fs_device, inode->data.indirect[indirect_idx]);
  block_sector_t *data = (block_sector_t *)&b->data;
  for (; cnt < sectors && indirect_off < MAX_INDIRECT_SECTOR; indirect_off++, cnt++) {
    success = free_map_allocate(1, data + indirect_off);
    if (!success) break;
  }
  if (success)
    bwrite(fs_device, inode->data.indirect[indirect_idx], 0, BLOCK_SECTOR_SIZE, (uint8_t *)data);
  else
    return success;
  b->cnt--;
  for (indirect_idx=indirect_idx+1; indirect_idx < 6 && cnt < sectors; indirect_idx++) {
    block_sector_t tdata[MAX_INDIRECT_SECTOR];
    success = free_map_allocate(1, &(inode->data.indirect[indirect_idx]));
    if (!success) break;
    for (size_t i = 0; i < MAX_INDIRECT_SECTOR && cnt < sectors; i++, cnt++) {
      success = free_map_allocate(1, &tdata[i]);
      if (!success) break;
    }
    if (success)
      bwrite(fs_device, inode->data.indirect[indirect_idx], 0, BLOCK_SECTOR_SIZE, (uint8_t *)tdata);
    else
      break;
  }
  if (success) {
    inode->data.length = newsz;
    bwrite(fs_device, inode->sector, 0, BLOCK_SECTOR_SIZE, (uint8_t *)&inode->data);
  }
  return success;
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

void inode_set_dir(struct inode *inode, bool is_dir) {
  inode->is_dir = is_dir;
}

bool is_open_inode(block_sector_t sector) { struct list_elem *e;
  struct inode *inode;
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector)
      return true;
  }

  return false;
}
