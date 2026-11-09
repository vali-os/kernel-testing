// Standalone privileged probe: QEMU enters this ELF at EL1. No runtime or
// stack is needed. GDB checks the production function's operand and state.
.text
.globl CacheProbeStart
CacheProbeStart:
    adr x0, CacheProbeFailed
    msr vbar_el1, x0
    mov x0, #2
    msr csselr_el1, x0
    mov x18, #0x1818
    mov x19, #0x1919
    mov x20, #0x2020
    // Leave IRQ masked but FIQ/debug/SError unmasked to detect an accidental
    // restore of all-masked or all-enabled state. No devices are running.
    mov x0, #0x80
    msr daif, x0
    isb
.globl CacheProbeReady
CacheProbeReady:
    bl Arm64FlushCpuCaches
.globl CacheProbeDone
CacheProbeDone:
    b CacheProbeDone
.balign 2048
.globl CacheProbeFailed
CacheProbeFailed:
    b CacheProbeFailed
