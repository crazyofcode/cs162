#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "devices/shutdown.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static int  practice(int i)  {return i+1;}
static void halt(void)      {shutdown_power_off();}
static int write(int fd, const void *buff, unsigned int size) {
  if (fd == 1) {
    putbuf((char *)buff, size);
    return size;
  } else {
    return -1;
  }
}
static pid_t exec(const char *cmd_line) {
  pid_t pid = process_execute(cmd_line);
  if (thread_current()->exit_status != -1)
    return pid;
  else return -1;
}
static int wait(pid_t pid) {
  return process_wait(pid);
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

  // if (args[0] == SYS_EXIT) {
  //   f->eax = args[1];
  //   printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
  //   process_exit();
  // } 
  switch(args[0]) {
    case SYS_EXIT:
      f->eax = args[1];
      // printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
      thread_current()->exit_status = args[1];
      process_exit();
      return;
    case SYS_PRACTICE:
      f->eax = practice(args[1]);
      return;
    case SYS_WRITE:
      f->eax = write(args[1], (void *)args[2], args[3]);
      return;
    case SYS_HALT:
      halt();
      return;
    case SYS_EXEC:
      f->eax = exec((char *)args[1]);
      return;
    case SYS_WAIT:
      f->eax = wait((pid_t)args[1]);
      return;
  }
}
