// These services are outside the scheduler/TLS fixture. Trap on any use rather
// than silently succeeding if the fixture unexpectedly enters one of them.
.text
.macro unavailable name
.globl \name
\name:
    brk #0
.endm
unavailable OSHandleCreate
unavailable exit
unavailable MutexInitialize
unavailable _assert_panic
unavailable OSNotificationQueueCreate
unavailable OSNotificationQueueCtrl
unavailable OSHandleDestroy
unavailable __usched_cond_notify_job
unavailable __usched_job_notify
unavailable __usched_mtx_notify_job
unavailable OSNotificationQueueWait
unavailable __crt_environment
unavailable calloc
unavailable strdup
unavailable SHMBuffer
unavailable SHMCreate
