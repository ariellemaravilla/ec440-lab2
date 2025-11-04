#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
// New Code (Arielle): Start
#include "threads/vaddr.h"      // for is_user_vaddr()
#include "userprog/process.h"   // for exit()


// get_user(): reads a byte at uaddr and returns val if successful (-1 if segfault)
// put_user(): writes byte to UDST, returns true on success, false if segfault
// check_user_ptr(): checks that a pointer is within the user address space and safely readable
// syscall_handler():
// sys_write()

static int
get_user(const uint8_t *uaddr) {
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:" : "=&a" (result) : "m" (*uaddr));
  return result;
}


static bool
put_user(uint8_t *udst, uint8_t byte) {
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:" : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

static void
check_user_ptr(const void *uaddr) {
  if (!is_user_vaddr(uaddr) || get_user((const uint8_t *) uaddr) == -1)
    sys_exit(-1);
}

// New Code (Arielle): Finish

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  // New Code (Arielle): Start
  uint32_t *esp = f -> esp;

  check_user_ptr(esp);

  int syscall_num = *(int *) esp;

  switch (syscall_num) {
    case SYS_WRITE:
      check_user_ptr(esp + 1);  //fd
      check_user_ptr(esp + 2);  //buffer
      check_user_ptr(esp + 3);  //size
      f->eax = sys_write(*(int *)(esp + 1), (const void *) *(esp + 2), *(unsigned *)(esp +3));
      break;
    
    case SYS_EXIT:
      check_user_ptr(esp + 1);
      sys_exit(*(int *)(esp + 1));
      break;  
    
    default:
      printf("Unknown syscall %d\n", syscall_num);
      thread_exit();
  }
  // New Code (Arielle): Finish
}


// New Code (Arielle): Start
int sys_write(int fd, const void *buffer, unsigned size) {
  if (fd == 1) {
    const uint8_t *buf = buffer;
    for (unsigned i = 0; i < size; i++) {
      check_user_ptr(buf + i);  
    }
    putbuf(buffer, size);
    return size;
  }
  return -1;
}


void
sys_exit(int status) {
  printf("%s: exit(%d)\n", thread_name(), status);
  thread_exit();
}

// New Code (Arielle): Finish