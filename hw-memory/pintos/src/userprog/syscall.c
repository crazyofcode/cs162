#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"

#define ROUND_UP(sz)    (((sz)+PGSIZE-1) & ~(PGSIZE-1))
static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

void syscall_exit(int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);
  thread_exit();
}

/*
 * This does not check that the buffer consists of only mapped pages; it merely
 * checks the buffer exists entirely below PHYS_BASE.
 */
static void validate_buffer_in_user_region(const void* buffer, size_t length) {
  uintptr_t delta = PHYS_BASE - buffer;
  if (!is_user_vaddr(buffer) || length > delta)
    syscall_exit(-1);
}

/*
 * This does not check that the string consists of only mapped pages; it merely
 * checks the string exists entirely below PHYS_BASE.
 */
static void validate_string_in_user_region(const char* string) {
  uintptr_t delta = PHYS_BASE - (const void*)string;
  if (!is_user_vaddr(string) || strnlen(string, delta) == delta)
    syscall_exit(-1);
}

static int syscall_open(const char* filename) {
  struct thread* t = thread_current();
  if (t->open_file != NULL)
    return -1;

  t->open_file = filesys_open(filename);
  if (t->open_file == NULL)
    return -1;

  return 2;
}

static int syscall_write(int fd, void* buffer, unsigned size) {
  struct thread* t = thread_current();
  if (fd == STDOUT_FILENO) {
    putbuf(buffer, size);
    return size;
  } else if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int)file_write(t->open_file, buffer, size);
}

static int syscall_read(int fd, void* buffer, unsigned size) {
  struct thread* t = thread_current();
  if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int)file_read(t->open_file, buffer, size);
}

static void syscall_close(int fd) {
  struct thread* t = thread_current();
  if (fd == 2 && t->open_file != NULL) {
    file_close(t->open_file);
    t->open_file = NULL;
  }
}

static void *incr_user_heap(intptr_t increment) {
  struct thread *cur = thread_current();
  uint32_t align_heap_end = ROUND_UP(cur->heap_end);

  intptr_t align_incr = 0;
  uint32_t last_page_free_space = align_heap_end - cur->heap_end;
  if (last_page_free_space >= (uint32_t)increment) {
    goto done;
  }
  else
    align_incr = ROUND_UP(increment - last_page_free_space);
  int npages = align_incr / PGSIZE;

  uint8_t *kpage = palloc_get_multiple(PAL_ZERO | PAL_USER, npages);
  if (kpage == NULL)
    return (void *)(-1);
  for (int i = 0; i < npages; i++) {
    pagedir_set_page(cur->pagedir, (void *)(align_heap_end + i * PGSIZE), kpage + i * PGSIZE, true);
  }

done:
  cur->heap_end += increment;
  return (void *)(cur->heap_end - increment);
}

static void *desc_user_heap(uintptr_t increment) {
  struct thread *cur = thread_current();
  intptr_t align_incr = ROUND_UP(increment);
  uint32_t align_heap_end = ROUND_UP(cur->heap_end - increment);
  int npages = align_incr / PGSIZE;
  for (int i = 0; i < npages; i++) {
    void *kpage = pagedir_get_page(cur->pagedir, (const void *)align_heap_end + i * PGSIZE);
    palloc_free_page(kpage);
    pagedir_clear_page(cur->pagedir, (void *)align_heap_end + i * PGSIZE);
  }
  cur->heap_end -= increment;
  return (void *)(cur->heap_end + increment);
}

static void *syscall_sbrk(intptr_t increment) {
  if (increment == 0)
    return (void *)thread_current()->heap_end;
  else if (increment > 0)
    return incr_user_heap(increment);
  else
    return desc_user_heap(-increment);
}

static void syscall_handler(struct intr_frame* f) {
  uint32_t* args = (uint32_t*)f->esp;
  struct thread* t = thread_current();
  t->in_syscall = true;

  validate_buffer_in_user_region(args, sizeof(uint32_t));
  switch (args[0]) {
    case SYS_EXIT:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      syscall_exit((int)args[1]);
      break;

    case SYS_OPEN:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      validate_string_in_user_region((char*)args[1]);
      f->eax = (uint32_t)syscall_open((char*)args[1]);
      break;

    case SYS_WRITE:
      validate_buffer_in_user_region(&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region((void*)args[2], (unsigned)args[3]);
      f->eax = (uint32_t)syscall_write((int)args[1], (void*)args[2], (unsigned)args[3]);
      break;

    case SYS_READ:
      validate_buffer_in_user_region(&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region((void*)args[2], (unsigned)args[3]);
      f->eax = (uint32_t)syscall_read((int)args[1], (void*)args[2], (unsigned)args[3]);
      break;

    case SYS_CLOSE:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      syscall_close((int)args[1]);
      break;

    case SYS_SBRK:
      validate_buffer_in_user_region(&args[1], sizeof(intptr_t));
      f->eax = (uint32_t)syscall_sbrk((intptr_t)args[1]);
      break;

    default:
      printf("Unimplemented system call: %d\n", (int)args[0]);
      break;
  }

  t->in_syscall = false;
}
