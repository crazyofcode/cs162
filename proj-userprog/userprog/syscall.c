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
  if (!pagedir_get_page(thread_current()->pcb->pagedir, ptr+size*argu_size))  return false;
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
  return filesys_create(file, initial_size);
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
static tid_t sys_thread_create(stub_fun sfun, pthread_fun pfun, void *arg) {
  return pthread_execute(sfun, pfun, arg);
}
static void sys_pthread_exit(void) {
  struct thread *t = thread_current();
  pthread_exit();
  if (is_main_thread(t, t->pcb)) {
    if (t->tid == -1)
      thread_exit();
    t->pcb->exit_state = 0;
    process_exit();
  }
  NOT_REACHED();
}
static struct lock *find_pthread_lock(struct list *list, lock_t *id) {
  struct list_elem *e;
  for (e = list_begin(list); e != list_end(list); e = list_next(e)) {
    struct user_lock *ulock = list_entry(e, struct user_lock, elem);
    if (ulock->id == *id)
      return &ulock->lock;
  }
  return NULL;
}
static struct semaphore *find_pthread_sema(struct list *list, sema_t *id) {
  struct list_elem *e;
  for (e = list_begin(list); e != list_end(list); e = list_next(e)) {
    struct user_sema *usema = list_entry(e, struct user_sema, elem);
    if (usema->id == *id)
      return &usema->sema;
  }
  return NULL;
}
static bool sys_lock_init(lock_t *lock) {
  if (lock == NULL)
    return false;
  struct user_lock *ulock= malloc (sizeof (struct user_lock));
  if (ulock == NULL)
    return false;
  lock_init(&ulock->lock);
  struct process *pcb = thread_current()->pcb;
  lock_acquire(&pcb->user_thread_lock);
  lock_t id = list_entry(list_begin(&pcb->lock_list), struct user_lock, elem)->id;
  ulock->id = id + 1;
  list_push_front(&pcb->lock_list, &ulock->elem);
  lock_release(&pcb->user_thread_lock);
  *lock = ulock->id;
  return true;
}
static tid_t sys_get_tid(void) {
  return thread_current()->tid;
}
static bool sys_lock_acquire(lock_t *lock) {
  bool success = false;
  struct process *pcb = thread_current()->pcb;
  lock_acquire(&pcb->user_thread_lock);
  struct lock *klock = find_pthread_lock(&pcb->lock_list, lock);
  lock_release(&pcb->user_thread_lock);
  if (klock != NULL && !lock_held_by_current_thread(klock)) {
    success = true;
    lock_acquire(klock);
  }
  return success;
}
static bool sys_lock_release(lock_t *lock) {
  bool success = false;
  struct process *pcb = thread_current()->pcb;
  lock_acquire(&pcb->user_thread_lock);
  struct lock *klock = find_pthread_lock(&pcb->lock_list, lock);
  lock_release(&pcb->user_thread_lock);
  if (klock != NULL && lock_held_by_current_thread(klock)) {
    lock_release(klock);
    success = true;
  }
  return success;
}
static bool sys_sema_init(sema_t *sema, int val) {
  if (sema == NULL || val < 0)
    return false;
  struct process *pcb = thread_current()->pcb;

  struct user_sema *usema = malloc( sizeof (struct user_sema));
  if (usema == NULL)
    return false;

  sema_init(&usema->sema, val);
  lock_acquire(&pcb->user_thread_lock);
  sema_t id = list_entry(list_begin(&pcb->sema_list), struct user_sema, elem)->id;
  usema->id = id + 1;
  list_push_front(&pcb->sema_list, &usema->elem);
  lock_release(&pcb->user_thread_lock);
  *sema = usema->id;
  return true;
}
static bool sys_sema_down(sema_t *sema) {
  struct process *pcb = thread_current()->pcb;
  bool success = false;

  lock_acquire(&pcb->user_thread_lock);
  struct semaphore *ksema = find_pthread_sema(&pcb->sema_list, sema);
  lock_release(&pcb->user_thread_lock);
  if (ksema != NULL) {
    success = true;
    sema_down(ksema);
  }

  return success;
}
static bool sys_sema_up(sema_t *sema) {
  struct process *pcb = thread_current()->pcb;
  bool success = false;

  lock_acquire(&pcb->user_thread_lock);
  struct semaphore *ksema = find_pthread_sema(&pcb->sema_list, sema);
  lock_release(&pcb->user_thread_lock);
  if (ksema != NULL) {
    sema_up(ksema);
    success = true;
  }

  return success;
}

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);
  switch (args[0]) {
    case SYS_EXIT:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      exit(args[1]);
      return;
    case SYS_WRITE:
      if (!check_ptr((void *)&args[1], 3)) goto bad;
      f->eax = write(args[1], (void *)args[2], (unsigned)args[3]);
      return;
    case SYS_PRACTICE:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = args[1] + 1;
      return;
    case SYS_HALT:
      halt();
      return;
    case SYS_EXEC:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = exec((char *)args[1]);
      return;
    case SYS_CREATE:
      if (!check_ptr((void *)&args[1], 2)) goto bad;
      f->eax = args[1];
      f->eax = create((char *)args[1], (unsigned)args[2]);
      return;
    case SYS_REMOVE:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = remove((char *)args[1]);
      return;
    case SYS_OPEN:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = open((char *)args[1]);
      return;
    case SYS_FILESIZE:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = filesize((int)args[1]);
      return;
    case SYS_READ:
      if (!check_ptr((void *)&args[1], 3)) goto bad;
      f->eax = args[1];
      f->eax = read(args[1], (void *)args[2], (unsigned)args[3]);
      return;
    case SYS_SEEK:
      if (!check_ptr((void *)&args[1], 2)) goto bad;
      f->eax = args[1];
      seek(args[1], (unsigned)args[2]);
      return;
    case SYS_TELL:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = tell(args[1]);
      return;
    case SYS_CLOSE:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      close(args[1]);
      return;
    case SYS_COMPUTE_E:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = (int)compute_e(args[1]);
      return;
    case SYS_WAIT:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = args[1];
      f->eax = wait(args[1]);
      return;
    case SYS_LOCK_INIT:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      else f->eax = sys_lock_init((lock_t *)args[1]);
      return;
    case SYS_LOCK_ACQUIRE:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      else f->eax = sys_lock_acquire((lock_t *)args[1]);
      return;
    case SYS_LOCK_RELEASE:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      else f->eax = sys_lock_release((lock_t *)args[1]);
      return;
    case SYS_SEMA_INIT:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      else if (!check_ptr((void *)&args[2], 1)) goto bad;
      else f->eax = sys_sema_init((sema_t *)args[1], (int)args[2]);
      return;
    case SYS_SEMA_DOWN:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      else f->eax = sys_sema_down((sema_t *)args[1]);
      return;
    case SYS_SEMA_UP:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      else f->eax = sys_sema_up((sema_t *)args[1]);
      return;
    case SYS_GET_TID:
      f->eax = sys_get_tid();
      return;
    case SYS_PT_CREATE:
      if (!check_ptr((void *)&args[3], 1)) f->eax = TID_ERROR;
      else if (!check_ptr((void *)&args[2], 1)) f->eax = TID_ERROR;
      else if (!check_ptr((void *)&args[1], 1)) f->eax = TID_ERROR;
      else f->eax = sys_thread_create((stub_fun)args[1], (pthread_fun)args[2], (void *)args[3]);
      return;
    case SYS_PT_EXIT:
      sys_pthread_exit();
      return;
    case SYS_PT_JOIN:
      if (!check_ptr((void *)&args[1], 1)) goto bad;
      f->eax = pthread_join((tid_t)args[1]);
      return;
  }
bad:
  exit(-1);
  return;
}
