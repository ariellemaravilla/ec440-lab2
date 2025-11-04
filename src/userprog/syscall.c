#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include "lib/kernel/stdio.h"

/* Process identifier type. */
typedef int pid_t;

//lock to proctect all filesys
static struct lock filesys_lock; 

static void syscall_handler (struct intr_frame *);

//forward declarations for user mem helpers
static void validate_uaddr (const void *uaddr); 
static void copy_in (void *dst, const void *usrc, size_t size);
static void copy_out (void *udst, const void *src, size_t size);
static uint32_t get_u32 (const void *uaddr);
static bool get_string (const char *usrc, char *dst, size_t size);

//exit helper to call from anywhere in fault
static void sys_exit (int status);

//sys call implementations
static void sys_halt (void);
static void sys_exit_handler (int status);
static pid_t sys_exec (const char *cmd_line);
static int sys_wait (pid_t pid);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);

//file descriptor helpers
static int allocate_fd (struct file *file);
static struct file *get_file (int fd);
static void close_fd (int fd);
static void close_all_fds (void); 

//initialization
void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// main handler
static void
syscall_handler (struct intr_frame *f) 
{
  // f->esp points to user stack. 
  validate_uaddr (f->esp);
  uint32_t sysno = get_u32 (f->esp);

  switch (sysno) {
    case SYS_HALT:
      sys_halt();
      break;
      
    case SYS_EXIT: {
      int status = (int)get_u32((uint8_t*)f->esp + 4);
      sys_exit_handler(status);
      break; /* not reached */
    }
    
    case SYS_EXEC: {
      const char *cmd_line = (const char *)get_u32((uint8_t*)f->esp + 4);
      char cmd_line_copy[1024];
      if (!get_string(cmd_line, cmd_line_copy, sizeof cmd_line_copy)) {
        f->eax = -1;
        break;
      }
      f->eax = sys_exec(cmd_line_copy);
      break;
    }
    
    case SYS_WAIT: {
      pid_t pid = (pid_t)get_u32((uint8_t*)f->esp + 4);
      f->eax = sys_wait(pid);
      break;
    }
    
    case SYS_CREATE: {
      const char *file = (const char *)get_u32((uint8_t*)f->esp + 4);
      unsigned initial_size = get_u32((uint8_t*)f->esp + 8);
      char file_copy[256];
      if (!get_string(file, file_copy, sizeof file_copy)) {
        f->eax = false;
        break;
      }
      f->eax = sys_create(file_copy, initial_size);
      break;
    }
    
    case SYS_REMOVE: {
      const char *file = (const char *)get_u32((uint8_t*)f->esp + 4);
      char file_copy[256];
      if (!get_string(file, file_copy, sizeof file_copy)) {
        f->eax = false;
        break;
      }
      f->eax = sys_remove(file_copy);
      break;
    }
    
    case SYS_OPEN: {
      const char *file = (const char *)get_u32((uint8_t*)f->esp + 4);
      char file_copy[256];
      if (!get_string(file, file_copy, sizeof file_copy)) {
        f->eax = -1;
        break;
      }
      f->eax = sys_open(file_copy);
      break;
    }
    
    case SYS_FILESIZE: {
      int fd = (int)get_u32((uint8_t*)f->esp + 4);
      f->eax = sys_filesize(fd);
      break;
    }
    
    case SYS_READ: {
      int fd = (int)get_u32((uint8_t*)f->esp + 4);
      void *buffer = (void *)get_u32((uint8_t*)f->esp + 8);
      unsigned size = get_u32((uint8_t*)f->esp + 12);
      f->eax = sys_read(fd, buffer, size);
      break;
    }
    
    case SYS_WRITE: {
      int fd = (int)get_u32((uint8_t*)f->esp + 4);
      const void *buffer = (const void *)get_u32((uint8_t*)f->esp + 8);
      unsigned size = get_u32((uint8_t*)f->esp + 12);
      f->eax = sys_write(fd, buffer, size);
      break;
    }
    
    case SYS_SEEK: {
      int fd = (int)get_u32((uint8_t*)f->esp + 4);
      unsigned position = get_u32((uint8_t*)f->esp + 8);
      sys_seek(fd, position);
      break;
    }
    
    case SYS_TELL: {
      int fd = (int)get_u32((uint8_t*)f->esp + 4);
      f->eax = sys_tell(fd);
      break;
    }
    
    case SYS_CLOSE: {
      int fd = (int)get_u32((uint8_t*)f->esp + 4);
      sys_close(fd);
      break;
    }

    default:
      //  syscall unknown process kill
      sys_exit(-1);
      break;
  }
}

//helpers 
// validate a single user address is in user space
static void
validate_uaddr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr(uaddr) ||
      pagedir_get_page(thread_current()->pagedir, uaddr) == NULL) {
    sys_exit(-1);
  }
}

// validate a range of user addresses
static void
validate_uaddr_range (const void *uaddr, size_t size)
{
  const uint8_t *start = (const uint8_t *)uaddr;
  const uint8_t *end = start + size;
  for (const uint8_t *p = start; p < end; p++) {
    validate_uaddr(p);
  }
}

// safe copy from user → kernel
static void
copy_in (void *dst, const void *usrc, size_t size)
{
  uint8_t *d = dst;
  const uint8_t *s = usrc;
  for (size_t i = 0; i < size; i++) {
    validate_uaddr(s);
    d[i] = *s++;
  }
}

// safe copy from kernel → user
static void
copy_out (void *udst, const void *src, size_t size)
{
  uint8_t *d = udst;
  const uint8_t *s = src;
  for (size_t i = 0; i < size; i++) {
    validate_uaddr(d);
    *d++ = s[i];
  }
}

// get a 32-bit value from user memory
static uint32_t
get_u32 (const void *uaddr)
{
  uint32_t val = 0;
  copy_in(&val, uaddr, sizeof val);
  return val;
}

// get a null-terminated string from user memory
static bool
get_string (const char *usrc, char *dst, size_t size)
{
  if (usrc == NULL || size == 0) {
    return false;
  }
  
  for (size_t i = 0; i < size - 1; i++) {
    validate_uaddr(usrc + i);
    dst[i] = usrc[i];
    if (usrc[i] == '\0') {
      return true;
    }
  }
  
  // if string too long
  return false;
}

// exit(status)
static void
sys_exit (int status)
{
  struct thread *t = thread_current();
  t->exit_status = status;  
  thread_exit();
}

// exit(status) - system call handler
static void
sys_exit_handler (int status)
{
  struct thread *t = thread_current();
  t->exit_status = status;
  // process_exit handle cleanup
  thread_exit();
}

// halt system call
static void
sys_halt (void)
{
  shutdown_power_off();
}

// exec system call helper
struct find_thread_aux {
  tid_t tid;
  struct thread *thread;
};

static void find_thread_func(struct thread *t, void *aux_) {
  struct find_thread_aux *aux = aux_;
  if (t->tid == aux->tid) {
    aux->thread = t;
  }
}

// exec system call
static pid_t
sys_exec (const char *cmd_line)
{
  tid_t tid = process_execute(cmd_line);
  
  if (tid == TID_ERROR) {
    return -1;
  }
  
  // find child thread
  struct find_thread_aux aux;
  aux.tid = tid;
  aux.thread = NULL;
  
  enum intr_level old_level = intr_disable();
  thread_foreach(find_thread_func, &aux);
  intr_set_level(old_level);
  
  if (aux.thread == NULL) {
    return -1;
  }
  
  // wait for child 
  sema_down(&aux.thread->exec_sema);
  
  if (!aux.thread->load_success) {
    return -1;
  }
  
  return tid;
}

// wait system call
static int
sys_wait (pid_t pid)
{
  return process_wait(pid);
}

// create system call
static bool
sys_create (const char *file, unsigned initial_size)
{
  lock_acquire(&filesys_lock);
  bool success = filesys_create(file, initial_size);
  lock_release(&filesys_lock);
  return success;
}

// remove system call
static bool
sys_remove (const char *file)
{
  lock_acquire(&filesys_lock);
  bool success = filesys_remove(file);
  lock_release(&filesys_lock);
  return success;
}

// open system call
static int
sys_open (const char *file)
{
  lock_acquire(&filesys_lock);
  struct file *f = filesys_open(file);
  lock_release(&filesys_lock);
  
  if (f == NULL) {
    return -1;
  }
  
  return allocate_fd(f);
}

// filesize system call
static int
sys_filesize (int fd)
{
  struct file *f = get_file(fd);
  if (f == NULL) {
    return -1;
  }
  
  lock_acquire(&filesys_lock);
  int size = file_length(f);
  lock_release(&filesys_lock);
  return size;
}

// read system call
static int
sys_read (int fd, void *buffer, unsigned size)
{
  if (buffer == NULL) {
    return -1;
  }
  
  validate_uaddr_range(buffer, size);
  
  if (fd == 0) {
    // read from key stdin
    uint8_t key_buf[256];  // read into kernel buffer first
    unsigned bytes_to_read = size < 256 ? size : 256;
    unsigned bytes_read = 0;
    for (unsigned i = 0; i < bytes_to_read; i++) {
      key_buf[i] = input_getc();
      bytes_read++;
    }
    // copy to user buffer
    copy_out(buffer, key_buf, bytes_read);
    return bytes_read;
  }
  
  struct file *f = get_file(fd);
  if (f == NULL) {
    return -1;
  }
  
  // read into kernel buffer first, then copy to user memory
  // use a buffer size , read in chunks if needed
  unsigned total_read = 0;
  unsigned remaining = size;
  uint8_t *user_buf = (uint8_t *)buffer;
  
  while (remaining > 0) {
    unsigned chunk_size = remaining < 4096 ? remaining : 4096;
    uint8_t *kernel_buf = malloc(chunk_size);
    if (kernel_buf == NULL) {
      return total_read > 0 ? total_read : -1;
    }
    
    lock_acquire(&filesys_lock);
    int bytes_read = file_read(f, kernel_buf, chunk_size);
    lock_release(&filesys_lock);
    
    if (bytes_read <= 0) {
      free(kernel_buf);
      break;
    }
    
    copy_out(user_buf, kernel_buf, bytes_read);
    free(kernel_buf);
    
    total_read += bytes_read;
    user_buf += bytes_read;
    remaining -= bytes_read;
    
    if ((unsigned)bytes_read < chunk_size) {
      // end file
      break;
    }
  }
  
  return total_read;
}

// write system call
static int
sys_write (int fd, const void *buffer, unsigned size)
{
  if (buffer == NULL) {
    return -1;
  }
  
  validate_uaddr_range(buffer, size);
  
  if (fd == 1) {
    // write to console, stdout
    putbuf((const char *)buffer, size);
    return size;
  }
  
  struct file *f = get_file(fd);
  if (f == NULL) {
    return -1;
  }
  
  // copy from user buffer to kernel buffer first
  // write in chunks if needed
  unsigned total_written = 0;
  unsigned remaining = size;
  const uint8_t *user_buf = (const uint8_t *)buffer;
  
  while (remaining > 0) {
    unsigned chunk_size = remaining < 4096 ? remaining : 4096;
    uint8_t *kernel_buf = malloc(chunk_size);
    if (kernel_buf == NULL) {
      return total_written > 0 ? total_written : -1;
    }
    
    copy_in(kernel_buf, user_buf, chunk_size);
    
    lock_acquire(&filesys_lock);
    int bytes_written = file_write(f, kernel_buf, chunk_size);
    lock_release(&filesys_lock);
    
    free(kernel_buf);
    
    if (bytes_written <= 0) {
      break;
    }
    
    total_written += bytes_written;
    user_buf += bytes_written;
    remaining -= bytes_written;
    
    if ((unsigned)bytes_written < chunk_size) {
      // no more write
      break;
    }
  }
  
  return total_written;
}

// seek system call
static void
sys_seek (int fd, unsigned position)
{
  struct file *f = get_file(fd);
  if (f == NULL) { 
    return;
  }
  
  lock_acquire(&filesys_lock);
  file_seek(f, position);
  lock_release(&filesys_lock);
}

// tell system call
static unsigned
sys_tell (int fd)
{
  struct file *f = get_file(fd);
  if (f == NULL) {
    return -1;
  }
  
  lock_acquire(&filesys_lock);
  unsigned position = file_tell(f);
  lock_release(&filesys_lock);
  return position;
}

// close system call
static void
sys_close (int fd)
{
  close_fd(fd);
}

// file descriptor helpers
static int
allocate_fd (struct file *file)
{
  struct thread *t = thread_current();
  for (int i = 2; i < 128; i++) {
    if (t->fd_table[i] == NULL) {
      t->fd_table[i] = file;
      return i;
    }
  }
  return -1;
}

static struct file *
get_file (int fd)
{
  struct thread *t = thread_current();
  if (fd < 0 || fd >= 128) {
    return NULL;
  }
  return t->fd_table[fd];
}

static void
close_fd (int fd)
{
  struct file *f = get_file(fd);
  if (f == NULL) {
    return;
  }
  
  struct thread *t = thread_current();
  t->fd_table[fd] = NULL;
  
  lock_acquire(&filesys_lock);
  file_close(f);
  lock_release(&filesys_lock);
}

static void
close_all_fds (void)
{
  struct thread *t = thread_current();
  for (int i = 0; i < 128; i++) {
    if (t->fd_table[i] != NULL) {
      close_fd(i);
    }
  }
}
