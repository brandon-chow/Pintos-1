#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"

#include <debug.h>

#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"

void frame_map (void *frame_addr, struct page *page, bool writable);
void frame_unmap (void *frame_addr);

static unsigned frame_hash (const struct hash_elem *e, void *aux);
static bool frame_less (const struct hash_elem *a,
                        const struct hash_elem *b,
                        void *aux);
static void *frame_allocator_evict_page (void);
static struct frame *frame_allocator_choose_eviction_frame (void);
static void frame_allocator_save_frame (struct frame*);

struct lock frame_table_lock;
struct lock frame_allocation_lock;

/* Initialises the frame table. */
void
frame_table_init(void)
{
  hash_init (&frame_table, frame_hash, frame_less, NULL);
  lock_init (&frame_table_lock);

  /* We must prevent multiple pages allocating at the same time to avoid eviction problems. */
  lock_init (&frame_allocation_lock);
}

void frame_map(void *frame_addr, struct page *page, bool writable)
{ 
  struct frame *new_fr = NULL;
  new_fr = malloc (sizeof(struct frame));
  if(!new_fr) 
    PANIC("Failed to malloc memory for struct frame");

  new_fr->page = page;
  new_fr->frame_addr = frame_addr;
  new_fr->owner_id = thread_current()->tid;
  new_fr->unused_count = 0;

  lock_acquire (&frame_table_lock);
  hash_insert(&frame_table, &new_fr->hash_elem);
  lock_release (&frame_table_lock);
}

void frame_unmap(void *frame_addr)
{
  struct frame f;
  f.frame_addr = frame_addr;

  lock_acquire (&frame_table_lock);
  hash_delete (&frame_table, &f.hash_elem);
  lock_release (&frame_table_lock);
}


/* Hash function for the frame table. */
static unsigned
frame_hash(const struct hash_elem *e, void *aux UNUSED)
{
  const struct frame *f = hash_entry (e, struct frame, hash_elem);

  return hash_bytes (&f->frame_addr, sizeof (f->frame_addr));
}

static bool
frame_less (const struct hash_elem *a, const struct hash_elem *b,
      void *aux UNUSED)
{
  const struct frame *frame_a = hash_entry (a, struct frame, hash_elem);
  const struct frame *frame_b = hash_entry (b, struct frame, hash_elem);

  return frame_a->frame_addr < frame_b->frame_addr;
}

/* Getting user frames */
void *
frame_allocator_get_user_page(struct page* page, enum palloc_flags flags,
                              bool writable)
{
  int iE;
  lock_acquire(&frame_allocation_lock);
  void * user_vaddr = page->vaddr;

  ASSERT(is_user_vaddr(user_vaddr));

  void *kernel_vaddr = palloc_get_page (PAL_USER | flags);

  if (!kernel_vaddr) {
    frame_allocator_evict_page();
    kernel_vaddr = palloc_get_page (PAL_USER | flags);
    ASSERT(kernel_vaddr)
  }

  size_t i;

  /* Map the frame used to it's virtual address. */
  if (!install_page(user_vaddr, kernel_vaddr, writable)) {
    PANIC("Could not install user page %p", user_vaddr);
  }

  frame_map (kernel_vaddr, page, writable);

  lock_release(&frame_allocation_lock);

  memset(kernel_vaddr, 0, PGSIZE);

  return kernel_vaddr;
}

void
frame_allocator_free_user_page(void* kernel_vaddr, bool is_locked)
{
  if (!is_locked)
    lock_acquire (&frame_allocation_lock);
  
  palloc_free_page (kernel_vaddr);

  struct frame lookup;
  lookup.frame_addr = kernel_vaddr;

  struct hash_elem *e = hash_find (&frame_table, &lookup.hash_elem);
  if (!e)
    PANIC("Frame doesn't exist in frame table.");

  struct frame *f = hash_entry (e, struct frame, hash_elem);
  if (!f)
    PANIC ("Could not load frame info from frame table");


  f->page->page_status &= ~PAGE_IN_MEMORY;

  tid_t thread_id = f->owner_id;
  struct thread* t = thread_lookup(thread_id);
  if(!t)
    PANIC("Corruption of frame table");

  pagedir_clear_page (t->pagedir, f->page->vaddr);
  frame_unmap (kernel_vaddr);  
  free(f);

  if (!is_locked)
    lock_release (&frame_allocation_lock);
}

static void *
frame_allocator_evict_page(void)
{
  struct frame * f = frame_allocator_choose_eviction_frame();

  frame_allocator_save_frame (f);
  frame_allocator_free_user_page(f->frame_addr, true);
}

static void
frame_allocator_save_frame (struct frame *f)
{
  /* Get the owner thread. */
  tid_t thread_id = f->owner_id;
  struct thread* t = thread_lookup (thread_id);
  if(!t)
    PANIC("Corruption of frame table");

  ASSERT(f->page);

  bool dirty_flag = pagedir_is_dirty (t->pagedir, f->page->vaddr);
  enum page_status status = f->page->page_status;
 

  if ((status & PAGE_MEMORY_MAPPED) && dirty_flag)
  {
      struct page_mmap_info *mmap_info = (struct page_mmap_info *)f->page->aux;
      struct mmap_mapping *m = mmap_get_mapping (&t->mmap_table, mmap_info->mapid);
      
      mmap_write_back_data (m, f->frame_addr, mmap_info->offset, mmap_info->length);
  } else if (!(f->page->page_status & PAGE_FILESYS)) {  
    // Allocate some Swap memory
    struct swap_entry *s = swap_alloc();
    if (!s) {
      PANIC("Frame Eviction: No Swap Memory left!");
    }

    /* Set the page status to swap. */
    f->page->page_status |= PAGE_SWAP;
    f->page->page_status &= ~(PAGE_IN_MEMORY);
    f->page->aux = s;

    /* Save the data into the swap slot. */
    swap_save(s, (void*)f->frame_addr);
  } 
}

struct frame *
frame_allocator_choose_eviction_frame (void)
{
  struct hash_iterator i;
  struct thread *t;
  struct frame * eviction_candidate;
  int32_t least_used = 0;
  bool dirty_candidate = true;
  bool accessed_candidate = true;
  bool dirty;
  bool accessed;

  // We aim to use pseudo LRU replacement.
  // When we choose to evict, find the oldest page which hasn't
  // been accessed since the last eviction. We do
  // this by choosing the page which has the greatest 
  // unused_count, which increments when it is unused in this 
  // algorithm.
  lock_acquire (&frame_table_lock);
  hash_first (&i, &frame_table);
  // Iterate through the frame table.
  while(hash_next (&i)) {
    // Get the frame that it is equivalent to
    struct frame *f = hash_entry (hash_cur (&i), struct frame, hash_elem);
    // Get the owning thread
    t = thread_lookup(f->owner_id);
    dirty = pagedir_is_dirty(t->pagedir, f->frame_addr);
    accessed = pagedir_is_accessed(t->pagedir, f->frame_addr);
    // If it is accessed, set is as not accessed and move on

    if (accessed) {
      if (!accessed_candidate) {
        f->unused_count = 0;
        break;
      }
    }
    else
     f->unused_count++;

    if (dirty) {
      if (!dirty_candidate) {
        f->unused_count = 0;
        break;
      }
    }
    else
     f->unused_count++;

    if (++f->unused_count > least_used && !(f->page->page_status & PAGE_FILESYS)) {
      eviction_candidate = f;
      dirty_candidate = dirty;
      accessed_candidate = accessed;  
      least_used = f->unused_count;
    }
  }

  eviction_candidate->unused_count = 0;
  lock_release (&frame_table_lock);
  return eviction_candidate;
}