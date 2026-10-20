# Interrupt allocation tests

The kernel allocation API is in `kernel/include/interrupts_allocation.h`.
It owns groups of reservations, serializes changes, checks every binding before
opening one shared dispatch gate, and retains ownership when teardown fails.
Message data is a fixed-width 64-bit address and 32-bit value, separate from
allocation indices and descriptor handles. MSI allocates one message; MSI-X
allocates independent vectors. MSI multi-message allocation is deferred.

The x86 backend uses physical, fixed, edge-triggered APIC delivery to the BSP.
It shares the existing 16-entry software-vector pool with kernel consumers;
available capacity may therefore be less than 16. Unsupported APIC destinations
and ARM64 message allocation are rejected. Affinity, CPU hotplug, interrupt
remapping, PCI capability discovery, PCI message/table programming and device
authorization are not implemented here. DeviceId is metadata, not authority.
The public driver syscall remains INTx-only until those PCI pieces are connected.

Owners must stop hardware messages and finish any platform-specific draining
before release/reuse. Closing the kernel dispatch gate does not mask a PCI device.

## Host and architecture tests

On Linux, run `testing/interrupt_registration_test.sh` for ASan/UBSan coverage
and `testing/interrupt_architecture_test.sh` with the Vali cross-compiler.

On Windows, with ValiCC and the Visual Studio native linker libraries installed:

```powershell
./testing/interrupt_registration_test.ps1
./testing/interrupt_architecture_test.ps1
./testing/smp_boot_test.ps1
```

The host runner defaults to ARM64 Windows. Use `-Target x86_64-pc-windows-msvc`
for x64. `-Compiler` selects a different Clang. `-AddressSanitizer` enables ASan
when that compiler supplies the Windows runtime; put its runtime DLL directory
on PATH before running. Windows-only fail-fast service stubs satisfy references
from discarded, unrelated syscall sections; interrupt logic is the real source.

Coverage includes malformed requests, ownership, copy-out rollback, every group
allocation failure, shared INTx, partial binding/unmapping, delivery suppression
during group enable, rollback of partial enable, message-vector exhaustion,
single-message MSI limits, unsupported-platform fallback, and vector reuse.
The Windows runner additionally performs 1,000 allocations/releases across four
concurrent threads, checking for duplicate live vector claims and leaked slots.

The SMP host suite checks trampoline allocation/mapping failures, parameter
publication, delayed AP startup and timeouts, and independent kernel memory-space
handles and cleanup. Its futex test races a remote wake against waiter insertion
30 times, including the async-fork recheck. The race test fails against the old
unserialized code. It also assembles both i386 and amd64 SIPI pages and verifies
their size and parameter trailer. The SMP host tests use the x64 Windows target.

Clock regressions cover reference selection before calibration, all RTC rates,
retaining the RTC when no invariant TSC is available, quantized frequencies,
exact fractional timestamps (including overflow boundaries), TSC calibration,
and APIC deadline rounding and the idle zero-deadline sentinel. Memory-sync tests
run the real shootdown and spinlock code across four host threads: IRQ-disabled
lock waiters acknowledge requests, late IPIs are harmless, and 400 concurrent
shootdowns complete. Separate cases check failed delivery, missing acknowledgements,
and synchronization before physical-page reuse. Event-queue tests cover partial
splices, requeueing consumed nodes, pending counts and output capacity.

APIC IPI delivery polls the busy bit at most 1,000,000 times per wait, with a
CPU pause between reads. This is an iteration budget, not a wall-clock timeout:
RTC/PIT clocks cannot advance on the BSP while the sender has interrupts disabled.
The IPI regression uses a frozen clock and covers immediate/transient completion,
completion on the final permitted read, and stuck delivery before and after
writing the ICR. It checks `OS_ETIMEOUT`, xAPIC/x2APIC write ordering, and restoration
of both initially enabled and initially disabled interrupt states.

Negative controls using the original boot-order, RTC-rate, list-splice and
unmap-order implementations fail the corresponding new regressions.

## QEMU delivery tests

Configure `-DVALI_INTERRUPT_SELFTEST=ON`, rebuild the kernel and drivers, and stage
an EFI tree containing `EFI/BOOT/BOOTX64.EFI` and `EFI/vali/{kernel.mos,
phoenix.mos,initrd.mos}`. Compress the kernel with the built `lzss c` tool and
generate the initrd using the built `mkvafs --arch amd64 --compression aplib`.
The CMake option defaults to OFF and only supports x86.

```powershell
python testing/interrupt_qemu_test.py `
  --qemu 'C:/Program Files/qemu/qemu-system-x86_64.exe' `
  --code 'C:/Program Files/qemu/share/edk2-x86_64-code.fd' `
  --vars 'C:/Program Files/qemu/share/edk2-i386-vars.fd' `
  --esp build/interrupt-tests/esp --disk build/windows/disk.img `
  --output build/interrupt-tests/qemu-smp1 --smp 1
```

The runner copies firmware variables, uses snapshot overlays for both disks,
runs QEMU without a visible window, records serial output, and terminates its
own QEMU process. Add `--no-hpet` to exercise alternate timer initialization or
`--smp 4` to exercise multiprocessor startup. A pass requires the kernel self-test
marker, all requested CPUs online, a matching CPU-count/self-test marker, a
userspace process-creation marker and 40 seconds of observation without a kernel
panic or memory-synchronization timeout. `--require InstallBundledApplications`
selects the storage/service-startup milestone used for full boot validation.
`--debug-exceptions` records exception traces and CPU/APIC snapshots through a
localhost QMP socket (which needs local networking permission).
`--debug-state` captures the same CPU/APIC state and stacks without the large
exception trace. Failure captures include the interrupted kernel stack.

The kernel test allocates one MSI and two MSI-X vectors, uses their composed
destination/data to send actual APIC IPIs, checks handler counts while disabled
and enabled, then checks exhaustion and reuse. This validates CPU delivery and
the group lifecycle; it does not demonstrate PCI-generated MSI/MSI-X delivery.
On SMP, it verifies that every discovered CPU is running, then asks each AP in
turn to deliver all three vectors to their composed BSP destination. Serializing
the senders avoids APIC coalescing of identical pending vectors.

## Validation on 2026-10-19

- Full amd64 build passed; interrupt boundary compilation passed for i386,
  amd64 and aarch64.
- Native ARM64 host tests and four-thread stress passed. The x64 ASan run also
  passed without a reported memory error, but the Windows-on-ARM runtime emitted
  three interceptor warnings; this is not equivalent to a clean Linux ASan/UBSan run.
- QEMU single-CPU APIC self-tests passed with HPET on and off. With HPET on,
  userspace reached Virtio block I/O and `InstallBundledApplications`.
- The SMP trampoline now allocates and identity maps firmware-reported free RAM
  below 1 MiB, enters long mode with correctly encoded protected-mode instructions,
  and uses kernel stacks with ABI-correct C entry. Kernel clones return their own
  handles and AP idle threads retain their private memory spaces. Startup waits
  for completion after a second SIPI and keeps memory alive on timeout.
- Repeated four-CPU QEMU boots with HPET passed the APIC self-test, storage I/O
  and `InstallBundledApplications`. The final expanded test passed cross-CPU
  delivery from all three APs. A final single-CPU regression boot also passed.
  Logs: `qemu-final-smp4-crosscpu`, `qemu-smp4-wakeup`, `qemu-smp4-repeat`, and
  `qemu-final-smp1-regression` under `build/interrupt-tests/`.
- SMP boot exposed a futex check/enqueue lost-wakeup race. Serializing the value
  check and waiter insertion with the wake-side lock closes that race, but does
  not eliminate every observed boot stall. Native host regression tests passed; x64 ASan tests also passed
  with the same Windows-on-ARM interceptor warnings noted above.
- The HPET-off clock now selects a reference before calibration, retains the
  RTC when the TSC is non-invariant, and uses the actual 1024 Hz RTC rate.
  RTC update flags no longer resynchronize wall time unless update interrupts
  were requested. Timestamps and APIC deadlines use the calibrated ratios.
- Memory synchronization no longer sleeps while its caller may hold an IRQ
  spinlock. A dedicated IPI/mailbox and polling by contended lock waiters allow
  all CPUs to acknowledge without scheduling or handle lookup. Unmapping waits
  for local and remote invalidation before freeing physical pages. Failed
  delivery or missing acknowledgements are fatal rather than returning with a
  live reference to a stack request; the progress bound is independent of the
  IRQ-driven clock.
- Boot testing also exposed broken partial list-splice links/counts and an event
  node reuse hazard. Splices now detach both boundaries, and handle-set consumers
  detach each event node before clearing the flags that permit IRQ requeueing.
- With these fixes, four-CPU boots passed cross-CPU vector delivery and reached
  `InstallBundledApplications` with both HPET enabled and disabled, followed by
  the runner's 40-second observation. Final-code passing logs include
  `qemu-validated-smp4-nohpet-a` and `qemu-io-stall-hpet` (the latter completed
  successfully despite its diagnostic directory name).
- The final single-CPU HPET-off regression also reached the storage milestone
  and completed observation: `qemu-validated-smp1-nohpet-repeat`. The normal
  amd64 build passed after restoring `VALI_INTERRUPT_SELFTEST=OFF`.
- **Before the storage follow-up below, full boot validation remained intermittent.** Repeats
  `qemu-validated-smp4-hpet`, `qemu-state-smp4-hpet-a`, and
  `qemu-validated-smp4-nohpet-repeat` passed CPU startup and the cross-CPU
  interrupt tests but timed out during storage/service startup. No kernel panic
  or memory-synchronization timeout was reported. The HPET state capture shows
  all four CPUs halted in their idle paths, with no pending APIC interrupt.
  These captures motivated the storage/wakeup investigation below.

## Storage startup follow-up on 2026-10-20

The investigation found two kernel allocator hazards and a separate RPC wait race:

- `storage-capture-2` caught a CPU holding the handle-table lock during table
  growth, entering the heap's contended mutex, then recursively acquiring the
  handle-table lock to look up the mutex owner. Owner spinning now uses bounded
  acquisition attempts without resolving a thread handle.
- Heap cache ownership previously used sleeping mutex acquisition. Heap
  operations are also called while IRQ spinlocks are held and from interrupt
  handlers, so sleeping or preempting the owner is unsafe. Cache operations now
  retain recursive ownership with IRQs disabled, retry without scheduling, and
  service pending TLB shootdowns while contended. IRQ masking also covers
  publication/removal of per-CPU cached pointers. This increases the time spent
  with IRQs disabled during slab growth; allocation latency has not been
  benchmarked.
- Gracht's asynchronous awaiter registered itself, then waited unconditionally.
  A response arriving between those steps could signal before the wait and leave
  it asleep indefinitely. The waiter now checks its completion predicate under
  the same mutex used by notification, and repeats after spurious wakes.
  Notification retains the registry lock through its last awaiter access,
  preventing an ANY waiter from freeing it during another completion. Duplicate
  error responses no longer notify twice.

`smp_boot_test.ps1` now includes `heap_irq_test` and `mutex_owner_test`. Both fail
against the original implementations and pass with these fixes. Coverage
includes forbidden sleeping/handle lookup, contended cache acquisition,
IRQ-state restoration, exhausted/failed allocation, and per-CPU cache paths.
The full host suite and i386/amd64/aarch64 boundary compilation passed. The x64
ASan suite passed with the Windows-on-ARM interceptor limitations described above.

Gracht's `client_await_test` is included in its own CMake tests when static builds
are enabled. Eight cases cover early response, normal wait, spurious wake, ALL,
ANY, duplicate errors, and four concurrent responders. Both concurrent cases
passed 100 repetitions; all eight cases also passed under ASan. The early
response test fails against the original runtime.

```powershell
cmake --build build/gracht-host --target client_await_test client_runtime_test
ctest --test-dir build/gracht-host --output-on-failure -R client_await_
ctest --test-dir build/gracht-host --output-on-failure -R client_await_concurrent --repeat until-fail:100
```

The broader pre-existing RPC suite has one unrelated failure:
`client_runtime_peer` asserts that a valid response should be rejected, although
its in-memory transport provides no peer-mismatch metadata. It fails identically
against the original `HEAD` client implementation. The other six existing
client-runtime cases pass. This test was not changed or excluded from the suite.

The final four-CPU QEMU matrix passed ten consecutive boots: five with HPET on
(`storage-lock-1`, `-3`, `-5`, `-7`, `-9`) and five with HPET off
(`storage-lock-2`, `-4`, `-6`, `-8`, `-10`). Every run brought all four CPUs online,
passed cross-CPU vector delivery, reached `InstallBundledApplications`, and
completed the 40-second observation with no panic or synchronization timeout.
Single-CPU regressions passed with both clock sources as well
(`storage-single-hpet`, `storage-single-rtc`).
These runs use `build/interrupt-tests/storage-esp`; that staged image retains the
self-test kernel while the normal build has `VALI_INTERRUPT_SELFTEST=OFF`.
PCI MSI/MSI-X device programming is still a separate next step.

Local logs are under `build/interrupt-tests/`. The test scripts live in the
`testing` submodule; the RPC fix and its tests live in `librt/libgracht`. Both
submodules must be included alongside the parent repository changes.

## PCI single-message MSI and EDU validation (2026-10-20)

The PCI provider now owns a single MSI reservation per function. This adds
single-message MSI through the normal libddk API; PCI MSI-X table programming
and MSI multi-message remain deferred. No device authorization or capability
system was introduced. The allocation handle in a PCI request is a lifecycle
identity, not a permission grant. Existing kernel memory-space ownership checks
are unchanged.

The implementation is split between:

- `services/deviced/bus/pci/msi.c` and `msi.h`: bounded conventional-capability
  discovery, configuration snapshots, masked preparation, enable, disable and
  restore. Both 32-bit and 64-bit address layouts, with and without optional
  per-vector masking, use their correct data/mask offsets. Multiple Message
  Enable is always zero while our reservation is prepared or enabled.
- The PCI device provider: serializes operations under the existing PCI lock,
  observes the ACPI NO_MSI policy, rejects MSI on unsupported architectures,
  preserves INTx Disable during ordinary bus-control requests and rejects raw
  configuration writes while a reservation is active. A reservation also keeps
  host destruction from discarding its saved state.
- `librt/libddk/interrupt.c`: intersects requested modes with provider support,
  reserves the kernel vector, and coordinates PCI and kernel cleanup. The driver
  still supplies device-specific source quiescence, MMIO and bus-master setup.
- The interrupt allocation syscall: exposes single-message MSI alongside INTx
  and returns the composed message in the allocation result. MSI-X remains
  available only through the existing kernel allocation interfaces.

The device protocol now transports `BusDevice.IsPci`. Previously the conversion
discarded that field, which made the first EDU boot reject MSI as unsupported.
The protocol and allocation-result layouts changed: rebuild the kernel, runtime,
services and modules together.

### Ordering and failure contracts

Allocate snapshots the PCI state once and programs the message with both MSI and
INTx delivery disabled. The snapshot is published before the first write so an
incomplete preparation remains recoverable. The configuration engine verifies
each write by reading the same register. This detects failed programming and
orders writes; it is not a guarantee that interrupt messages have drained.

Enable binds all resources and opens kernel dispatch before enabling PCI MSI.
Disable first disables PCI MSI, then closes kernel dispatch, waits for running
handlers and releases their mappings. The driver must first stop device-specific
producers such as queues or DMA. Destroy restores the original message and mask
registers and original INTx Disable bit, while preserving unrelated command bits.
It does not restore an old MSI enable bit or take over already enabled MSI/MSI-X.

Operations can be retried with the same allocation identity after an uncertain
RPC result. In particular, failure to stop PCI delivery leaves kernel dispatch
and mapped resources intact. A later Enable reconciles incomplete Disable state
before reporting success. If MSI Destroy fails, finish Destroy before trying a
new allocation. Even a failed Allocate may retain `State` when rollback fails;
the caller must retain the object and retry Destroy. Transport/decode failures
remain `OS_EPROTOCOL`, distinct from a service response such as `OS_EBUSY`.

Released non-kernel message vectors that have ever been enabled remain reserved
as disabled, unbound descriptors until reboot. The allocation object and driver
mappings are freed; the descriptor retains a permanent closed delivery gate.
Late messages therefore cannot invoke another device's newly allocated handler.
Never-enabled allocations can be reused, and Disable/Enable consumes no new
vectors. Repeated Destroy/Allocate consumes finite vector capacity (the current
x86 backend offers 16 candidate slots, shared with other reservations). Platform
message draining and automatic cleanup after a driver/service crash are future
work; the current reservation protocol does not claim to solve either.

### Host regression tests

```powershell
./testing/pci_msi_test.ps1
./testing/interrupt_registration_test.ps1
./testing/interrupt_architecture_test.ps1
```

The RPC wrapper test needs the generated protocol headers from `build/windows`.
On Linux, build/generate the protocols first, then run
`VALI_PROTOCOLS=/path/to/build/protocols ./testing/pci_msi_test.sh`.

`pci_msi_test.c` exercises the real configuration engine with four MSI layouts
and 116 injected failures at individual setup, enable, disable and restore
writes. Each is tested both as a dropped write and as an applied write with a
failed readback. Cleanup retains the original snapshot and a subsequent Release
restores it. Additional cases cover capability cycles, overlap, truncation,
duplicate MSI, already enabled modes, unsupported message widths, competing
reservations, idempotent operations and preserving unrelated command bits.

`pci_msi_ddk_test.c` runs the actual libddk wrapper and configuration engine
together. It covers lost responses before/after PCI operations, failed kernel
bind/enable/disable/unbind/release, persistent stop failures, rollback retained
after failed Allocate, failed Enable followed by failed cleanup, re-enable after
an uncertain Disable, INTx fallback, unsupported counts and repeated lifecycles.
`pci_msi_rpc_test.c` checks send, await, decode and response-length failures in
the actual ioctl wrapper. The kernel harness additionally verifies that retired
vectors suppress late delivery and are not reused, while never-enabled vectors
remain reusable.

These tests passed natively on Windows ARM64. The MSI tests and kernel harness
also passed under x64 AddressSanitizer, with the same Windows-on-ARM interceptor
warnings documented earlier; that run is not a clean Linux ASan/UBSan result.
Compilation of the interrupt boundary passed for i386, amd64 and aarch64,
including the new PCI engine/provider and EDU module. ARM64 reports MSI
unsupported at runtime.

### Real PCI delivery in QEMU

The optional `modules/edu` driver binds QEMU's `1234:11e8` educational device.
It is enabled with `VALI_EDU_SELFTEST=ON` and omitted by default. It uses
`DeviceInterruptAllocate(..., 1, 1, INTERRUPT_STRATEGY_MSI, ...)`; it cannot pass
by silently falling back to INTx. No kernel-only message injection substitutes
for the EDU device's interrupt-raise register.

Each boot checks 192 acknowledged MSI messages across three reservations and six
enable/disable cycles. It raises and clears causes while disabled, verifies no
handler delivery, checks bus control preserves INTx Disable and rejects raw
configuration writes, and repeats Destroy before releasing the MMIO mapping.
Bus mastering must be enabled for EDU's MSI memory writes; an early diagnostic
boot omitted that bit and timed out with zero deliveries.

```powershell
cmake -S . -B build/windows -DVALI_INTERRUPT_SELFTEST=ON -DVALI_EDU_SELFTEST=ON
cmake --build build/windows --parallel 8
# Stage kernel.mos, phoenix.mos and an initrd rebuilt from deploy/hdd/system.
python testing/interrupt_qemu_test.py `
  --qemu "C:/Program Files/qemu/qemu-system-x86_64.exe" `
  --code "C:/Program Files/qemu/share/edk2-x86_64-code.fd" `
  --vars "C:/Program Files/qemu/share/edk2-i386-vars.fd" `
  --esp build/interrupt-tests/msi-esp --disk build/windows/disk.img `
  --output build/interrupt-tests/msi-example --smp 4 --edu `
  --require InstallBundledApplications --timeout 90
# Add --no-hpet for the RTC clock path.
```

The runner requires both the APIC allocation self-test and the EDU PASS marker,
all four CPUs online, the userspace storage-startup marker and a 40-second
observation. Any EDU failure, kernel panic or synchronization timeout fails the
run. Disks and firmware variables use per-run copies/snapshots.

The final image passed six consecutive four-CPU boots: HPET on in `msi-final-1`,
`msi-final-3` and `msi-final-5`, and HPET off in `msi-final-2`, `msi-final-4` and
`msi-final-6`. All six passed the APIC and EDU checks, reached
`InstallBundledApplications no bundled apps!`, and completed observation without
a panic or synchronization timeout. Each boot also exercised the existing
virtio-blk INTx storage path. This validates single-message PCI MSI; the separate
kernel MSI-X self-test still does not validate PCI MSI-X table programming.

The tested image is retained in `build/interrupt-tests/msi-esp`; its SHA-256
manifest is `build/interrupt-tests/msi-image-hashes.json`. Serial logs and exact
QEMU commands are in the six run directories. Early diagnostic runs are retained
as `msi-hpet-1` (missing IsPci conversion), `msi-hpet-2` (bus mastering disabled)
and `msi-hpet-3` (first successful delivery, before the final test additions).

After validation, restore `VALI_INTERRUPT_SELFTEST=OFF` and
`VALI_EDU_SELFTEST=OFF` for normal builds. When reusing a deployment tree that
previously included EDU, also remove its generated `modules/edu-msi.dll` and
`modules/edu-msi.yaml` before packaging a normal initrd. The staged test image
does not need to change.
