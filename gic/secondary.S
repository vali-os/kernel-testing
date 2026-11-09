// Exercise the real secondary entry with MMU off, including VHE and denied
// lower-EL ICC access. GDB selects HCR.E2H/TGE in x20 before the first instruction.
.text
.global SeedStart
SeedStart:
    msr daifset, #15
    orr x0, x20, #0x80000000
    msr hcr_el2, x0
    isb
    mov x0, #7
    msr icc_sre_el2, x0 // SRE enabled at EL2, but lower-EL access denied.
    isb
    mov x0, #0x1f
    lsl x0, x0, #10
    msr ich_hcr_el2, x0 // Seed ICC traps that the real entry must remove.
    msr cnthctl_el2, xzr
    isb
    b Arm64SecondaryEntry

.global Arm64SecondaryStart
Arm64SecondaryStart:
    // The real entry installed EL1 mappings, vectors, FP access and the stack.
    // Execute accesses here so trapped instructions cannot pass by inspection.
    mrs x9, cntpct_el0
    mrs x10, icc_sre_el1
    orr x10, x10, #7
    msr icc_sre_el1, x10
    isb
    mrs x11, icc_ctlr_el1
    msr icc_pmr_el1, xzr
    .arch_extension fp
    fmov d0, xzr
.global ProbeDone
ProbeDone:
    b ProbeDone

.balign 2048
.global Arm64Vectors
Arm64Vectors:
.rept 16
    b Failed
    .balign 128
.endr
.global Failed
Failed:
    b Failed

.data
.balign 128
.global g_arm64SecondaryState
g_arm64SecondaryState:
    .quad 0, StackTop, Root, Root
    // T0SZ/T1SZ=16, WBWA inner-shareable walks, 4 KiB granules, IPS=40 bits.
    .quad 0x2b5103510, 0xff, 0, 1
    .quad 3 // GIC version.
    .zero 56
    .zero 255 * 128
.balign 4096
Root:
    .quad Level1 + 3
    .zero 4096 - 8
Level1:
    .quad 0
    .quad 0x40000701 // 1 GiB Normal RAM identity block, AF=1, inner-shareable.
    .zero 4096 - 16
.bss
.balign 4096
    .zero 16384
.global StackTop
StackTop:
