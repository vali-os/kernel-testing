# ARM64 ABI preflight

Run independently of the kernel build; output must be a new directory:

```sh
python3 testing/arm64-abi/probe.py \
  --clang /usr/local/valicc/bin/clang \
  --tools /home/philip/Projects/llvm-project/vali-port/build-23/bin \
  --out /tmp/vali-arm64-evidence
```

Exit 1 means the Vali compiler gate is blocked. Each invocation, diagnostic,
macro dump, IR model, object inspection and disassembly is saved. Controls use
**the same compiler** with explicit Windows and ELF targets. A successful control
never counts as a successful Vali build. No host headers or libraries are used;
image links pass `/nodefaultlib` and explicit objects/import libraries only.

The minimal linked pair exercises imports, exported code/data, and an absolute
exported pointer. `rebase.py` verifies ARM64 and PE32+, maps sections, applies
DIR64 relocations at two alternative bases and checks the exported pointer.
This is an offline model, not execution through Vali's loader.

`features.c` is deliberately separate: aggregate returns, FP, varargs, TLS,
constructor sections, a large stack frame and 128-bit division expose the
runtime dependencies. Inspection does not claim that these features work at
runtime. `escape` is an intentional test harness dependency; all other undefined
symbols need real compiler-rt/CRT implementations, never empty stubs. The kernel
probe uses only general registers; all ARM probes reserve x18, target ARMv8-A and
disable outline atomics. Inspect exclusive-load/store loops and reject LSE,
SIMD/FP in the kernel, and `__aarch64_*` atomic helper dependencies.

An additional standalone CMake fixture provides Clang `.S` assembly and a small
compiler-rt archive for the division probe:

```sh
cmake -S testing/arm64-abi -B /tmp/vali-arm64-cmake \
  -DCMAKE_C_COMPILER=/usr/local/valicc/bin/clang \
  -DCMAKE_C_COMPILER_TARGET=aarch64-uml-vali \
  -DCOMPILER_RT_SOURCE=/home/philip/Projects/llvm-project/compiler-rt
cmake --build /tmp/vali-arm64-cmake
```

This is expected to fail until the compiler target is implemented. This archive
is a dependency fixture, not a complete or validated AArch64 Vali runtime.

Runtime acceptance still requires a full-feature DLL and executable linked with
real CRT and compiler-rt, then loaded at nonpreferred bases on Vali ARM64:

- Check imported data and calls, pair/large/HFA arguments and returns across the
  DLL boundary, `fp(1.25, 2.5f) == 3.75`, and `call_variadic() == 3.75`.
- Assert constructors run exactly once before entry. Access initialized TLS in
  two threads, modify one copy, verify isolation and new-thread initialization;
  repeat with dynamic module load/unload and TLS callbacks.
- Cross multiple guard pages with the large frame and verify stack overflow
  handling. Check 128-bit signed/unsigned division and boundary cases.
- Inspect final imports, link map and every archive member. No host library,
  unresolved helper, LSE-only instruction or outline atomic helper is allowed.
- Verify unwind metadata describes the large frame. C++ exceptions remain
  unavailable; do not interpret C constructors as C++ runtime validation.

See [the ABI decision record](../../docs/arm64-abi.md) for decisions and blockers.
