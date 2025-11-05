#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/shutdown.h"
#include "devices/input.h"

static void syscall_handler (struct intr_frame *);
static void validate_pointer (const void *ptr);
static void validate_buffer (const void *buffer, size_t size);
static void validate_string (const char *str);
static int get_user (const uint8_t *uaddr);
static void syscall_exit (int status);

/* File system lock to ensure atomic file operations */
static struct lock filesys_lock;

/* System call implementations */
static void sys_halt (void);
static void sys_exit (int status);
static int sys_exec (const char *cmd_line);
static int sys_wait (int pid);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Validates that PTR is a valid user pointer */
static void
validate_pointer (const void *ptr)
{
  if (ptr == NULL || !is_user_vaddr (ptr) || 
      get_user ((const uint8_t *) ptr) == -1)
    {
      syscall_exit (-1);
    }
}

/* Validates that BUFFER of SIZE bytes is accessible */
static void
validate_buffer (const void *buffer, size_t size)
{
  size_t i;
  const uint8_t *buf = (const uint8_t *) buffer;
  
  for (i = 0; i < size; i++)
    {
      validate_pointer (buf + i);
    }
}

/* Validates that STR is a valid string */
static void
validate_string (const char *str)
{
  validate_pointer (str);
  while (get_user ((const uint8_t *) str) != 0 && 
         get_user ((const uint8_t *) str) != -1)
    str++;
  if (get_user ((const uint8_t *) str) == -1)
    syscall_exit (-1);
}

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t *esp = (uint32_t *) f->esp;
  
  validate_pointer (esp);
  validate_pointer ((uint8_t *) esp + 3);  /* Validate all 4 bytes of the int */
  
  int syscall_number = (int) *esp;
  
  switch (syscall_number)
    {
    case SYS_HALT:
      sys_halt ();
      break;
      
    case SYS_EXIT:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      sys_exit ((int) *(esp + 1));
      break;
      
    case SYS_EXEC:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_string ((const char *) *(esp + 1));
      f->eax = sys_exec ((const char *) *(esp + 1));
      break;
      
    case SYS_WAIT:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      f->eax = sys_wait ((int) *(esp + 1));
      break;
      
    case SYS_CREATE:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_pointer (esp + 2);
      validate_pointer ((uint8_t *) (esp + 2) + 3);
      validate_string ((const char *) *(esp + 1));
      f->eax = sys_create ((const char *) *(esp + 1), (unsigned) *(esp + 2));
      break;
      
    case SYS_REMOVE:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_string ((const char *) *(esp + 1));
      f->eax = sys_remove ((const char *) *(esp + 1));
      break;
      
    case SYS_OPEN:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_string ((const char *) *(esp + 1));
      f->eax = sys_open ((const char *) *(esp + 1));
      break;
      
    case SYS_FILESIZE:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      f->eax = sys_filesize ((int) *(esp + 1));
      break;
      
    case SYS_READ:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_pointer (esp + 2);
      validate_pointer ((uint8_t *) (esp + 2) + 3);
      validate_pointer (esp + 3);
      validate_pointer ((uint8_t *) (esp + 3) + 3);
      validate_buffer ((void *) *(esp + 2), (unsigned) *(esp + 3));
      f->eax = sys_read ((int) *(esp + 1), (void *) *(esp + 2), 
                         (unsigned) *(esp + 3));
      break;
      
    case SYS_WRITE:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_pointer (esp + 2);
      validate_pointer ((uint8_t *) (esp + 2) + 3);
      validate_pointer (esp + 3);
      validate_pointer ((uint8_t *) (esp + 3) + 3);
      validate_buffer ((const void *) *(esp + 2), (unsigned) *(esp + 3));
      f->eax = sys_write ((int) *(esp + 1), (const void *) *(esp + 2), 
                          (unsigned) *(esp + 3));
      break;
      
    case SYS_SEEK:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      validate_pointer (esp + 2);
      validate_pointer ((uint8_t *) (esp + 2) + 3);
      sys_seek ((int) *(esp + 1), (unsigned) *(esp + 2));
      break;
      
    case SYS_TELL:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      f->eax = sys_tell ((int) *(esp + 1));
      break;
      
    case SYS_CLOSE:
      validate_pointer (esp + 1);
      validate_pointer ((uint8_t *) (esp + 1) + 3);
      sys_close ((int) *(esp + 1));
      break;
      
    default:
      syscall_exit (-1);
      break;
    }
}

/* Terminates Pintos */
static void
sys_halt (void)
{
  shutdown_power_off ();
}

/* Terminates the current user program */
static void
sys_exit (int status)
{
  syscall_exit (status);
}

/* Helper function for exit */
static void
syscall_exit (int status)
{
  struct thread *cur = thread_current ();
  cur->exit_status = status;
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

/* Runs the executable given in cmd_line */
static int
sys_exec (const char *cmd_line)
{
  return process_execute (cmd_line);
}

/* Waits for a child process and returns its exit status */
static int
sys_wait (int pid)
{
  return process_wait ((tid_t) pid);
}

/* Creates a new file */
static bool
sys_create (const char *file, unsigned initial_size)
{
  bool success;
  
  lock_acquire (&filesys_lock);
  success = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  
  return success;
}

/* Deletes a file */
static bool
sys_remove (const char *file)
{
  bool success;
  
  lock_acquire (&filesys_lock);
  success = filesys_remove (file);
  lock_release (&filesys_lock);
  
  return success;
}

/* Opens a file */
static int
sys_open (const char *file)
{
  struct file *f;
  struct thread *cur = thread_current ();
  int fd;
  
  lock_acquire (&filesys_lock);
  f = filesys_open (file);
  lock_release (&filesys_lock);
  
  if (f == NULL)
    return -1;
  
  /* Find available file descriptor */
  for (fd = 2; fd < 128; fd++)
    {
      if (cur->fd_table[fd] == NULL)
        {
          cur->fd_table[fd] = f;
          return fd;
        }
    }
  
  /* No available file descriptors */
  lock_acquire (&filesys_lock);
  file_close (f);
  lock_release (&filesys_lock);
  return -1;
}

/* Returns the size of a file */
static int
sys_filesize (int fd)
{
  struct thread *cur = thread_current ();
  struct file *f;
  int size;
  
  if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
    return -1;
  
  f = cur->fd_table[fd];
  
  lock_acquire (&filesys_lock);
  size = file_length (f);
  lock_release (&filesys_lock);
  
  return size;
}

/* Reads from a file */
static int
sys_read (int fd, void *buffer, unsigned size)
{
  struct thread *cur = thread_current ();
  struct file *f;
  int bytes_read;
  unsigned i;
  
  if (fd == 0)
    {
      /* Read from stdin */
      for (i = 0; i < size; i++)
        ((char *) buffer)[i] = input_getc ();
      return size;
    }
  
  if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
    return -1;
  
  f = cur->fd_table[fd];
  
  lock_acquire (&filesys_lock);
  bytes_read = file_read (f, buffer, size);
  lock_release (&filesys_lock);
  
  return bytes_read;
}

/* Writes to a file */
static int
sys_write (int fd, const void *buffer, unsigned size)
{
  struct thread *cur = thread_current ();
  struct file *f;
  int bytes_written;
  
  if (fd == 1)
    {
      /* Write to stdout */
      putbuf (buffer, size);
      return size;
    }
  
  if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
    return -1;
  
  f = cur->fd_table[fd];
  
  lock_acquire (&filesys_lock);
  bytes_written = file_write (f, buffer, size);
  lock_release (&filesys_lock);
  
  return bytes_written;
}

/* Changes the position in a file */
static void
sys_seek (int fd, unsigned position)
{
  struct thread *cur = thread_current ();
  struct file *f;
  
  if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
    return;
  
  f = cur->fd_table[fd];
  
  lock_acquire (&filesys_lock);
  file_seek (f, position);
  lock_release (&filesys_lock);
}

/* Returns the position in a file */
static unsigned
sys_tell (int fd)
{
  struct thread *cur = thread_current ();
  struct file *f;
  unsigned position;
  
  if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
    return 0;
  
  f = cur->fd_table[fd];
  
  lock_acquire (&filesys_lock);
  position = file_tell (f);
  lock_release (&filesys_lock);
  
  return position;
}

/* Closes a file */
static void
sys_close (int fd)
{
  struct thread *cur = thread_current ();
  struct file *f;
  
  if (fd < 2 || fd >= 128 || cur->fd_table[fd] == NULL)
    return;
  
  f = cur->fd_table[fd];
  
  lock_acquire (&filesys_lock);
  file_close (f);
  lock_release (&filesys_lock);
  
  cur->fd_table[fd] = NULL;
}
