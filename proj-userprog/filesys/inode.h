#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

#define ENDING  0xffffffff

struct bitmap;
struct buf;

void inode_init(void);
bool inode_create(block_sector_t, off_t);
struct inode* inode_open(block_sector_t);
struct inode* inode_reopen(struct inode*);
block_sector_t inode_get_inumber(const struct inode*);
void inode_close(struct inode*);
void inode_remove(struct inode*);
off_t inode_read_at(struct inode*, void*, off_t size, off_t offset);
off_t inode_write_at(struct inode*, const void*, off_t size, off_t offset);
void inode_deny_write(struct inode*);
void inode_allow_write(struct inode*);
off_t inode_length(const struct inode*);

void  binit(void);
struct buf *bread(struct block *, block_sector_t);
void  bwrite(struct block *, block_sector_t, off_t, off_t, const uint8_t *);
void  bflush(void);

bool inode_resize(struct inode *, off_t);
bool  inode_isdir(struct inode *);
void inode_set_dir(struct inode *, bool);
#endif /* filesys/inode.h */
