#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"

static void syscall_handler(struct intr_frame*);

#define argu_size sizeof(void *)
static bool check_ptr(void *ptr, size_t size) {
  if (ptr == NULL)  return false;
  if (!is_user_vaddr(ptr))                  return false;
  if (!is_user_vaddr(ptr+size*argu_size))   return false;
  if (!pagedir_get_page(thread_current()->pcb->pagedir, ptr))  return false;
  if (!pagedir_get_page(thread_current()->pcb->pagedir, (void *)((char *)ptr+size*argu_size)))  return false;
  return true;
}
static bool check_str(void *ptr) {
  if (ptr == NULL) return false;

  // 初始化当前地址为 ptr
  char *current_ptr = (char *)ptr;
  
  // 遍历指针，直到遇到 '\0'，同时检查指针的有效性
  while (true) {
    if (!is_user_vaddr(current_ptr)) return false;
    if (!pagedir_get_page(thread_current()->pcb->pagedir, current_ptr)) {
      return false; // 如果当前地址不可访问
    }
    if (*current_ptr == '\0') {
      break; // 找到字符串的结束符
    }
    current_ptr++; // 继续检查下一个字节
  }

  return true;
}

static struct file_entry *find(int fd) {
  struct thread *t = thread_current();
  struct process *p = t->pcb;
  struct list_elem *e;
  for (e = list_begin(&p->file); e != list_end(&p->file); e = list_next(e)) {
    struct file_entry *entry = list_entry(e, struct file_entry, elem);
    if (entry->fd == fd)
      return entry;
  }
  return NULL;
}

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static void exit(int state) {
  thread_current()->pcb->exit_state = state;
  process_exit();
}
static int wait(pid_t pid) {
  return process_wait(pid);
}
static int write(int fd, void *buffer, unsigned size) {
  if (!check_str(buffer)) {
    exit(-1);
    return -1;
  }
  if (fd == 1) {
    putbuf(buffer, size);
    return size;
  } else {
    struct file_entry *entry = find(fd);
    if (entry == NULL)  return -1;
    lock_acquire(&thread_current()->pcb->file_lock);
    int32_t ret = file_write(entry->file, buffer, size);
    lock_release(&thread_current()->pcb->file_lock);
    return ret;
  }
}
static void halt(void) {
  shutdown_power_off();
}
static bool create(const char *file, unsigned initial_size) {
  if (!check_str((void *)file)) {
    exit(-1);
    return -1;
  }
  return filesys_create(file, initial_size, false);
}
static bool remove(const char *file) {
  return filesys_remove(file);
}
static int open(const char *file) {
  // if (strcmp(file, "std"))
  if (!check_str((void *)file)) {
    exit(-1);
    return -1;
  }
  struct thread* t = thread_current();
  lock_acquire(&t->pcb->file_lock);
  struct file *f = filesys_open(file);
  lock_release(&t->pcb->file_lock);
  if (f == NULL) return -1;
  struct file_entry *entry = malloc(sizeof(struct file_entry));
  entry->file = f;
  entry->fd = t->pcb->fd++;
  list_push_back(&t->pcb->file, &entry->elem);

  return entry->fd;
}

static int filesize(int fd) {
  struct file_entry *entry = find(fd);
  if (entry == NULL) return -1;
  return file_length(entry->file);
}
static int read(int fd, void *buffer, unsigned size) {
  if (!check_str(buffer)) {
    exit(-1);
    return -1;
  }
  char *str = (char *)buffer;
  if (fd == STDIN_FILENO) {
    for(unsigned i = 0; i < size; i++) {
      str[i] = input_getc();
    }
    return size;
  } else if (fd == STDOUT_FILENO) {
    return -1;
  } else{
    struct file_entry *entry = find(fd);
    if (entry == NULL)  return -1;
    lock_acquire(&thread_current()->pcb->file_lock);
    unsigned ret = file_read(entry->file, buffer, size);
    lock_release(&thread_current()->pcb->file_lock);
    return ret;
  }
}
static void seek(int fd, unsigned position) {
  struct file_entry *entry = find(fd);
  if (entry == NULL)  return;
  lock_acquire(&thread_current()->pcb->file_lock);
  file_seek(entry->file, position);
  lock_release(&thread_current()->pcb->file_lock);
}
static int tell(int fd) {
  struct file_entry *entry = find(fd);
  if (entry == NULL)  return -1;
  lock_acquire(&thread_current()->pcb->file_lock);
  int32_t ret = file_tell(entry->file);
  lock_release(&thread_current()->pcb->file_lock);
  return ret;
}
static void close(int fd) {
  struct file_entry *entry = find(fd);
  if (entry == NULL)  return;
  lock_acquire(&thread_current()->pcb->file_lock);
  file_close(entry->file);
  lock_release(&thread_current()->pcb->file_lock);
  list_remove(&entry->elem);
  free(entry);
}
static pid_t exec(const char *cmd_line) {
  if (!check_str((void *)cmd_line)) {
    exit(-1);
    return -1;
  }
  return process_execute(cmd_line);
}
static int compute_e(int n) {
  return sys_sum_to_e(n);
}
static bool sys_chdir(const char *dir) {
  if (dir[0] == '\0')
    return false;
  struct thread *cur = thread_current();
  struct file *file = filesys_open(dir);
  if (file == NULL)   return false;
  cur->pcb->cwd = file_get_inode(file);
  return true;
}
static bool sys_mkdir(const char *dir) {
  if (dir[0] == '\0')
    return false;
  return filesys_create(dir, 0, true);
}
static bool sys_readdir(int fd, char *name) {
  struct file_entry *entry = find(fd);
  if (entry == NULL)  return -1;
  lock_acquire(&thread_current()->pcb->file_lock);
  bool ret = filesys_readdir(file_get_inode(entry->file), name);
  lock_release(&thread_current()->pcb->file_lock);
  return ret;
}
static bool sys_isdir(int fd) {
  struct file_entry *entry = find(fd);
  if (entry == NULL)  return -1;
  return filesys_isdir(file_get_inode(entry->file));
}
static int sys_inumber(int fd) {
  struct file_entry *entry = find(fd);
  if (entry == NULL)  return -1;
  return file_get_inumber(entry->file);
}

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */
  switch (args[0]) {
    case SYS_EXIT:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      exit(args[1]);
      return;
    case SYS_WRITE:
      if (!check_ptr((void *)&args[1], (size_t)args[3])) goto bad;
      f->eax = write(args[1], (void *)args[2], (unsigned)args[3]);
      return;
    case SYS_PRACTICE:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1] + 1;
      return;
    case SYS_HALT:
      halt();
      return;
    case SYS_EXEC:
      if (!check_str((void *)&args[1])) goto bad;
      f->eax = exec((char *)args[1]);
      return;
    case SYS_CREATE:
      if (!check_ptr((void *)&args[1], (size_t)args[2])) goto bad;
      f->eax = create((char *)args[1], (unsigned)args[2]);
      return;
    case SYS_REMOVE:
      if (!check_str((void *)&args[1])) goto bad;
      f->eax = remove((char *)args[1]);
      return;
    case SYS_OPEN:
      if (!check_str((void *)&args[1])) goto bad;
      f->eax = open((char *)args[1]);
      return;
    case SYS_FILESIZE:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      f->eax = filesize((int)args[1]);
      return;
    case SYS_READ:
      if (!check_ptr((void *)&args[1], (size_t)args[3])) goto bad;
      f->eax = read(args[1], (void *)args[2], (unsigned)args[3]);
      return;
    case SYS_SEEK:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      seek(args[1], (unsigned)args[2]);
      return;
    case SYS_TELL:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      f->eax = tell(args[1]);
      return;
    case SYS_CLOSE:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      close(args[1]);
      return;
    case SYS_COMPUTE_E:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      f->eax = (int)compute_e(args[1]);
      return;
    case SYS_WAIT:
      if (!check_ptr((void *)&args[1], 4)) goto bad;
      f->eax = args[1];
      f->eax = wait(args[1]);
      return;
    case SYS_CHDIR:
      if (!check_str((void *)&args[1])) goto bad;
      f->eax = sys_chdir((const char *)args[1]);
      return;
    case SYS_MKDIR:
      if (!check_str((void *)&args[1])) goto bad;
      f->eax = sys_mkdir((const char *)args[1]);
      return;
    case SYS_ISDIR:
      f->eax = sys_isdir(args[1]);
      return;
    case SYS_READDIR:
      if (!check_ptr((void *)&args[2], 4)) goto bad;
      f->eax = sys_readdir(args[1], (char *)args[2]);
      return;
    case SYS_INUMBER:
      f->eax = sys_inumber(args[1]);
      return;
  }
bad:
  exit(-1);
  return;
}
