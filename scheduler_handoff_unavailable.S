// Fail if the deterministic handoff fixture enters an unmodelled OS service.
.text
.macro unavailable name
.globl \name
\name:
    brk #0
.endm
unavailable exit
unavailable _Exit
unavailable _assert_panic
unavailable OSNotificationQueueWait
unavailable strdup
unavailable SHMBuffer
unavailable SHMCreate
unavailable SystemDebug
