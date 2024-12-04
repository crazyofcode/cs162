#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <stdint.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);
typedef char  lock_t;
typedef char  sema_t;

struct file_entry {
   struct file *file;
   int         fd;
   struct list_elem elem;
};
struct child_entry {
  pid_t             pid;
	struct list_elem  elem;
	bool              is_waiting;
  bool              alive;
  int               exit_state;
  struct semaphore  sema_wait;
  struct thread *   t;
};
/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */

	// process
	pid_t pid;
	struct list	child_list;
	// struct child_entry* child;
	int 	exit_state;
	struct semaphore	sema_exec;
	bool	load_success;
	// optional
	bool	killed;

	// file
  int fd;
  struct lock file_lock;
  struct list file;

  struct file *exec_file;

  // user thread
  struct list user_thread;
  struct lock user_thread_lock;
  struct semaphore wait_main;

  // user synth
  struct list lock_list;
  struct list sema_list;
};

struct pthread {
  tid_t       tid;
  uint8_t *   stack;
  bool        is_waiting;
  struct semaphore sema_wait;
  struct list_elem elem;
};

struct pthread_data {
  bool        success;
  pthread_fun tf;
  stub_fun    sf;
  void *      arg;
  struct pthread *pthread;
  struct semaphore sema;
  tid_t       tid;
};

struct user_lock {
  lock_t      id;
  struct lock lock;
  struct list_elem elem;
};

struct user_sema {
  sema_t           id;
  struct semaphore sema;
  struct list_elem elem;
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
