#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->elapsed_time = 0;
  p->arrival_time = ticks;
  p->sched_policy = -1;
  // p->deadline = 2147483647;
  p->deadline = -1;
  p->exec_time = -1;
  p->rate = -1;
  p->period = -1;


  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.




//round-robin
// void
// scheduler(void)
// {
//   struct proc *p;
//   struct cpu *c = mycpu();
//   c->proc = 0;
  
//   for(;;){
//     // Enable interrupts on this processor.
//     sti();

//     // Loop over process table looking for process to run.
//     acquire(&ptable.lock);
//     for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//       if(p->state != RUNNABLE)
//         continue;

//       // Switch to chosen process.  It is the process's job
//       // to release ptable.lock and then reacquire it
//       // before jumping back to us.
//       c->proc = p;
//       switchuvm(p);
//       p->state = RUNNING;

//       swtch(&(c->scheduler), p->context);
//       switchkvm();

//       // Process is done running for now.
//       // It should have changed its p->state before coming back.
//       c->proc = 0;
//     }
//     release(&ptable.lock);

//   }
// }return


void
scheduler(void)
{
  struct proc *p1;
  struct cpu *c = mycpu(); 
  c->proc = 0;
  struct proc *p;
  for(;;){
    int check_policy = -1;
    acquire(&ptable.lock);
    for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++){
      if(p1->state != UNUSED){
        check_policy = p1->sched_policy;
        break;
      }
    }
    release(&ptable.lock);
    if(check_policy == -1){
      sti();
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
    else if(check_policy == 0){
      // struct proc *edf_proc = &ptable.proc[0];
      // int min_deadline = -1;
      // sti();
      // acquire(&ptable.lock);
      // for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      //   if(p->state != RUNNABLE)
      //     continue;
      //   if(min_deadline == -1 || p->deadline < min_deadline){
      //     if(p->deadline != min_deadline || p->pid < edf_proc->pid){
      //       min_deadline = p->deadline;
      //       edf_proc = p;
      //     }
      //   }
      // }
      struct proc *edf_proc = 0;
      sti();
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        if(edf_proc == 0 || p->deadline < edf_proc->deadline){
          edf_proc = p;
        }
        else if(p->deadline == edf_proc->deadline && p->pid < edf_proc->pid){
          edf_proc = p;
        }
      }
      // p = edf_proc;
      // cprintf("edf_proc->pid = %d", edf_proc->pid);
      if(edf_proc != 0){
        c->proc = edf_proc;
        switchuvm(edf_proc);
        edf_proc->state = RUNNING;

        swtch(&(c->scheduler), edf_proc->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
    else{
      struct proc *rm_proc = 0;
      // int min_weight = -1;
      // sti();
      // acquire(&ptable.lock);
      // for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      //   if(p->state != RUNNABLE)
      //     continue;
      //   if(min_weight == -1 || p->weight < min_weight){
      //     if(p->weight != min_weight || p->pid < rm_proc->pid){
      //       min_weight = p->weight;
      //       rm_proc = p;
      //     }
      //   }
      // }
      sti();
      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;
        if(rm_proc == 0 || p->weight < rm_proc->weight){
          rm_proc = p;
        }
        else if(p->weight == rm_proc->weight && p->pid < rm_proc->pid){
          rm_proc = p;
        }
      }
      // p = edf_proc;
      // cprintf("edf_proc->pid = %d", edf_proc->pid);
      if(rm_proc != 0){
        c->proc = rm_proc;
        switchuvm(rm_proc);
        rm_proc->state = RUNNING;

        swtch(&(c->scheduler), rm_proc->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
  }
}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

void increase_elapsed_time(void)
{
  struct proc *p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if(p->state == RUNNING)
    {
      p->elapsed_time++;
      break;
    }
  }
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int 
set_exec_time(int pid, int exec_time)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      if(p->sched_policy >= 0 && p->state == RUNNING){
        release(&ptable.lock);
        return -22;
      }
      p->exec_time = exec_time;
      // cprintf("exec_time of %d : %d is set\n", pid, p->exec_time);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;
}

int 
set_deadline(int pid, int deadline)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->deadline = p->arrival_time + deadline;
      p->period = deadline;
      // cprintf("deadline of %d : %d is set\n", pid, p->deadline);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;
}


int 
gcd(int a, int b) 
{
  if (b == 0)
    return a;
  return gcd(b, a % b);
}

int
lcm(int a, int b)
{
  return (a*b)/gcd(a,b);
}

int 
calc_lcm(int periods[], int n)
{
  int i;
  int res = 1;
  for(i = 0; i < n; i++)
    if(periods[i] != -1)
      res = lcm(res, periods[i]);
  return res;
}

// int
// schedulability_test(int sched_policy, int periods[])
// {
//   int n = NPROC;
//   // calculate the lcm of the periods
//   int LCM = calc_lcm(periods, n);  

//   struct proc *p;
//   int sum = 0;
//   acquire(&ptable.lock);
//   for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
  
//     if(sched_policy == 0){
//       if(p->pid != 1 && p->pid != 2){
//         sum += p->exec_time*LCM/p->deadline;
//       }
//     }
//     else if(sched_policy == 1){
//       if(p->pid != 1 && p->pid != 2){
//         sum += p->exec_time*p->rate;
//       }
//     }
//   }
//   release(&ptable.lock);
//   if(sched_policy == 0){
//     if (sum > LCM )
//       return -22;
//     else
//       return 0;
//   }
//   else if (sched_policy == 1){
//     if (sum > 83)
//       return -22; 
//     else{
//       return 0;
//     }
//   }
//   else{
//     return 0;
//   }
// }


int 
set_sched_policy(int pid, int sched_policy)
{
  struct proc *p;
  if(sched_policy != 0 && sched_policy != 1)
    return -22;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->pid == pid){
      struct proc *p1;
      int sum = 0;
      if(sched_policy == 0){
        int periods[NPROC];
        // cprintf("entered for loop");
        int i = 0;
        for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++){
          periods[i] = p1->period;
          i++;
        }
        int lcm = calc_lcm(periods, NPROC);
        struct proc *p2;
        for(p2 = ptable.proc; p2 < &ptable.proc[NPROC]; p2++){
          if(p1->exec_time != -1 && p1->period != -1){
            sum += (p1->exec_time*lcm)/p1->period;
          }
        }
        if (sum <= lcm){
          p->sched_policy = sched_policy;
          release(&ptable.lock);
          // cprintf("sched policy of pid %d : %d is set\n", pid, sched_policy);
          return 0;
        }
        else{
          p->state = UNUSED;
          release(&ptable.lock);
          return -22;
        }
      }
      else if(sched_policy == 1){
        for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++){
          if(p1->exec_time != -1 && p1->rate != -1){
            sum += p1->exec_time*p1->rate;
          }
        }
        if (sum <= 83){
          p->sched_policy = sched_policy;
          release(&ptable.lock);
          // cprintf("sched policy of pid %d : %d is set\n", pid, sched_policy);
          return 0;
        }
        else{
          p->state = UNUSED;
          release(&ptable.lock);
          return -22;
        }
        
      }
    }
  }
  release(&ptable.lock);
  return -22;
}

int 
set_rate(int pid, int rate)
{
  struct proc *p;
  float xval = (3*(30-rate)/29.0);
  int val = (int)xval;

  if ((30-rate)%29!=0){
    val++;
  }
  if (val<1){
    val=1;
  }
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    { 
      p->weight = val;
      p->rate = rate;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;
}
// int 
// set_rate(int pid, int rate)
// {
//   struct proc *p;

//   acquire(&ptable.lock);
//   for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//     if(p->pid == pid){
//       p->rate = rate;
//       cprintf("rate of %d : %d is set\n", pid, p->rate);
//       release(&ptable.lock);
//       return 0;
//     }
//   }
//   release(&ptable.lock);
//   return -22;
// }