// Capture the ABI before a C prologue changes SP, LR or argument registers.
// Single aligned stores also work while the entry memory is Device memory.
.text
.global kentry
kentry:
    adrp x4, entryState
    add x4, x4, :lo12:entryState
    str x0, [x4, #0]
    str x1, [x4, #8]
    str x2, [x4, #16]
    str x3, [x4, #24]
    mov x5, sp
    str x5, [x4, #32]
    str x29, [x4, #40]
    str x30, [x4, #48]
    str x18, [x4, #56]
    mrs x5, CurrentEL
    str x5, [x4, #64]
    mrs x5, DAIF
    str x5, [x4, #72]
    mrs x5, SPSel
    str x5, [x4, #80]
    mrs x5, SCTLR_EL1
    str x5, [x4, #88]
    mrs x5, VBAR_EL1
    str x5, [x4, #96]
    mrs x5, CPACR_EL1
    str x5, [x4, #104]
    mrs x5, CNTKCTL_EL1
    str x5, [x4, #112]
    mrs x5, CNTP_CTL_EL0
    str x5, [x4, #120]
    mrs x5, CNTV_CTL_EL0
    str x5, [x4, #128]
    mrs x5, TPIDR_EL1
    str x5, [x4, #136]
    mrs x5, TPIDR_EL0
    str x5, [x4, #144]
    mrs x5, TPIDRRO_EL0
    str x5, [x4, #152]
    mrs x5, CNTFRQ_EL0
    str x5, [x4, #160]
    mrs x5, CNTPCT_EL0
    str x5, [x4, #168]
    mrs x5, CNTVCT_EL0
    str x5, [x4, #176]
    mrs x5, TTBR0_EL1
    str x5, [x4, #184]
    mrs x5, TTBR1_EL1
    str x5, [x4, #192]
    mrs x5, TCR_EL1
    str x5, [x4, #200]
    mrs x5, SP_EL0
    str x5, [x4, #208]
    adr x5, kentry
    str x5, [x4, #216]
    mov x1, x4
    bl VerifyHandoff
    // This intentional exception checks that VBAR_EL1 reaches retained stop
    // vectors. x0 holds the result so GDB can distinguish it from an early fault.
    brk #0x42
    b .

.bss
.balign 8
.global entryState
entryState:
    .space 224
