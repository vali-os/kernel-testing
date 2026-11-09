// Synthetic firmware state for testing the production UEFI trampoline. QEMU
// enters at EL2; the debugger selects E2H/TGE in x20 before executing SeedStart.
.text
.global SeedStart
SeedStart:
    msr daifset, #15
    adr x0, SeedVectors
    msr vbar_el2, x0
    ldr x0, =StackTop
    mov sp, x0
    mov x0, #0x80000000
    orr x0, x0, x20
    msr hcr_el2, x0
    isb

    // Use 1 GiB blocks with a 4 KiB L1 table. RAM is identity mapped at
    // 0x40000000; 0x80000000 deliberately aliases RAM, and 0xc0000000 faults.
    // These mappings exercise Arm64Identity with real translations under VHE.
    adr x0, SeedTable
    mov x1, #0x401
    str x1, [x0]
    mov x1, #0x705
    movk x1, #0x4000, lsl #16
    stp x1, x1, [x0, #8]
    msr ttbr0_el2, x0
    mov x1, #0xff00 // Attr0 device nGnRnE, Attr1 normal write-back.
    msr mair_el2, x1
    mov x1, #0x3519
    movk x1, #0x8082, lsl #16 // Non-VHE: PS=40, T0SZ=25, required RES1.
    tbz x20, #34, 1f
    movk x1, #0x8099, lsl #16 // VHE: EPD1, T1SZ=25, TG1=4 KiB.
    movk x1, #2, lsl #32     // VHE: IPS=40, inner-shareable WB walks.
1:  msr tcr_el2, x1
    dsb sy
    isb
    tlbi alle2
    dsb sy
    isb
    mov x1, #0x1805
    movk x1, #0x30d0, lsl #16
    msr sctlr_el2, x1
    isb

    // Seed lower-EL controls with values the trampoline must replace. EL1 is
    // not executing yet, so its MMU can be marked on without installing tables.
    adr x16, SeedVectors
    mov x1, #0x1805
    movk x1, #0x30d0, lsl #16
    mov x2, #0x300000
    mov x3, #0x303
    mov x4, #3 // Enabled but masked timers must become disabled.
    tbz x20, #34, 2f
    msr S3_5_C1_C0_0, x1   // SCTLR_EL12
    msr S3_5_C1_C0_2, x2   // CPACR_EL12
    msr S3_5_C14_C1_0, x3  // CNTKCTL_EL12
    msr S3_5_C14_C2_1, x4  // CNTP_CTL_EL02
    msr S3_5_C14_C3_1, x4  // CNTV_CTL_EL02
    msr S3_5_C12_C0_0, x16 // VBAR_EL12
    msr S3_4_C14_C3_1, x4  // CNTHV_CTL_EL2
    b 3f
2:  msr sctlr_el1, x1
    msr cpacr_el1, x2
    msr cntkctl_el1, x3
    msr cntp_ctl_el0, x4
    msr cntv_ctl_el0, x4
    msr vbar_el1, x16
3:  msr cnthp_ctl_el2, x4
    // Deny EL1 ICC access until the production trampoline enables it. Keep
    // EL2 SRE set so this seed can program ICH_HCR's virtual interrupt traps.
    mov x4, #7
    msr icc_sre_el2, x4
    isb
    mov x4, #0x1f
    lsl x4, x4, #10
    msr ich_hcr_el2, x4
    msr cnthctl_el2, xzr // EL1 counter/timer accesses initially trap.
    mov x4, #0x9999
    msr cntvoff_el2, x4
    // The negative case has no usable counter frequency. No firmware control
    // is changed by the production check; it must reject this before handoff.
    cbz x21, 4f
    msr cntfrq_el0, xzr
4:  isb
.global SeedReady
SeedReady:
    bl CheckFirmwareState
    cbnz x0, SeedFailed
    mov x0, #0x5678
    movk x0, #0x1234, lsl #16
    adr x1, KernelEntry
    ldr x2, =StackTop
    b Arm64Enter

.global KernelEntry
KernelEntry:
    // These must execute at EL1 without trapping back to the loader vectors.
    // Register inspection alone would miss an incorrect CNTHCTL encoding.
    mrs x9, cntpct_el0
    mrs x10, cntvct_el0
    mrs x11, cntfrq_el0
    mrs x12, cntp_ctl_el0
    mrs x13, cntv_ctl_el0
    mrs x14, cntkctl_el1
    mov x15, #0x1000
    msr cntp_tval_el0, x15
    msr cntv_tval_el0, x15
    mov x15, #3
    msr cntp_ctl_el0, x15
    msr cntv_ctl_el0, x15
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr

    // Execute actual ICC accesses after the EL2 transition. Merely reading
    // ICC_SRE_EL2 from GDB would not detect trapping lower-EL register accesses.
    mrs x15, icc_sre_el1
    orr x15, x15, #7
    msr icc_sre_el1, x15
    isb
    mrs x15, icc_ctlr_el1
    msr icc_pmr_el1, xzr
    isb

    // FP is disabled on entry by CPACR_EL1. After EL1 enables it, CPTR_EL2
    // must allow the access rather than sending an exception to dead firmware.
    mov x15, #0x300000
    msr cpacr_el1, x15
    isb
    .arch_extension fp
    fmov d0, xzr
    msr cpacr_el1, xzr
    isb
.global ProbeDone
ProbeDone:
    ret // The production trampoline supplies a safe LR halt address.
.global SeedFailed
SeedFailed:
    wfe
    b SeedFailed
.balign 2048
SeedVectors:
.rept 16
    b SeedFailed
    .balign 128
.endr
.bss
.balign 4096
SeedTable:
    .skip 4096
.balign 16
    .skip 16384
.global StackTop
StackTop:
