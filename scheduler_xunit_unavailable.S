// Kernel joins/signals are forbidden in the cooperative pool-control path.
.text
.macro unavailable name
.globl \name
\name:
    brk #0
.endm
unavailable ThreadsJoin
unavailable ThreadsSignal
unavailable ThreadsCurrentId
unavailable ThreadParametersInitialize
unavailable Syscall_ThreadCreate
unavailable Syscall_ThreadExit
unavailable syscall1
unavailable syscall4
unavailable OSSystemQuery
unavailable OsErrToErrNo
unavailable MutexInitialize
unavailable hashtable_construct
unavailable __usched_startup
unavailable __usched_init
unavailable __usched_destroy
unavailable __usched_job_has_exit
unavailable usched_job_queue
unavailable usched_yield
unavailable usched_wait
unavailable usched_timedwait
unavailable __cxa_threadinitialize
unavailable __cxa_threadfinalize
unavailable __crt_environment
unavailable calloc
unavailable strdup
unavailable SHMBuffer
unavailable SHMCreate
unavailable _assert_panic
