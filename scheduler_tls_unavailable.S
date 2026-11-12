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
unavailable OSNotificationQueueWait
unavailable __crt_environment
unavailable calloc
unavailable strdup
unavailable SHMBuffer
unavailable SHMCreate
