# Interrupt group tests

`kernel/include/interrupts_group.h` defines the kernel interrupt group API.
Each group owns its reservations and serializes changes. Before setting the
shared group enable flag, the kernel checks every binding. Failed teardown leaves
ownership with the caller so cleanup can be retried.

`InterruptGroupCreate` accepts an `InterruptGroupRequest_t` and returns an
`InterruptGroupResult_t`. Binding, delivery control, and release operate on the
group handle. The syscall and DDK allocation verbs remain `ScInterruptAllocate`
and `DeviceInterruptAllocate`; the group naming does not change syscall numbers
or request/result layouts.

Message data has a 64-bit address and a 32-bit value, separate from group
indices and descriptor handles. MSI gets one message. MSI-X gets independent
vectors. Multi-message MSI is unsupported.

The x86 backend sends physical, fixed, edge-triggered APIC interrupts to the BSP.
It shares a 16-entry software-vector pool with kernel consumers, so fewer than
16 slots may be available. Unsupported APIC destinations and ARM64 message
allocation are rejected. The backend has no affinity, CPU hotplug or interrupt
remapping support. PCI discovery and message/table programming belong outside
this API. DeviceId is metadata. There are no device access checks here.

Before the PCI integration described below, the public driver syscall supported
only INTx. Owners must stop hardware messages and finish any platform-specific
draining before releasing or reusing vectors. Closing kernel dispatch leaves the
PCI device itself unmasked.

## Host and architecture tests

On Linux, run `testing/interrupt_registration_test.sh` for ASan/UBSan coverage
and `testing/interrupt_architecture_test.sh` with the Vali cross-compiler.

On Windows, with ValiCC and the Visual Studio native linker libraries installed:

```powershell
./testing/interrupt_registration_test.ps1
./testing/interrupt_architecture_test.ps1
./testing/smp_boot_test.ps1
```

The host runner defaults to ARM64 Windows. For x64, use
`-Target x86_64-pc-windows-msvc`. Choose another Clang with `-Compiler`.
`-AddressSanitizer` enables ASan if that compiler includes the Windows runtime.
Put the runtime DLL directory on PATH before running.

The tests compile the real interrupt code. Windows also needs fail-fast stubs
for unrelated services referenced by discarded syscall sections.

The group tests check malformed requests, ownership and copy-out rollback,
then inject failures at every group allocation. Other cases exercise shared INTx,
partial binding and unmapping, dispatch during group enable, and rollback after a
partial enable. Message-vector tests cover exhaustion, single-message MSI limits,
fallback on unsupported platforms and reuse. On Windows, four concurrent threads
perform 1,000 allocations/releases and check for duplicate live claims and leaked
vector slots.

The SMP host suite uses the x64 Windows target. It checks trampoline allocation
and mapping failures, parameter publication, delayed AP startup and timeouts.
Kernel memory-space clones must return independent handles and clean up their
mappings. The suite also assembles the i386 and amd64 SIPI pages and checks their
size and parameter trailer.

The futex test races a remote wake against waiter insertion 30 times, including
the async-fork recheck. It fails against the old code without serialization.

Clock tests check reference selection before calibration, every RTC rate and
keeping the RTC when no invariant TSC is available. They also cover quantized
frequencies, exact fractional timestamps at overflow boundaries, TSC calibration,
APIC deadline rounding and the idle zero-deadline sentinel.

Memory-sync tests run the real shootdown and spinlock code on four host threads.
Lock waiters with IRQs disabled must acknowledge requests, late IPIs must be
harmless, and all 400 concurrent shootdowns must complete. Separate cases check
failed delivery, missing acknowledgements and synchronization before physical-page
reuse. Event-queue tests check partial splices, requeueing consumed nodes, pending
counts and output capacity.

APIC IPI delivery polls the busy bit at most 1,000,000 times per wait, with a
CPU pause between reads. The limit counts iterations because RTC/PIT clocks
cannot advance on the BSP while the sender has interrupts disabled. With the clock
frozen, the IPI test checks immediate completion, completion after several reads
and completion on the last allowed read. It also tests stuck delivery before and
after the ICR write, checking `OS_ETIMEOUT`, xAPIC/x2APIC write order and restoration
of the caller's interrupt state with IRQs initially enabled and disabled.

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

The runner copies firmware variables and gives both disks snapshot overlays.
QEMU runs without a visible window. The runner records serial output and stops
only the QEMU process it started. Add `--no-hpet` to test the alternate timer
initialization or `--smp 4` to test multiprocessor startup.

A pass needs the kernel self-test marker, every requested CPU online, a matching
CPU-count/self-test marker and a userspace process-creation marker, followed by
40 seconds without a kernel panic or memory-synchronization timeout.
Use `--require InstallBundledApplications` to wait for the storage/service-startup
milestone used in full boot testing.

`--debug-exceptions` records exception traces and CPU/APIC snapshots over a
localhost QMP socket, which needs local networking permission. For CPU/APIC state
and stacks without the large exception trace, use `--debug-state`. Failure
captures include the interrupted kernel stack.

The kernel test allocates one MSI and two MSI-X vectors, uses their composed
destination/data to send APIC IPIs, and checks handler counts with dispatch
disabled and enabled. It then checks exhaustion and reuse. PCI-generated
MSI/MSI-X delivery needs a separate test.

On SMP, every discovered CPU must be running. Each AP then sends all three
vectors to their composed BSP destination, one AP at a time so the APIC cannot
coalesce identical pending vectors from different senders.

## Validation on 2026-10-19

- Full amd64 build passed. Interrupt boundary compilation passed for i386,
  amd64 and aarch64.
- Native ARM64 host tests and four-thread stress passed. The x64 ASan run also
  passed without a reported memory error, but the Windows-on-ARM runtime emitted
  three interceptor warnings. A clean Linux ASan/UBSan run is still needed.
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
  check and waiter insertion with the wake-side lock closes that race. Some boot
  stalls remained. Native host regression tests passed, as did x64 ASan tests
  with the Windows-on-ARM interceptor warnings noted above.
- The HPET-off clock now selects a reference before calibration, retains the
  RTC when the TSC is non-invariant, and uses the actual 1024 Hz RTC rate.
  RTC update flags no longer resynchronize wall time unless update interrupts
  were requested. Timestamps and APIC deadlines use the calibrated ratios.
- Memory synchronization no longer sleeps while its caller may hold an IRQ
  spinlock. A dedicated IPI/mailbox and polling by contended lock waiters allow
  all CPUs to acknowledge without scheduling or handle lookup. Unmapping waits
  for local and remote invalidation before freeing physical pages. Failed
  delivery or missing acknowledgements are fatal because returning would leave
  a live reference to a stack request. The progress limit works independently
  of the IRQ-driven clock.
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
- **Full boot still failed intermittently before the storage follow-up below.** Repeats
  `qemu-validated-smp4-hpet`, `qemu-state-smp4-hpet-a`, and
  `qemu-validated-smp4-nohpet-repeat` passed CPU startup and the cross-CPU
  interrupt tests but timed out during storage/service startup. No kernel panic
  or memory-synchronization timeout was reported. The HPET state capture shows
  all four CPUs halted in their idle paths, with no pending APIC interrupt.
  The storage/wakeup investigation below started from these captures.

## Storage startup follow-up on 2026-10-20

`storage-capture-2` caught a CPU holding the handle-table lock during table growth.
It entered the heap's contended mutex, then tried to acquire the handle-table
lock again to look up the mutex owner. Owner spinning now makes a bounded number
of acquisition attempts without resolving a thread handle.

Heap cache acquisition could also sleep, even though heap calls run from
interrupt handlers and with IRQ spinlocks held. A sleeping or preempted owner
cannot release the cache for an interrupt caller. Cache operations now keep
recursive ownership with IRQs disabled, retry without scheduling and service
pending TLB shootdowns while waiting. IRQs also stay masked while per-CPU cached
pointers are published or removed. Slab growth therefore keeps IRQs disabled
longer. Allocation latency has not been benchmarked.

Gracht had a separate wait race. Its asynchronous awaiter registered itself,
then waited unconditionally. A response between those steps could signal before
the wait and leave it asleep indefinitely. The waiter now checks completion under
the notification mutex and rechecks after spurious wakes. Notification holds the
registry lock through its last awaiter access, so an ANY waiter cannot free the
awaiter during another completion. Duplicate error responses notify only once.

`smp_boot_test.ps1` now includes `heap_irq_test` and `mutex_owner_test`. Both fail
against the original implementations and pass with these fixes. Coverage
includes forbidden sleeping/handle lookup, contended cache acquisition,
IRQ-state restoration, exhausted/failed allocation, and per-CPU cache paths.
The full host suite and i386/amd64/aarch64 boundary compilation passed. The x64
ASan suite passed with the Windows-on-ARM interceptor limitations described above.

Gracht's `client_await_test` is included in its own CMake tests when static builds
are enabled. Eight cases cover early response, normal wait, spurious wake, ALL,
ANY, duplicate errors, and four concurrent responders. Both concurrent cases
passed 100 repetitions. All eight cases also passed under ASan. The early
response test fails against the original runtime.

```powershell
cmake --build build/gracht-host --target client_await_test client_runtime_test
ctest --test-dir build/gracht-host --output-on-failure -R client_await_
ctest --test-dir build/gracht-host --output-on-failure -R client_await_concurrent --repeat until-fail:100
```

The existing RPC suite has one unrelated failure:
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
These runs use `build/interrupt-tests/storage-esp`. That staged image retains the
self-test kernel while the normal build has `VALI_INTERRUPT_SELFTEST=OFF`.
PCI MSI/MSI-X device programming had not been added at this point.

Local logs are under `build/interrupt-tests/`. The test scripts live in the
`testing` submodule. The RPC fix and its tests live in `librt/libgracht`. Both
submodules must be included alongside the parent repository changes.

## PCI single-message MSI and EDU validation (2026-10-20)

The PCI provider now owns one MSI reservation per function, and drivers can
request single-message MSI through libddk. PCI MSI-X table programming and
multi-message MSI remain unsupported.

The group handle identifies the reservation. Vali still has no device
authorization or capability system, and the existing kernel memory-space
ownership checks are unchanged.

`services/deviced/bus/pci/msi.c` and `msi.h` walk the conventional capability chain
with a fixed bound, save configuration, prepare MSI with delivery disabled and
handle enable, disable and restore. They use the data and mask offsets for each
32-bit or 64-bit address layout, including optional per-vector masking.
Multiple Message Enable stays zero while the reservation is prepared or enabled.

The PCI device provider holds the existing PCI lock for each operation. It checks
ACPI NO_MSI policy and rejects MSI on unsupported architectures. While a
reservation is active, bus-control requests preserve INTx Disable and raw
configuration writes are rejected. The host cannot be destroyed until the
reservation is released.

`librt/libddk/interrupt.c` checks the requested modes against provider support,
reserves the kernel vector and coordinates PCI and kernel cleanup. The driver
must still stop its interrupt sources and set up MMIO and bus mastering.

The interrupt allocation syscall accepts single-message MSI and INTx, returning
the composed message in its result. MSI-X remains available only through the
kernel interrupt group interfaces.

The device protocol transports `BusDevice.Type` as `sys.bus_type`, with
`GENERIC = 0` and `PCI = 1`. `DeviceInterruptInitialize` derives its `IsPci`
predicate by comparing the type with `BusTypePci`; other nonzero bus types must
not enable PCI MSI. These two values retain the former `uint32 is_pci` wire
encoding. `device_bus_test.c` checks byte-for-byte compatibility, conversion in
both directions, and preservation of routing metadata. Before bus identity was
transported, conversion discarded it, which made the first EDU boot reject MSI
as unsupported. Rebuild generated protocols, the runtime, services and modules
together for the renamed field; the allocation-result layout changes also
require rebuilding the kernel.

### Ordering and failure contracts

Allocate saves PCI state once, then programs the message with MSI and INTx
disabled. It records the snapshot before writing anything, so cleanup can use it
even if the first write fails. Every write is checked by reading the same
register. Readback catches programming failures and orders writes. It cannot
guarantee that interrupt messages have drained.

Enable binds all resources and opens kernel dispatch before enabling PCI MSI.
The driver must stop its queues, DMA and other interrupt sources before calling
Disable. Disable stops PCI MSI first, closes kernel dispatch, then waits for
running handlers before releasing their mappings.

Destroy restores the original message and mask registers. INTx Disable follows
the latest accepted bus-control request during the reservation, or the original
bit if no request intervened. The PCI lock serializes this update with Release,
including requests between failed cleanup attempts. Rejected requests do not
change the saved state. Unrelated command bits are preserved. MSI stays disabled,
and the API refuses to take over MSI/MSI-X that was already enabled.

If an RPC result is uncertain, retry with the same group handle. A failed
PCI stop leaves kernel dispatch and mapped resources intact. The next Enable
must finish that cleanup before it can succeed. If MSI Destroy fails, finish
Destroy before allocating again.

Allocate can fail and still leave `State` set when rollback also fails. Keep the
object and retry Destroy. Transport or decode failures return `OS_EPROTOCOL`,
distinct from service results such as `OS_EBUSY`.

After release, non-kernel message vectors that were enabled stay reserved until
reboot. They keep disabled, unbound descriptors with a permanent closed dispatch
gate. The group object and driver mappings are freed, but a late message
cannot reach another device's handler through a reused vector.

Vectors from never-enabled groups can be reused. Disable/Enable also keeps the same
vector. Destroy/Allocate uses another slot each time an enabled vector is retired,
out of the x86 backend's 16 candidates shared with other reservations. Reclaiming
these slots needs platform message draining support. Automatic cleanup after a
driver or service crash is also unimplemented.

### Host regression tests

```powershell
./testing/pci_msi_test.ps1
./testing/interrupt_registration_test.ps1
./testing/interrupt_architecture_test.ps1
```

The RPC wrapper test needs the generated protocol headers from `build/windows`.
On Linux, build/generate the protocols first, then run
`VALI_PROTOCOLS=/path/to/build/protocols ./testing/pci_msi_test.sh`.

`pci_msi_test.c` runs the real configuration engine against four MSI layouts.
It injects 116 failures at setup, enable, disable and restore writes. Each failure
is tested as a dropped write and as a write that succeeds but fails readback.
Cleanup must keep the original snapshot so a later Release can restore it.
Other cases check capability cycles, overlap and truncation, duplicate MSI,
already enabled modes, unsupported message widths, competing reservations,
repeated operations and preservation of unrelated command bits.

`pci_msi_ddk_test.c` runs the actual libddk wrapper and configuration engine
together. It loses responses before and after PCI operations and fails each
kernel bind, enable, disable, unbind and release. Cases also cover persistent
stop failures, failed Allocate with incomplete rollback, failed Enable followed
by failed cleanup, and re-enable after an uncertain Disable. INTx fallback,
unsupported counts and repeated allocation lifetimes are checked too.

`pci_msi_rpc_test.c` checks send, await, decode and response-length failures in
the ioctl wrapper. The kernel harness checks that retired vectors suppress late
delivery and stay reserved, while never-enabled vectors can be reused.

`pci_msi_provider_test.c` integrates the actual provider's bus-control and MSI
dispatch with configuration access and the MSI engine. Its 24 cases cover
initially enabled/disabled INTx, no intervening request, either requested state,
successive requests, rejected requests and a request between Release retries.
It verifies that INTx stays masked during reservation and that Release preserves
the latest request and unrelated command bits. Temporarily removing the saved
INTx update makes this test fail.

Run `python testing/interrupt_qemu_runner_test.py` to exercise the actual runner
with a mocked clock and QEMU process. Seven tests cover each late milestone,
optional EDU, the overall timeout, delayed failures and a process exit at the
acceptance boundary. Kernel completion at 0 seconds and userspace readiness at
50 seconds must complete at 90 seconds; a failure at 60 seconds must be rejected.
Temporarily timing from kernel completion makes the suite fail.

These tests passed natively on Windows ARM64. The MSI tests and kernel harness
also passed under x64 AddressSanitizer, with the same Windows-on-ARM interceptor
warnings documented earlier. A clean Linux ASan/UBSan run is still needed.
Compilation of the interrupt boundary passed for i386, amd64 and aarch64,
including the new PCI engine/provider and EDU module. ARM64 reports MSI
unsupported at runtime.

### Real PCI delivery in QEMU

The optional `modules/edu` driver binds QEMU's `1234:11e8` educational device.
Build it with `VALI_EDU_SELFTEST=ON`. It is off by default.
The call to `DeviceInterruptAllocate(..., 1, 1, INTERRUPT_STRATEGY_MSI, ...)`
requires MSI, so INTx fallback cannot make the test pass. All test interrupts
come from writes to EDU's interrupt-raise register.

Each boot checks 192 acknowledged MSI messages across three reservations and six
enable/disable cycles. With MSI disabled, it raises and clears causes and checks
that the handler stays idle. It also checks that bus control preserves INTx
Disable and rejects raw configuration writes. Each reservation starts with
INTx enabled; after a later bus-disable request, Destroy must leave INTx
disabled. It then repeats Destroy before releasing the MMIO mapping.

EDU's MSI writes need bus mastering. An early diagnostic boot left that bit
clear and timed out without delivering a single interrupt.

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
all four CPUs online, and the userspace storage-startup marker before starting
its 40-second observation. The overall timeout includes startup and observation;
slow startup never shortens the observation period. Any EDU failure, kernel
panic, synchronization timeout or QEMU exit fails the run, including on the
final observation sample. Each run uses its own disk snapshots and
firmware-variable copy.

The initial image passed six consecutive four-CPU boots: HPET on in `msi-final-1`,
`msi-final-3` and `msi-final-5`, and HPET off in `msi-final-2`, `msi-final-4` and
`msi-final-6`. All six passed the APIC and EDU checks, reached
`InstallBundledApplications no bundled apps!`, and completed observation without
a panic or synchronization timeout. Those runs timed observation from kernel
self-test completion, so they do not establish 40 seconds of observation after
userspace readiness. Each boot also exercised the existing virtio-blk INTx
storage path. These runs validate single-message PCI MSI delivery.
PCI MSI-X table programming remains untested by the separate kernel MSI-X test.

The tested image is in `build/interrupt-tests/msi-esp`, with a SHA-256 manifest
at `build/interrupt-tests/msi-image-hashes.json`. Serial logs and exact
QEMU commands are in the six run directories. Early diagnostic runs are retained
as `msi-hpet-1` (missing IsPci conversion), `msi-hpet-2` (bus mastering disabled)
and `msi-hpet-3` (first successful delivery, before the final test additions).

After fixing the bus-control/Release interaction and observation timer, six more
four-CPU boots passed on October 20, 2026. `msi-review-1`, `msi-review-3` and
`msi-review-5` used HPET; `msi-review-2`, `msi-review-4` and `msi-review-6`
disabled HPET. Each boot delivered 192 EDU messages, checked the later bus-disable
request across three reservation releases, reached the full
`InstallBundledApplications no bundled apps!` marker and then completed the
40-second observation. No panic, synchronization timeout or startup failure
marker appeared. The runner used a 120-second overall deadline.

The corrected image is retained in `build/interrupt-tests/msi-review-esp`.
Its hashes are in `msi-review-image-hashes.json`, and the six serial-log checks
are summarized in `msi-review-results.json`, both under `build/interrupt-tests`.
Each `msi-review-N` directory contains its serial log and exact QEMU command.
The provider and runner negative-control logs are `review-provider-negative.log`
and `review-runner-negative.log` in the same test-output directory. Native host
tests, the kernel harness, all three architecture compilations and both
self-test and normal builds passed. The four PCI host tests also passed x64
ASan with the Windows-on-ARM interceptor warnings described above.

After validation, restore `VALI_INTERRUPT_SELFTEST=OFF` and
`VALI_EDU_SELFTEST=OFF` for normal builds. When reusing a deployment tree that
previously included EDU, also remove its generated `modules/edu-msi.dll` and
`modules/edu-msi.yaml` before packaging a normal initrd. The staged test image
does not need to change.

## Indexed messages and PCI capacity contract (2026-10-20)

`DeviceInterruptGetMessage` reads one allocation vector's composed message through
the new `ScInterruptGetMessage` syscall (75). Existing syscall numbers are unchanged.
The syscall checks ownership, rejects kernel groups, checks the index through the
group API, and copies the message to userspace. INTx returns `OS_ENOTSUPPORTED`.
The public DDK wrapper preserves the caller's output on failure. The message uses
three 32-bit fields: address low, address high and data. An allocation index is
not a CPU vector number. Table assignment is a separate provider contract; the
initial MSI-X provider maps allocation indices directly to table indices.

`PciInterruptSupport` now includes `MsiCapacity` and `MsixCapacity` alongside
`Strategies`. These counts describe the capabilities advertised by an eligible
function, not available kernel vectors or implemented modes. The shared probe
validates the entire conventional capability chain, including duplicate MSI-X
capabilities. It reports 1..32 MSI messages and 1..2048 MSI-X table entries without
changing configuration. The MSI-X provider described below validates its BARs
and table before offering that strategy. Policy-blocked queries return zero
capacities and strategies.

Initially `Strategies` advertised only single-message MSI. At that stage libddk
and the allocation syscall rejected MSI-X-only requests, including one vector.
The lifecycle integration below removes those gates. Multi-message MSI remains unsupported.
The capacity response grew from 4 to 12 bytes; rebuild deviced and its clients
together. Old-length replies are rejected by the RPC wrapper.

Libddk allocates vector storage from the kernel's returned count and initializes
every vector's default affinity and invalid event handle. It retains the kernel
handle if storage allocation fails and rollback cannot release the group. Setters
and Enable reject that incomplete allocation; Destroy can retry its cleanup.
Single-message MSI now obtains its programming payload through the indexed query.

Validation: the PCI host suite, interrupt lifecycle suite, three architecture
boundary compilations and full normal amd64 build passed. The architecture tests
also check message/capacity field offsets and sizes. Host cases cover all MSI-X
table sizes, malformed discovery, policy gating, independent indexed messages,
ownership, output failures, retained rollback and continued MSI-X rejection.
The multi-vector DDK storage test uses an internal kernel group so it does not
bypass or relax production allocation gates. Removing the new syscall ownership
check makes the regression test fail; restoring it passes. No QEMU delivery run
was performed for this contract change.

The interrupt harness and all four PCI host tests also passed under x64 ASan
using Visual Studio's Clang 22. The runtime reported the same three
Windows-on-ARM interceptor warnings described above, with no reported memory
error. ValiCC's Clang 23 installation does not include the required ASan runtime.

## deviced MSI-X backend (2026-10-20)

The provider now offers MSI-X when the capability, table and pending-bit array
(PBA) have valid memory BARs. Validation checks BIRs, 64-bit BAR pairs, current
configuration against the enumerated BAR address, full table/PBA extents, CPU
addressability and overlap, including aliased BARs. An invalid MSI-X layout
does not hide a valid MSI capability. The table BAR must also have a matching
published memory I/O registration.

`OSIOCTLREQUEST_PCI_MSIX` carries a fixed 32-byte `PciMsixRequest`. Reserve chooses
entries `0..Count-1`; Program supplies one indexed message; Enable requires every
selected entry to be programmed. The MSI request layout is unchanged. MSI and
MSI-X reservations exclude each other, and raw PCI configuration writes remain
blocked while either reservation exists.

Reserve maps the full table through `MapDeviceIo` (syscall 76), which maps a
bounded, uncached alias without taking or changing the driver's exclusive BAR
claim. The caller keeps the registration alive and releases the page-aligned
mapping through `MemoryFree`. This follows the existing device-I/O trust model;
it does not add device authorization. The syscall checks range/address/page-count
overflow and safely copies the result to userspace.

The engine saves the table before writing it, enables MSI-X under Function Mask,
and masks every entry, including unused entries. Payload writes require both the
function and entry masks and are verified by readback. Enable unmasks only the
selected entries before clearing Function Mask. Disable sets Function Mask and
keeps MSI-X mode enabled, avoiding a fallback to INTx during cleanup. Readback
orders writes; callers must stop device activity and account for in-flight
messages before releasing kernel handlers/vectors.

Release restores saved table entries while masked, then unmaps and restores the
original disabled configuration. Failed writes or unmaps retain the reservation
for retry with the same allocation handle. Once the table is restored, retries
never touch it again. Cleanup honors the latest accepted INTx/MMIO bus-control
request and remains available after a platform-policy change. Unpublication and
host destruction refuse active reservations so BARs and the cleanup RPC remain
available.

The backend initially left driver-facing allocation gated. The lifecycle
integration and delivery validation below now enable that path.

Validation includes the full normal build, i386/amd64/ARM64 boundary compilation,
the existing MSI/INTx and RPC suites, bounded mapping/copyout regressions, and the
MSI-X provider integration test. The MSI-X engine covers 152 dropped/uncertain
write cases with direct cleanup or retry, allocation/mapping/unmapping failures,
masked ordering, unused entries, and full lifecycles at 1, 65 and 2048 entries.
The programming-guard mutation is rejected by the test suite; its log is at
`build/interrupt-tests/msix-mutation.log`. Native ARM64 and x64 ASan host runs pass
(with the Windows-on-ARM interceptor warnings noted above). No QEMU MSI-X delivery
run has been performed for this backend-only change.

## Driver-facing MSI-X lifecycle and QEMU delivery (2026-10-20)

`DeviceInterruptAllocate` now negotiates MSI-X, MSI and INTx in that order, among
the caller's allowed strategies. MSI-X requests are capped to the provider's
table capacity; the kernel can return fewer vectors down to `min`. MSI and INTx
fallback require `min == 1`. Non-PCI devices continue to use INTx. Unsupported
capabilities and kernel vector exhaustion permit fallback, while malformed
configuration, provider ownership conflicts and RPC failures remain errors.

For MSI-X, libddk reserves the table and programs each entry with the corresponding
indexed kernel message before reporting allocation success. Enable binds every
handler and opens kernel dispatch before clearing the provider's masks. Disable
stops PCI delivery before waiting for handlers and unbinding. Destroy releases the
provider before releasing kernel vectors. Lost replies, partial programming,
failed unmaps and failed rollback retain a reachable allocation for retry. Once
Destroy starts, re-enabling that object is rejected until teardown finishes.

The provider uses table entry `i` for allocation vector `i`. Drivers must configure
their device's queue/cause routing accordingly and only request modes they know
how to use. Existing storage, USB and network drivers still explicitly request
INTx. MSI-X does not automatically convert those drivers' device-specific setup.
On x86, enabled MSI/MSI-X vectors remain retired until reboot after destruction;
Disable/Enable reuses the reservation. ARM64 still has no message-vector provider.

`pci_msix_ddk_test.c` drives the real libddk and MSI-X engine with modeled hardware.
It checks one-vector MSI-X, capacity negotiation and partial grants, fallback,
each allocation stage, every indexed message query/program, every bind position,
lost replies before/after provider operations, kernel failures, and retry after
unmapping failure. The kernel harness also checks public multi-vector copyout
rollback, ownership and retirement of every enabled MSI-X vector. Native ARM64
and x64 ASan runs pass, with the existing Windows-on-ARM interception warnings.
The capacity-comparison mutation fails the suite (`msix-lifecycle-mutation.log`).

The optional `modules/rocker` driver (`VALI_ROCKER_SELFTEST=ON`, off by default)
binds QEMU's `1b36:0006` Rocker device. Its BAR0 TEST_IRQ register sends a real
MSI-X message using the selected table entry. The test requires MSI-X, uses three
distinct entries, verifies their kernel-composed payloads, and claims BAR1 before
allocation to exercise the provider's shared table mapping. It checks the unused
fourth entry stays masked and that pending bits set while disabled deliver only
after Enable. It verifies 306 messages over three reservations and six
enable/disable cycles, and checks full table restoration after each Destroy.
It configures no network ports or DMA rings.

Three boots of `build/interrupt-tests/msix-esp` passed: `msix-qemu-1` used four
CPUs with HPET, `msix-qemu-2` used four CPUs without HPET, and `msix-qemu-3` used
one CPU with HPET. Each also passed the kernel APIC self-test, EDU's 192-message
MSI test, and INTx virtio storage startup, followed by 40 seconds of observation.
No self-test failure, panic, synchronization timeout or QEMU exit occurred.
The normal module-search fallback for `mfs.dll` appears in these logs as it did
in earlier MSI boots; its subsequent `/initfs/modules/mfs.dll` load succeeds.

Serial logs, exact QEMU commands, image hashes (`msix-image-hashes.json`) and the
result summary (`msix-results.json`) are under `build/interrupt-tests`. Reproduce
with the existing runner using `--edu --rocker --smp 4` (optionally `--no-hpet`),
the staged ESP, the disk snapshot, and `--require 'InstallBundledApplications no
bundled apps!' --timeout 150`. Build with all three interrupt/EDU/Rocker self-test
options enabled when rebuilding that ESP. The eight runner regression tests also
pass, including a missing or delayed MSI-X PASS marker and a failure at the end
of observation.

The full self-test and normal builds and all three architecture compile checks
pass. Build settings have been restored to all self-test options OFF, and the
generated EDU/Rocker modules were removed from the normal deployment tree. The
staged validation image remains available.
