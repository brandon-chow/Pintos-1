#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Ordered list of processes currently sleeping. Processes in this list 
   have state set to THREAD_SLEEP. This list is ordered suck that the head
   is the next thread to be woken up.*/
static struct list sleeping_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */

static long long idle_ticks;          /* # of timer ticks spent idle. */
static long long kernel_ticks;        /* # of timer ticks in kernel threads. */
static long long user_ticks;          /* # of timer ticks in user programs. */



/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void thread_enqueue (struct thread *t);
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
bool has_higher_priority(const struct list_elem *, const struct list_elem *, void *);

bool sleep_list_less_func(const struct list_elem *a, const struct list_elem *b, void *aux);
void thread_donate_priority_lock_rec(struct thread *acceptor, struct lock* lock);

/* For the MLFQ Scheduler. */

/* # of ticks after which priorities are recomputed. */
#define MLFQS_RECOMPUTE_INTERVAL 4

/* The list of ready-to-run threads used for the mlfqs scheduler. */
static struct list thread_mlfqs_queue;

/* # of timer ticks until the thread priorities will be recomputed. */
static long long mlfqs_recompute_ticks;
static fixed_point thread_mlfqs_load_avg;        /* The system load average. */

static void thread_mlfqs_init (void);
static void thread_mlfqs_recompute_load_avg (void);
static void thread_mlfqs_recompute_all_priorities (void);
static void thread_mlfqs_recompute_priority (struct thread *t, void *aux);
static int thread_mlfqs_get_recent_cpu (struct thread *t);
static void thread_mlfqs_recompute_recent_cpu (struct thread *t, void *aux);
static void thread_mlfqs_recompute_all_recent_cpu (void);
static int thread_mlfqs_get_nice (struct thread *t);
static bool thread_mlfqs_less_function(const struct list_elem *a,
                                       const struct list_elem *b,
                                       void *aux);
static void thread_mlfqs_print_threads(void);



/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  list_init (&sleeping_list);

  if (thread_mlfqs) {
    thread_mlfqs_init();
  }

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  if (thread_mlfqs) {
    initial_thread->nice = 0;
    initial_thread->recent_cpu = INT_TO_FIX(0);
  }
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */

  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

    thread_sleep_ticker();

  if (thread_mlfqs) {
    if (t != idle_thread)
      t->recent_cpu = ADD_FIXED_INT(t->recent_cpu, 1);

    /* Recompute the load_avg first as this is depended on by recent_cpu. */
    if (timer_ticks () % TIMER_FREQ == 0) {
      thread_mlfqs_recompute_load_avg ();
      thread_mlfqs_recompute_all_recent_cpu ();
    }

    if (++mlfqs_recompute_ticks == MLFQS_RECOMPUTE_INTERVAL) {
      thread_mlfqs_recompute_all_priorities ();
      mlfqs_recompute_ticks = 0;
    }
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */

tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  
  // Initialise timer sleep member to 0
  t->wakeup_tick = 0;

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  if (thread_mlfqs) {
    t->nice = thread_get_nice ();
  }


  intr_set_level (old_level);



  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Puts the current thread to sleep for a given integer ticks. It will not be
   scheduled until the given ticks has elapsed, as measured by thread_tick()
   */
void
thread_sleep (int ticks) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  struct thread *t   = thread_current ();
  struct thread *cur = NULL;

  // We store an absolute tick number which the thread should sleep until.
  t->wakeup_tick = timer_ticks () + ticks;

  ASSERT(t->status != THREAD_SLEEP);
  list_insert_ordered(&sleeping_list, &t->elem, &sleep_list_less_func, NULL);
  
  cur = thread_current ();
  cur->status = THREAD_SLEEP;
  schedule();
}

bool sleep_list_less_func(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);

  return thread_a->wakeup_tick < thread_b->wakeup_tick;
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;


  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  
  thread_enqueue(t);
  
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

  #ifdef USERPROG
      process_exit ();
  #endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */

  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();

  // thread_enqueue() may call thread_yield() in the round-robin scheduler,
  // so do the list insertion ourselves.
  if (cur != idle_thread) {
    if (thread_mlfqs) {
      list_insert_ordered (&thread_mlfqs_queue,
                           &cur->elem,
                           &thread_mlfqs_less_function,
                           NULL);
    } else {
      list_insert_ordered (&ready_list, &cur->elem, &has_higher_priority, NULL);
    }
  }

  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

static void
thread_enqueue (struct thread *t)
{
  t->status = THREAD_READY;

  if (thread_mlfqs) {
    int running_pri = thread_get_priority();

    list_insert_ordered (&thread_mlfqs_queue,
                         &t->elem,
                         &thread_mlfqs_less_function,
                         NULL);

    if (t->priority > running_pri) {
      if (intr_context())
        intr_yield_on_return ();
      else
        thread_yield ();
    }
  } else {
    int running_pri = thread_get_priority();
    int new_pri = thread_explicit_get_priority(t);

    if (thread_current () != idle_thread && 
                        new_pri > running_pri) {

      list_push_front(&ready_list, &t->elem);
      thread_yield();
    } 
    else   
     list_insert_ordered (&ready_list, &t->elem, &has_higher_priority,
                       NULL);
  }
}

void thread_sleep_ticker (void) {
  struct list_elem *e = NULL;
  struct thread *t = NULL;

  e = list_begin(&sleeping_list);

  int64_t current_tick = timer_ticks ();
  
  while(e != list_end(&sleeping_list))
  {
    t = list_entry(e, struct thread, elem);

    // Since the list is ordered, if the thread's wakeup tick is greater than
    // this tick, then there are no more threads to wake up.
    if (t->wakeup_tick > current_tick)
      return;

    e = list_remove(e);

    // We won't be pre-empted here because we're already in the timer interrupt
    // handler.
    ASSERT(t->status == THREAD_SLEEP);

    thread_enqueue(t);
  }
}

/* Invoke function 'func' on all threads in a given list, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreachinlist (struct list * thread_list, thread_action_func *func,
                      void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (thread_list); e != list_end (thread_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}


/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  thread_foreachinlist(&all_list, func, aux);
}

/*
    */
struct thread *
thread_lookup(tid_t tid)
{
  struct list_elem *e;
  for (e = list_begin (&all_list); e != list_end (&all_list);
	   e = list_next (e))
	{
	  struct thread *t = list_entry (e, struct thread, allelem);
	  if (t->tid == tid)
		  return t;
	}
  return NULL;
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current()->priority = new_priority;
  thread_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_explicit_get_priority(thread_current());
}

/* Returns the given thread's priority. */
int
thread_explicit_get_priority (struct thread *t)
{
  if (!list_empty(&t->lock_list)) {
    struct list_elem *e = list_begin (&t->lock_list);
    struct lock *l = list_entry(e, struct lock, elem);
    return MAX(*l->semaphore.priority, t->priority);
  }
  else {
    return t->priority;
  }
}


void thread_donate_priority_lock_rec(struct thread *acceptor, struct lock* lock)
{
  // If the acceptor is blocked, find out what by, and donate to that lock's priority if required
  if (acceptor->status == THREAD_BLOCKED) {
    if (acceptor->blocker && *acceptor->blocker->semaphore.priority < *lock->semaphore.priority) {
      acceptor->blocker->semaphore.priority = lock->semaphore.priority;
      if (acceptor->blocker->holder->blocker) {
        thread_donate_priority_lock_rec(acceptor->blocker->holder, lock);
      }
    }
  }  
}

/*Sets the priority of the threat acceptor to the value new priority*/
void
thread_donate_priority_lock(struct thread *acceptor, struct lock* lock) 
{
  ASSERT(is_thread(acceptor));
  // Push this into the acceptors lock list, ordered by priority
  list_insert_ordered(&acceptor->lock_list, &lock->elem, &lock_has_higher_priority, NULL);
  thread_donate_priority_lock_rec(acceptor, lock);
}

/* Removes the thread's priority that is associated with the given lock
  from the thread's priority list*/
void
thread_restore_priority_lock(struct lock* lock)
{
  list_remove(&lock->elem);
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  ASSERT (thread_mlfqs);

  struct thread *t = thread_current ();
  t->nice = nice;

  thread_mlfqs_recompute_priority (t, NULL);
  list_sort (&thread_mlfqs_queue, &thread_mlfqs_less_function, NULL);

  // Yield if our priority is now not the highest.
  if (!list_empty (&thread_mlfqs_queue)) {
    int highest_priority = list_entry (list_front (&thread_mlfqs_queue),
                                       struct thread,
                                       elem)->priority;

    if (thread_get_priority () < highest_priority)
      thread_yield ();
  }
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_mlfqs_get_nice (thread_current ());
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return FIX_TO_INT_R_NEAR(MUL_FIXED_INT(thread_mlfqs_load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  struct thread *t = running_thread ();

  return FIX_TO_INT_R_NEAR(MUL_FIXED_INT(t->recent_cpu, 100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);


  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;


  list_init(&t->lock_list);
  #ifdef USERPROG
  list_init(&t->children);
  #endif
  // Set Priority
  if (thread_mlfqs)
    thread_mlfqs_recompute_priority (t, NULL);
  else
    t->priority = priority;

  t->magic = THREAD_MAGIC;

  lock_init(&t->supplemental_page_table_lock);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (thread_mlfqs) {
    if (list_empty (&thread_mlfqs_queue))
      return idle_thread;
    else
      return list_entry (list_pop_front (&thread_mlfqs_queue), struct thread, elem);
  }

  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/*	Return true if the thread to which elem1 refers to 
	has higher priority than the thread elem2 refers to. */
bool
has_higher_priority (const struct list_elem *elem1,
                     const struct list_elem *elem2, 
                     void *aux UNUSED) 
{
	struct thread *thread1 = list_entry(elem1, struct thread, elem);
  int priority1 = thread_explicit_get_priority(thread1);
	struct thread *thread2 = list_entry(elem2, struct thread, elem);
  int priority2 = thread_explicit_get_priority(thread2);
	
	return priority1 > priority2;
}


/* Initialises the multilevel feedback queue scheduler. */
static void 
thread_mlfqs_init(void)
{
  ASSERT(thread_mlfqs);

  thread_mlfqs_load_avg = INT_TO_FIX (0);

  list_init (&thread_mlfqs_queue);
}

/* Recomputes the system load average. */
static void
thread_mlfqs_recompute_load_avg(void)
{
  ASSERT(thread_mlfqs);

  /* This method should only be called from the timer interrupt handler. */
  ASSERT (intr_get_level () == INTR_OFF);

  fixed_point first_sum = MUL_FIXED(DIV_FIXED(INT_TO_FIX(59),
                                              INT_TO_FIX(60)),
                                              thread_mlfqs_load_avg);

  int num_threads = list_size (&thread_mlfqs_queue);
  if (running_thread () != idle_thread)
    num_threads++;

  fixed_point second_sum = MUL_FIXED_INT(DIV_FIXED(INT_TO_FIX(1),
                                                   INT_TO_FIX(60)),
                                                   num_threads); 

  thread_mlfqs_load_avg = ADD_FIXED(first_sum, second_sum);
}

/* Recomputes the priority of thread t. The second parameter aux is
   included so that this function can be used as a parameter to
   thread_foreach() or thread_foreachinlist(), which expects a pointer to
   a function with return type void and arguments (struct thread*, void*). */

static void
thread_mlfqs_recompute_priority(struct thread *t, void *aux UNUSED)
{
  ASSERT(thread_mlfqs);

  int new_priority = PRI_MAX
                     - (thread_mlfqs_get_recent_cpu (t) / 4)
                     - (thread_mlfqs_get_nice (t) * 2);

  if (new_priority > PRI_MAX)
    new_priority = PRI_MAX;

  if (new_priority < PRI_MIN)
    new_priority = PRI_MIN;

  t->priority = new_priority;
}

/* Recomputes the priorities of all threads. */
static void
thread_mlfqs_recompute_all_priorities(void)
{
  ASSERT(thread_mlfqs);

  /* This method should only be called from the timer interrupt handler. */
  ASSERT (intr_get_level () == INTR_OFF);
  
  thread_foreach (&thread_mlfqs_recompute_priority, NULL);
  list_sort (&thread_mlfqs_queue, &thread_mlfqs_less_function, NULL);
}

/* Recomputes the recent_cpu values of all threads. */
static void
thread_mlfqs_recompute_all_recent_cpu(void)
{
  ASSERT(thread_mlfqs);

  /* This method should only be called from the timer interrupt handler. */
  ASSERT (intr_get_level () == INTR_OFF);

  thread_foreach (&thread_mlfqs_recompute_recent_cpu, NULL);
}

/* Recomputes the recent_cpu value of thread t. The second parameter aux is
   included so that this function can be used as a parameter to
   thread_foreach() or thread_foreachinlist(), which expects a pointer to
   a function with return type void and arguments (struct thread*, void*). */
static void
thread_mlfqs_recompute_recent_cpu(struct thread *t, void *aux UNUSED)
{
  ASSERT(thread_mlfqs);

  fixed_point twice_load_avg = MUL_FIXED_INT(thread_mlfqs_load_avg, 2);
  fixed_point coefficient = DIV_FIXED(twice_load_avg, ADD_FIXED_INT(twice_load_avg, 1));

  t->recent_cpu = ADD_FIXED_INT(MUL_FIXED(coefficient, t->recent_cpu), thread_mlfqs_get_nice (t));
}

/* Simple wrapper around the nice member of t. */
static int
thread_mlfqs_get_nice(struct thread *t)
{
  ASSERT(thread_mlfqs);

  return t->nice;
}

/* Returns the recent_cpu rounded down to the nearest integer. */
static int
thread_mlfqs_get_recent_cpu(struct thread *t)
{
  ASSERT(thread_mlfqs);

  return FIX_TO_INT_R_NEAR(t->recent_cpu);
}

/* Used to sort threads by priority descending. */
static bool
thread_mlfqs_less_function(const struct list_elem *a,
                           const struct list_elem *b,
                           void *aux UNUSED)
{
  struct thread *thread_a = list_entry (a, struct thread, elem);
  struct thread *thread_b = list_entry (b, struct thread, elem);

  return thread_a->priority > thread_b->priority;
}

/* Function to aid debugging by printing all of the threads in
   thread_mlfqs_queue. */
static void
thread_mlfqs_print_threads(void)
{
  ASSERT(thread_mlfqs);

  struct list_elem *e;

  printf ("current thread %p: p %d\n", thread_current (),
                                       thread_current ()->priority);

  for (e = list_begin (&thread_mlfqs_queue);
       e != list_end (&thread_mlfqs_queue); e = list_next (e)) {
    struct thread *t = list_entry (e, struct thread, elem);

    printf ("thread %p: p %d\n", t, t->priority);
  }

  printf ("\n");
 }

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
