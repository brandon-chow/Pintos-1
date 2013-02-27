#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

struct argument
{
    char *token;
    struct list_elem token_list_elem;
};

struct stack_setup_data
{
  struct list argv;
  int argc;
};

unsigned file_descriptor_table_hash_function (const struct hash_elem *e, void *aux);
bool file_descriptor_table_less_func (const struct hash_elem *a,
                                      const struct hash_elem *b,
                                      void *aux);
void file_descriptor_table_destroy_func (struct hash_elem *e, void *aux);

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
int sum_fileopen(struct thread * t, struct file * f);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
user_process_execute (const char *file_name) 
{ 
  tid_t usr_proc_tid = process_load_setup(file_name);

  return usr_proc_tid;
}

tid_t
process_execute (const char *file_name)
{
    return process_load_setup(file_name);
}


tid_t
process_load_setup(const char *file_name)
{
  char *fn_copy;
    tid_t tid;

    /* Make a copy of FILE_NAME.
       Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page (0);
    // fn_copy = "run.exe arg1 arg2 arg3..."
    if (fn_copy == NULL)
    {
        return TID_ERROR;
    }
    strlcpy (fn_copy, file_name, PGSIZE);

    struct stack_setup_data* setup_data = NULL;

    setup_data = malloc(sizeof(struct stack_setup_data));

    if (setup_data == NULL)
    {
        return TID_ERROR;
    }

    list_init(&setup_data->argv);
    setup_data->argc = 0;

    char *token, *pos;

    //printf("----Beginning tokenization\n");

   // ASSERT(fn_copy =! NULL);

    for (token = strtok_r (fn_copy, " ", &pos); token != NULL;
            token = strtok_r (NULL, " ", &pos))
    {
        struct argument *arg = NULL;
        arg = malloc(sizeof(struct argument));
        
        if(arg == NULL)
        {
          return TID_ERROR;
        }

        arg->token = token;
        list_push_front(&setup_data->argv, &arg->token_list_elem);
        //printf ("'%s'\n", token);
        setup_data->argc++;
    }

    //printf("----Ending tokenizatin\n");

    struct argument *fst_arg = list_entry(list_back(&setup_data->argv), struct argument, token_list_elem);
   // printf("%s %d\n", fst_arg->token, argc);

    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create (fst_arg->token, PRI_DEFAULT, start_process, setup_data);
    if (tid == TID_ERROR)
    {
        palloc_free_page (fn_copy);
    }

    // We want to collect an exit_status of -1, and return it, if thread cannot start
    // Get the thread structure
    struct list_elem *e;
    struct thread *cur = thread_current();
    for (e = list_begin (&cur->children); e != list_end (&cur->children);
	     e = list_next (e))
	{
		struct thread *t = list_entry (e, struct thread, procelem);
		if (t->tid == tid) {
			lock_acquire(&t->anchor);
			cond_wait(&t->condvar_process_sync, &t->anchor);
			// If the exit status is -1, this is what we want to return
			if (t->exit_status == -1)
				return -1;
			cond_signal(&t->condvar_process_sync, &t->anchor);
			lock_release(&t->anchor);
		}
	}


    return tid;
}


/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *setup_data_)
{

  struct stack_setup_data *setup_data = (struct stack_setup_data *) setup_data_;

  //struct params_struct *params = params_;
  //printf("Casted to params ptr\n");
  struct intr_frame if_;
  bool success;

   struct argument *fst_arg = list_entry(list_back(&setup_data->argv), struct argument, token_list_elem);
   char *fst_arg_saved = fst_arg->token; 

  //printf("----Proc name is %s\n", fst_arg_saved);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (fst_arg_saved, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  
  struct thread *cur = thread_current();

  lock_acquire(&cur->anchor);

  // Signal the parent process about the execution's validity
  cur->exit_status = success?1:-1;
  cond_signal(&cur->condvar_process_sync, &cur->anchor);
  // Wait for it to get the signal
  cond_wait(&cur->condvar_process_sync, &cur->anchor);
  lock_release(&cur->anchor);

  // Exit the process if the file failed to load
  if (!success)
    thread_exit ();
  // Return to default failure exit_status in case of exceptions
  cur->exit_status = -1;
  
  /*Set Up Stack here*/

  struct list_elem *e;

  //Push the actual strings by copying them, then change the 
  //char* value stored in the arguments list so that we can traverse again.

  for (e = list_begin (&setup_data->argv); e != list_end (&setup_data->argv);
          e = list_next (e))
  {
      struct argument *arg = list_entry (e, struct argument, token_list_elem);
      //printf("Got actual argument\n");
      //printf("%s\n", arg->token );
      char *curr_arg = arg->token;
      //printf("%s\n", curr_arg );
      if_.esp -= (strlen(curr_arg) + 1);
      strlcpy (if_.esp, curr_arg, strlen(curr_arg) + 1);
      // printf("Copied String\n");
     // printf("Esp is pointing to %s at location 0x%x\n", if_.esp, (unsigned int)if_.esp);
      arg->token = if_.esp;
      // printf("Stored pointer\n");
      // printf("Decreasing by %d\n", strlen(curr_arg) + 1);
      //printf("Decresed esp\n");
  }

   //hex_dump(0, if_.esp, 100, true);

    //printf("---First Pass done\n");
   //Push word align

  uint8_t align = 0;
  if_.esp -= (sizeof(uint8_t));
  *(uint8_t *)if_.esp = align;

  char *last_arg_ptr  = NULL;
  if_.esp-= (sizeof(char *));
  *(int32_t *)if_.esp = last_arg_ptr;


  for (e = list_begin (&setup_data->argv); e != list_end (&setup_data->argv);)
  {
	struct argument *arg = list_entry (e, struct argument, token_list_elem);
	char *curr_arg = arg->token;
	if_.esp -= (sizeof(char*));
	*(int32_t *)if_.esp = curr_arg;
	e = list_next (e);
	free(arg);
	//printf("Esp is pointing to 0x%x at addr 0x%x\n", *((int32_t*)if_.esp), (unsigned int)if_.esp);
	// printf("pushed ptr\n");
  }



      // printf("---Second pass done\n");

  char **fst_arg_ptr = if_.esp;
   // printf("Esp value is 0x%x\n", (unsigned int) if_.esp);
  // printf("I'm about to push 0x%x\n", fst_arg_ptr);
  if_.esp -= (sizeof(char **));
  *(int32_t *)if_.esp = fst_arg_ptr;
      
      //   printf("Esp is pointing to 0x%x\n", *((int32_t*)if_.esp) );

      // printf("---Pushed argv\n");
  if_.esp -=(sizeof(setup_data->argc));
  *(int32_t *)if_.esp = setup_data->argc;
     

       // printf("----Pushed argc\n");
       //   printf("Esp is pointing to %d\n", *((int32_t*)if_.esp) );

  void *fake_return  = 0;
  if_.esp -= (sizeof(void *));
  *(int32_t *)if_.esp = fake_return;

      // printf("Esp is pointing to %d\n", *((int32_t*)if_.esp) );
      

      //  printf("---------The value of esp at the beginning is 0x%x\n", (unsigned int)if_.esp);

        /*Free all the list*/

    //  for (e = list_begin (&argv); e != list_end (&argv);
    //         e = list_next (e))
    // {
    //   struct argument *arg = list_entry (e, struct argument, token_list_elem);
    //   free(arg);
    // }


    /* Start the user process by simulating a return from an
       interrupt, implemented by intr_exit (in
       threads/intr-stubs.S).  Because intr_exit takes all of its
       arguments on the stack in the form of a `struct intr_frame',
       we just point the stack pointer (%esp) to our stack frame
       and jump to it. */

       //hex_dump(0, if_.esp, 100, true);



    palloc_free_page (fst_arg_saved);
    free(setup_data);

    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
    NOT_REACHED ();
}

int sum_fileopen(struct thread * t, struct file * f) {
	struct list_elem *e;
	int count = 0;
	if (t->file == f) count++;
    for (e = list_begin (&t->children); e != list_end (&t->children);
	     e = list_next (e))
	{
    	struct thread *child = list_entry (e, struct thread, procelem);
    	count += sum_fileopen(child, f);
	}
    return count;
}


/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid)
{
	struct thread * cur = thread_current();
	struct list_elem * e;
	// Lookup child_tid in children
    for (e = list_begin (&cur->children); e != list_end (&cur->children);
	     e = list_next (e))
	{
		struct thread *t = list_entry (e, struct thread, procelem);
		if (t->tid == child_tid) {
			lock_acquire(&t->anchor);
			cond_wait(&t->condvar_process_sync, &t->anchor);
			int exitStatus = t->exit_status;
			cond_signal(&t->condvar_process_sync, &t->anchor);
			lock_release(&t->anchor);

			return exitStatus;
		}
	}
    return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
    // TODO: Use a condition variable synchronisation primitive instead of disabling interrupts 
    // Because from a design perspective, behaviour is more ensured.
    struct thread *cur = thread_current ();
    uint32_t *pd;

    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

    // Go to the most senior process
    struct thread *parent = cur;
    while (parent->parent != NULL)
    	parent = parent->parent;

    // Add up how many times (recursively) the executing file is open
    int file_executing_count = 0;
    file_executing_count = sum_fileopen(parent, cur->file);

    // If only one time, i.e. this thread only
    if (file_executing_count == 1) {
    	file_close(cur->file);
    }

    pd = cur->pagedir;
    // Remove this process from the parent's child process list
    list_remove(&cur->procelem);

    // Tell the processes waiters that this process is finished
    lock_acquire(&cur->anchor);
    cond_broadcast(&cur->condvar_process_sync, &cur->anchor);
    cond_wait(&cur->condvar_process_sync, &cur->anchor);
    lock_release(&cur->anchor);

    /* Destroy the current process's page directory and switch back
       to the kernel-only page directory. */
    if (pd != NULL)
    {
        /* Correct ordering here is crucial.  We must set
           cur->pagedir to NULL before switching page directories,
           so that a timer interrupt can't switch back to the
           process page directory.  We must activate the base page
           directory before destroying the process's page
           directory, or our active page directory will be one
           that's been freed (and cleared). */
        cur->pagedir = NULL;

        pagedir_activate (NULL);
        pagedir_destroy (pd);
    }

    /* Destroy the file descriptor table, closing all file descripotrs as we go */
    hash_destroy(&cur->file_descriptor_table,
                 &file_descriptor_table_destroy_func);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
    struct thread *t = thread_current ();

    /* Activate thread's page tables. */
    pagedir_activate (t->pagedir);

    /* Set thread's kernel stack for use in processing
       interrupts. */
    tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
    struct thread *t = thread_current ();
    struct Elf32_Ehdr ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* Allocate and activate page directory. */
    t->pagedir = pagedir_create ();
    if (t->pagedir == NULL)
        goto done;
    process_activate ();

    /* Open executable file. */
    file = filesys_open (file_name);
    if (file == NULL)
    {
        printf ("load: %s: open failed\n", file_name);
        goto done;
    }


    /* Read and verify executable header. */
    if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
            || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
            || ehdr.e_type != 2
            || ehdr.e_machine != 3
            || ehdr.e_version != 1
            || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
            || ehdr.e_phnum > 1024)
    {
        printf ("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++)
    {
        struct Elf32_Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length (file))
            goto done;
        file_seek (file, file_ofs);

        if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;
        case PT_LOAD:
            if (validate_segment (&phdr, file))
            {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0)
                {
                    /* Normal segment.
                       Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                  - read_bytes);
                }
                else
                {
                    /* Entirely zero.
                       Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment (file, file_page, (void *) mem_page,
                                   read_bytes, zero_bytes, writable))
                    goto done;
            }
            else
                goto done;
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack (esp))
        goto done;

    /* Set up file descriptor table. */
    hash_init (&t->file_descriptor_table,
               &file_descriptor_table_hash_function,
               &file_descriptor_table_less_func,
               NULL);
    t->next_fd = 2;

    /* Start address. */
    *eip = (void ( *) (void)) ehdr.e_entry;

    success = true;

    // Deny writes to a currently running executable
    file_deny_write(file);

done:

    /* We close the file when it finishes executing. */
    t->file = file;

    return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (Elf32_Off) file_length (file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr ((void *) phdr->p_vaddr))
        return false;
    if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed
       it then user code that passed a null pointer to system calls
       could quite likely panic the kernel by way of null pointer
       assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
    ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT (pg_ofs (upage) == 0);
    ASSERT (ofs % PGSIZE == 0);

    file_seek (file, ofs);
    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Calculate how to fill this page.
           We will read PAGE_READ_BYTES bytes from FILE
           and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page (PAL_USER);
        if (kpage == NULL)
            return false;

        /* Load this page. */
        if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
            palloc_free_page (kpage);
            return false;
        }
        memset (kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page (upage, kpage, writable))
        {
            palloc_free_page (kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp)
{
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page (PAL_USER | PAL_ZERO);
    if (kpage != NULL)
    {
        success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
        if (success)
            *esp = PHYS_BASE;
        else
            palloc_free_page (kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current ();

    /* Verify that there's not already a page at that virtual
       address, then map our page there. */
    return (pagedir_get_page (t->pagedir, upage) == NULL
            && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* File descriptor functions. */

struct file_descriptor *
process_get_file_descriptor_struct(int fd)
{
  // fd 0 and 1 are reserved for stout and stderr respectively.
  if (fd < 2)
    return NULL;

  struct file_descriptor descriptor;
  descriptor.fd = fd;

  struct thread *t = thread_current ();
  struct hash_elem *found_element = hash_find (&t->file_descriptor_table,
                                               &descriptor.hash_elem);
  if (found_element == NULL)
    return NULL;

  struct file_descriptor *open_file_descriptor = hash_entry (found_element,
                                                             struct file_descriptor,
                                                             hash_elem);

  return open_file_descriptor;
}

unsigned
file_descriptor_table_hash_function (const struct hash_elem *e, void *aux)
{
  struct file_descriptor *descriptor =  hash_entry (e, struct file_descriptor, hash_elem);

  return descriptor->fd;
}

bool
file_descriptor_table_less_func (const struct hash_elem *a,
                           const struct hash_elem *b,
                           void *aux)
{
  struct file_descriptor *descriptor_a =  hash_entry (a,
                                                      struct file_descriptor,
                                                      hash_elem);
  struct file_descriptor *descriptor_b =  hash_entry (b,
                                                      struct file_descriptor,
                                                      hash_elem);

  return descriptor_a->fd < descriptor_b->fd;
}

void
file_descriptor_table_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
  struct file_descriptor *descriptor =  hash_entry (e,
                                                    struct file_descriptor,
                                                    hash_elem);

  ASSERT (descriptor->file != NULL);

  // Close the file descriptor for the open file.
  close_syscall (descriptor->file);
}
