// Capture the entry state before the C prologue changes the stack/registers.
.text
.globl kentry
kentry:
    mov x9, sp
    sub sp, sp, #80
    stp x1, x2, [sp]
    stp x3, x18, [sp, #16]
    stp x29, x30, [sp, #32]
    mrs x10, vbar_el1
    stp x9, x10, [sp, #48]
    mrs x9, cpacr_el1
    str x9, [sp, #64]
    mrs x9, spsel
    str x9, [sp, #72]
    mov x1, sp
    bl FixtureMain
1:  wfe
    b 1b
