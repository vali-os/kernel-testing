// Test-only firmware shim, injected into QEMU RAM before wrapper entry.
// x16 is the real wrapper entry and x17 selects EL1 (1) or EL2 (0).
// System registers must be seeded by instructions: QEMU's GDB interface does
// not reliably write them or perform an EL transition by assigning CPSR.
.text
    mov x9, #0x80000000
    msr HCR_EL2, x9
    isb
    mov x9, #0x1111
    msr TPIDR_EL1, x9
    mov x9, #0x2222
    msr TPIDR_EL0, x9
    mov x9, #0x3333
    msr TPIDRRO_EL0, x9
    mov x9, #0x303
    msr CNTKCTL_EL1, x9
    mov x9, #0x300000
    msr CPACR_EL1, x9
    mov x9, #0x4444
    msr SP_EL0, x9
    mov x9, #0x1000
    msr SCTLR_EL2, x9
    isb
    cbz x17, .Lel2

    mov x9, #3
    msr CNTHCTL_EL2, x9
    mov x9, #0x33ff
    msr CPTR_EL2, x9
    msr MDCR_EL2, xzr
    msr CNTVOFF_EL2, xzr
    mov x9, #0x1800
    movk x9, #0x30d0, lsl #16
    msr SCTLR_EL1, x9
    mov x9, #0x3c5
    msr SPSR_EL2, x9
    msr ELR_EL2, x16
    isb
    eret

.Lel2:
    // Wrong CPU IDs and an MDCR_EL2 with PMU/debug traps and HPMN=1 check that
    // the wrapper replaces inherited values instead of relying on reset state.
    // QEMU's GDB interface does not expose MIDR_EL1 or MPIDR_EL1, so the real
    // IDs (as read at EL2) are stored at 0x70200 and 0x70208 for comparison.
    mov x10, #0x0200
    movk x10, #0x7, lsl #16
    mrs x9, MIDR_EL1
    str x9, [x10]
    mrs x9, MPIDR_EL1
    str x9, [x10, #8]
    mov x9, #0xdead
    msr VPIDR_EL2, x9
    mov x9, #0xbeef
    msr VMPIDR_EL2, x9
    mov x9, #0x2e1
    msr MDCR_EL2, x9
    mov x9, #0x39
    movk x9, #0x8000, lsl #16
    msr HCR_EL2, x9
    mov x9, #0x37ff
    movk x9, #0x8010, lsl #16
    msr CPTR_EL2, x9
    msr CNTHCTL_EL2, xzr
    mov x9, #0x5678
    movk x9, #0x1234, lsl #16
    msr CNTVOFF_EL2, x9
    isb
    br x16
