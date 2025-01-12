#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);

void filesys_binit(void) {
  binit();
}
/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  inode_init();
  free_map_init();

  if (format)
    do_format();

  free_map_open();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) { 
  bflush();
  free_map_close();
}

static bool parse_new_file_name(char *src, char *dst) {
  size_t length = strlen(src);
  size_t cnt = 0;
  char *p = src + length - 1;
  while (*p != '/' && p >= src) {
    ++cnt;
    --p;
  }
  if (cnt > NAME_MAX)
    return false;
  strlcpy(dst, p+1, cnt+1);
  *(p + 1) = '\0';
  return true;
}
/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char* name, off_t initial_size, bool is_dir) {
  struct dir *base_dir;
  struct dir *dir;
  struct dir *base = thread_current()->pcb->cwd;
  struct inode *inode;
  char fname[NAME_MAX+1];
  char tmp_name[strlen(name) + 1];
  block_sector_t inode_sector = 0;
  bool success = true;

  if (name[0] == '\0')
    return false;

  if (name[0] == '/' || base == NULL)
    base_dir = dir_open_root();
  else
    base_dir = dir_reopen(base);

  strlcpy(tmp_name, name, strlen(name)+1);
  success = parse_new_file_name(tmp_name, fname);
  if (!success) return success;

  if (!base_dir)
    return false;
  if (strlen(tmp_name) > 0 && strcmp(tmp_name, "/")) {
    dir_lookup(base_dir, tmp_name, &inode);
    dir = dir_open(inode);
  } else
    dir = dir_reopen(base_dir);

  if (success) {
    success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
                  inode_create(inode_sector, initial_size) && dir_add(dir, fname, inode_sector, is_dir));
  }
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
  dir_close(dir);
  dir_close(base_dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file* filesys_open(const char* name) {
  struct dir *base_dir;
  struct dir *base = thread_current()->pcb->cwd;
  struct inode* inode;

  if (strlen(name) <= 0)
    return NULL;
  if (strcmp(name, "/") == 0) {
    base_dir = dir_open_root();
    inode = dir_get_inode(base_dir);
    struct file *file = file_open(inode);
    dir_close(base_dir);
    return file;
  }
  if (name[0] == '/' || base == NULL)
    base_dir = dir_open_root();
  else
    base_dir = dir_reopen(base);
  if (base_dir)
    dir_lookup(base_dir, name, &inode);
  dir_close(base_dir);

  return file_open(inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char* name) {
  struct dir *base_dir;
  struct dir *base = thread_current()->pcb->cwd;
  struct dir *root = dir_open_root();
  bool success = true;

  if (name[0] == '\0')
    return false;

  if (name[0] == '/' || base == NULL)
    base_dir = root;
  else
    base_dir = dir_reopen(base);

  if (!base_dir)  return false;

  success = dir_remove(base_dir, name, base);
  dir_close(base_dir);

  return success;
}

struct dir *get_cwd_dir(struct dir *base) {
  if (base == NULL)
    return dir_open_root();
  else
    return dir_reopen(base);
}

bool filesys_readdir(struct inode *inode, char *dst) {
  struct dir *dir = dir_open(inode);
  if (dir == NULL)  return false;
  return dir_readdir(dir, dst);
}

bool filesys_isdir(struct inode *inode) {
  return inode_isdir(inode);
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(NULL,ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}
