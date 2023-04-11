# Defining Real-time Scheduling Algorithms in xv6 
For Problem-statement, please refer to *OS-A2_xv6.pdf*
we were asked to implement 2 scheduling algorithms in xv6 taking only 1 CPU, namely:
1. EDF (Earliest Deadline First)
2. RMA (Rate Monotonic Algorithm)
By default, the scheduling alogorithm used in xv6 is Round Robin.

## EDF
The implementation of EDF is done in the file *proc.c* in the function *scheduler()*. If the *policy = 1*, the scheduler function implements the EDF algorithm. The function *scheduler()* is called by the *trap()* function in the file *trap.c* when the timer interrupt occurs. The *scheduler()* function is responsible for selecting the next process to be run.

The EDF algorithm is implemented by sorting the processes in the *ptable* according to their deadlines. The process with the earliest deadline is selected to be run. The *ptable* is a global variable defined in the file *proc.c*.

## RMA
The implementation of RMA is done in the file *proc.c* in the function *scheduler()*. The function *scheduler()* is called by the *trap()* function in the file *trap.c* when the timer interrupt occurs. The *scheduler()* function is responsible for selecting the next process to be run.

The RMA algorithm is implemented by sorting the processes in the *ptable* according to their periods. The process with the lowest period is selected to be run. The *ptable* is a global variable defined in the file *proc.c*.

## Implementation
I have changed in these files:
1. syscall.c
2. syscall.h
3. sysproc.c
4. proc.h
5. proc.c
6. trap.c
8. Makefile
9. user.h
10. usys.S

## How to run
1. clone the repository
2. Build xv6 with the command: *make*
3. Run xv6 with the command: *make qemu*
4. Run the test program with command: *a<test_no>*