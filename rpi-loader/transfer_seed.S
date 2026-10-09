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
