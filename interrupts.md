# Interrupt group tests

## Virtio startup stall investigation (2026-10-24)

The historical `dma-e2e-recovery/runs/direct-msix` failure still has no confirmed
root cause. QEMU completed 267 block requests while the guest consumed 266, then
all CPUs were idle. Eight clean baseline repeats passed, including compressed
initrds and an eight-CPU run. Two additional runs booted and delivered keys but
crashed QEMU in the experimental `x-query-virtio-status` diagnostic after the
guest was stopped. That diagnostic was removed; those runs are not acceptance
passes. None of these results proves what caused the historical failure.

Investigation found and fixed two independent ways to strand work:

- `FutexWait` returned `OS_EFORKED` even when `ThreadFork` failed. Userspace could
  then wait forever for a child that did not exist. It now restores IRQ state,
  releases waiter bookkeeping, and returns the actual error.
- `CpuCoreStart` enabled the scheduler only on ARM64. A same-core wake on an idle
  x86 AP could enqueue work without requesting scheduling, leaving it stranded
  when no timer deadline existed. Scheduler enablement now applies to every AP.

The production-futex regression injects `OS_EOOM` and `OS_EUNKNOWN` fork failures
and checks propagation, IRQ restoration, and empty waiter bookkeeping. The new
`cpu_idle_wake_test.c` executes production AP startup and scheduler code, enters
idle with no timer deadline, wakes a task on that AP, and requires a scheduling
request followed by dispatch. Restoring either old behavior fails its regression.
The full `smp_boot_test.ps1` suite passes 14 executables and both trampoline
assemblies. Virtio interrupt tests and the nine-executable DMA suite pass too.
The AMD64 kernel links; both changed kernel files also compile for ARM64.

```powershell
./testing/smp_boot_test.ps1
$env:CC = "$env:USERPROFILE/ValiCC/bin/clang.exe"
python testing/cpu_idle_wake_test.py --mutation-check
./testing/virtio_msi_test.ps1 -Target x86_64-pc-windows-msvc
./testing/device_dma_test.ps1
```

The boot checker now requires successful device-side Virtio block completions
to match guest-consumed completions and retains a larger per-CPU memory snapshot.
The rebuilt test image and source hashes are in `build/virtio-startup-stall/esp`
and `build-manifest.json`; only the kernel changed from the previous USB test
image. The initrd still contains the test-only HID key trace.

```powershell
python testing/xhci_enumeration_boot.py `
  --esp build/virtio-startup-stall/esp `
  --output-root build/virtio-startup-stall/runs `
  --mode preferred --topology direct --smp 4 `
  --key-cycles 2 --reconnect --observe-seconds 60 --name fresh-run
```

All six rebuilt-image runs passed with identical image hashes, 60 seconds of
post-boot observation each, balanced Virtio completions, and successful keyboard
enumeration, input delivery, Disable Slot, and same-port reconnection. Totals:
2016 device and guest block completions, 96 matching DMA reports and parsed key
transitions, and 1752 USB control completions. No panic, block completion error,
USB command error, or unexpected USB transfer error occurred.

| Run under `build/virtio-startup-stall/runs/` | CPUs | Topology | xHCI mode |
| --- | ---: | --- | --- |
| `fixed-direct-msix` | 4 | Direct | MSI-X |
| `fixed-direct-intx` | 4 | Direct | INTx |
| `fixed-direct-msi` | 4 | Direct | MSI |
| `fixed-nested-msix` | 4 | Two hubs | MSI-X |
| `fixed-nested-intx` | 4 | Two hubs | INTx |
| `fixed-direct-8cpu` | 8 | Direct | MSI-X |

`build/virtio-startup-stall/results.json` retains these results, the baseline
repeats, diagnostic failures, and the unresolved historical stall. Virtio uses
MSI-X except in INTx runs. These are functional tests of coherent emulated x86
and USB control/interrupt-IN traffic; they are not performance benchmarks or
physical Pi/RP1 validation. QEMU still reports zero scratchpads.

## DMA recovery and end-to-end USB checks (2026-10-24)

The recovery changes retain DMA ownership across failed cancellation/detach
cleanup and retry through the controller monitor. Scratchpads use independent
pages; periodic transfers cache their element layout while still copying each
completed packet to the client. The native DMA suite covers these paths,
including injected synchronization/unmap failures and 262144 periodic packets.

Live reconnect checks exposed a libusb routing error: detach used the controller
device ID as its IPC destination. It now targets the controller driver handle
while retaining the device ID in the RPC payload. A production-lib regression
uses distinct handles to prevent this from recurring.

The shared xHCI interrupt handler also now latches event work when IMAN.IP is set
but the earlier USBSTS snapshot missed EINT. Otherwise it could acknowledge the
interrupt without draining the ring or clearing event-handler busy. The new
pending-only regression fails before the fix; an unrelated shared interrupt
still returns unhandled. Scratchpad-count decoding now uses bits 25:21 as the
high field and 31:27 as the low field, matching the
[xHCI capability definitions](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/xhci-caps.h).
Tests cover counts 0, 1, 32 and 1023 and ignore the restore bit.

`testing/device_dma_test.ps1` runs nine native executables;
`testing/hid_setup_test.ps1` checks HID setup and parsed key delivery.
Mutation checks under `build/dma-recovery-validation/` reject lost cancellation
retry, lost detach retry, stale periodic tails, reversed scratchpad counts,
incorrect detach routing and lost pending-only interrupts.

For QEMU, configure `VALI_USB_INPUT_SELFTEST=ON`, build and package the current
kernel, phoenix and modules into an isolated EFI tree. The option is OFF by
default and emits parsed keyboard transitions only in the test image. Restore
it to OFF after packaging; the normal HID binary must not contain the trace.
The current isolated image is `build/dma-e2e-recovery/esp`; its initrd uses
uncompressed streams. `build-manifest.json` records the staged source hashes.

```powershell
python testing/xhci_enumeration_boot.py `
  --esp build/dma-e2e-recovery/esp `
  --output-root build/dma-e2e-recovery/runs `
  --mode preferred --topology nested --smp 4 `
  --key-cycles 2 --reconnect --observe-seconds 60 --name fresh-run
```

The checker waits for system boot, verifies B/C press-release bytes at completed
interrupt-IN DMA buffers and requires the exact matching sequence parsed by the
HID client. This checks the staging-to-client copy as well as hardware completion.
After the first sequence it removes the keyboard, waits for a successful Disable
Slot command, reconnects on the same port and repeats the sequence. Short packets
are valid. Command errors, unexpected transfer errors, missing keys, panics and
incomplete observation fail acceptance. Paused diagnostic time is excluded.
Artifacts include serial and QEMU traces, PCI/IRQ/LAPIC state, xHCI MMIO registers,
queue snapshots, key payloads, guest transitions, image hashes and results.

The final image passed six four-CPU HPET/TCG runs, each with 60 seconds of
observation, 16 matching DMA reports and parsed key transitions, one successful
Disable Slot/reconnect and 336 Virtio block completions. No run reported a panic,
USB command error or unexpected transfer error. All six used identical image
hashes; totals are 96 key transitions, 2473 control completions and 2016 Virtio
completions. Results are under `build/dma-e2e-recovery/runs/`:

| Run | Topology | Interrupt mode | Control completions |
| --- | --- | --- | ---: |
| `irq-direct-msix` | Direct | MSI-X | 57 |
| `irq-direct-intx` | Direct | INTx | 57 |
| `irq-direct-msi` | Direct | MSI | 57 |
| `irq-nested-msix` | Two hubs | MSI-X | 764 |
| `irq-nested-intx` | Two hubs | INTx | 778 |
| `irq-nested-intx-repeat` | Two hubs | INTx | 760 |

Earlier failures are retained in the combined `build/dma-e2e-recovery/results.json`.
The initial `direct-msix` boot stalled with 267 device-side Virtio block
completions but only 266 consumed by the guest. Its precise interrupt/event cause
remains unknown; later boots passed. Before the interrupt fix, `final-nested-intx`
stalled after a successful Configure Endpoint completion. The pending-only bug
fits that failure, but that run did not capture the register-read race, and an
unchanged-image repeat passed. The native test proves the fixed defect; the
subsequent QEMU passes do not establish the historical stall's exact cause.

QEMU reports zero scratchpads, so its boot checks do not exercise scratchpad-page
allocation. Native fault tests cover that path. These runs exercise control and
interrupt-IN transfers on coherent identity-mapped x86; they do not validate USB
bulk/isochronous traffic, noncoherent cache maintenance or physical Pi/RP1 DMA.

## Boot-connected xHCI devices and hub routing (2026-10-24)

The initial xHCI port scan now reports connected devices even when startup port
power writes have already cleared connect-change. Later scans still report only
connection changes. Root devices cannot inherit an unaddressed peer as a parent,
and their root-port limit comes from the controller's actual port count.
External hub ports retain their existing one-based numbering: each downstream
tier adds a route nibble starting at bit zero, up to five tiers, with ports above
15 saturated to 15. Low/full-speed descendants select the branch port immediately
below their high-speed transaction-translator ancestor.

Live hub testing also exposed startup and RPC blockers. usbd now sends hub
configuration to the controller's driver handle. The hub driver publishes its
local lookup before scheduling initial enumeration. Initial enumeration and
coalesced status work share one joined worker; the client receive callback only
copies the completed bitmap and schedules work. Control requests can therefore
yield without blocking the receive loop that must deliver their replies. Hub
teardown waits for that worker before releasing its buffers and device state.

`testing/device_dma_test.ps1` passes all six native suites. The xHCI cases cover
initial discovery, subsequent change handling, root/child distinction, route
depth and port limits, and direct/nested TT ancestry. Hub event tests cover
deferred initial enumeration, bitmap snapshots, events arriving during control
work, coalescing, stall recovery, failed job allocation, invalid buffer offsets,
and teardown with queued work. Restoring the original startup guard or route
shift fails the xHCI tests; making hub work synchronous fails the hub test.
The restored sources pass. Independent review found no remaining blockers.

The fresh image passed all four four-CPU HPET QEMU boots below. Devices were
connected before QEMU started; none were hot-plugged. Each run completed 336
Virtio block requests, six keyboard completions and six HID dispatches, verified
B/C press-release bytes, and observed 40 seconds without panic, HID setup failure
or xHCI command error. PCI traces verify MSI-X enabled/unmasked or INTx enabled
with both message capabilities disabled in the QEMU configuration.

| Run under `build/driver-msi-boot/` | Keyboard topology | xHCI mode |
| --- | --- | --- |
| `enum-worker-direct-msix` | Root port 1 | MSI-X |
| `enum-worker-direct-intx` | Root port 1 | INTx |
| `enum-worker-nested-msix` | Two hubs, path `1.3.2` | MSI-X |
| `enum-worker-nested-intx` | Two hubs, path `1.3.2` | INTx |

`testing/xhci_enumeration_boot.py` reproduces these checks against the packaged
`build/driver-msi-boot/esp` and `build/windows/disk.img`. Use `--mode preferred`
or `--mode intx`, `--topology direct` or `--topology nested`, `--smp 4`, and a fresh
`--name`. Each run saves image hashes, QEMU arguments, serial/PCI traces, physical
report-buffer snapshots and `results.json`. Native logs and mutation failures
are under `build/xhci-enumeration-*.log`.

amd64, Pi 4 and Pi 5 builds of xHCI, usbhub and usbd pass. These changes stay in
the shared USB stack; Pi message controllers, RP1 chaining, resource activation
and DMA translation are unchanged. This validates emulated USB enumeration and
delivery, not physical Pi PCIe/MSI wiring or hardware transaction translation.

## HID descriptor and keyboard delivery (2026-10-24)

HID setup reads the HID class descriptor from the selected interface and
alternate setting in the configuration, then requests the report descriptor.
The previous standalone interface GET_DESCRIPTOR(HID, 0x21) stalled in QEMU.
Its [HID implementation](https://github.com/qemu/qemu/blob/master/hw/usb/dev-hid.c)
includes the class descriptor in configuration data and handles the separate
report-descriptor request. The driver now follows that configuration path for
all devices, without a QEMU or board-specific condition. The raw configuration
is fetched once more because libusb's parsed form retains only interface and
endpoint descriptors. Temporary storage is freed on success and failure.

`testing/hid_setup_test.ps1` compiles production setup, report parsing and event
handling. It checks descriptor bounds, interface/alternate selection, multiple
class-descriptor entries, failed configuration requests, and keyboard press and
release events. Inverting the alternate-setting match fails the test; restoring
it passes. Independent review found no blockers. amd64, Pi 4 and Pi 5 ARM64 HID
builds passed; the controller, DMA and board-specific paths are unchanged by
this HID fix.

Four-CPU HPET boots `hid-reports-msix` and `hid-reports-intx` under
`build/driver-msi-boot/` both passed 40 seconds of observation, 336 Virtio block
completions, 12 successful USB control transfers and four keyboard completions.
PCI traces show xHCI MSI-X enabled/unmasked in the first run; the second disables
MSI/MSI-X and enables INTx. Both runs have four HID report dispatches and no
panic, HID setup failure or command error.

The boot checker injects B and C after enumeration and captures the buffers of
completed interrupt-IN TRBs using QMP physical-memory reads. Both modes produce:

```text
00 00 05 00 00 00 00 00  B pressed
00 00 00 00 00 00 00 00  B released
00 00 06 00 00 00 00 00  C pressed
00 00 00 00 00 00 00 00  C released
```

QEMU's eight-byte reports complete as short packets against the advertised
16-byte endpoint buffer. These are valid completions; the checker accepts them
and verifies both report bytes and HID dispatch. Each run retains
`keyboard-reports.json`, `results.json`, serial/PCI traces and its QEMU command.
The executable checker is `build/driver-msi-boot/boot-hid.py`; run it with
`--mode preferred` or `--mode intx`, `--smp 4` and a fresh `--name` after packaging
the rebuilt image. This resolves the HID limitation recorded in the initial
provider/DMA runs below. Physical Pi hardware validation remains outstanding.

## Provider-independent xHCI resources and DMA (2026-10-24)

Drivers use `DeviceGetResources` for registered IO descriptors and
`DeviceActivate` for preparation, start and stop. PCI implements these stages
with its existing decode/bus-master controls. RP1 owns DWC3 setup and publishes
`generic-xhci` with only the standard xHCI register window. Private DWC3 globals,
RP1 routing and parent activation stay in deviced. xHCI accepts either a PCI or
platform device identity; its interrupt setup still uses the same libddk API.
Drivers with both compatibility strings and numeric PCI matches remain eligible
through either namespace.

The driver lifecycle is Prepare, acquire registers and interrupts, reset the
controller, open DMA and allocate queues, Start, then run. Teardown confirms the
controller has halted before Stop, interrupt destruction and DMA release. A
failed halt or unmap retains ownership for a retry. Stopping one RP1 child does
not disable the shared parent while another child may still be using it.

`DMAOpen` obtains a provider lease and copies its configured translation and
cache rules into a kernel context. The existing SHM device engine supplies
allocation, mapping, synchronization and translated segments through the eight
DMA syscalls. Context close is busy while mappings exist. The DDK releases the
provider lease only after kernel close succeeds; session tokens protect a new
lease from a delayed release of an older one. x86 PCI retains coherent identity
DMA, while the Broadcom providers retain their configured address windows.

Device authorization is deferred to Handle Grants. These changes add ownership
and lifetime handling, not a device authorization system. Process exit cannot
prove DMA has stopped: abandoned mappings and provider leases remain quarantined
until a future recovery path can stop hardware safely.

xHCI uses device addresses for rings, contexts, scratchpads and transfer TRBs.
Streaming mappings transfer ownership before submission and return only the
completed byte range to the CPU. Periodic transfers map one packet at a time so
the client can read a completed packet while the next packet is in flight.
Control setup packets are copied from their byte offset before device ownership;
they never require reverse translation of a device address. Ring publication
and completion observation use DMA barriers. The fast handler uses direct
volatile MMIO so it remains callable from its kernel mapping.

Native regression commands:

```powershell
./testing/device_dma_test.ps1
./testing/pci_msi_test.ps1
./testing/firmware/rp1_runtime_test.ps1
./testing/firmware/pci_host_test.ps1
```

The DMA suite covers kernel ownership and rollback, provider close retries,
addresses above 4 GiB, 64 KiB TRB boundaries, nonzero setup offsets, root slot
contexts, short control transfers through their status stage, periodic packet
offsets, cancellation, and failed cleanup/rearm. Inverting the manager's
hardware-active scan guard causes the regression test to fail; restoring the
guard passes. Provider tests exercise private USB setup, stable resource IDs,
partial preparation retries, exact lease tokens and teardown exclusion. PCI host
tests cover both legacy configurations, BCM2711 and BCM2712.

Physical Pi validation must still check RP1 USB setup readback, both USB
controllers, actual MSI/MSI-X delivery, translated and bounced DMA, cache
visibility, sustained bulk/interrupt traffic, and disconnect/cleanup under load.
The QEMU Pi 4 model checks ARM boot regression only; it does not establish PCIe
MSI delivery or RP1 operation. The boot-connected and nested-hub validation above
supersedes the external-hub routing limitation from these initial runs.

Validation completed with full amd64 and Pi 4 builds and the ARM64 kernel,
deviced and USB-driver targets. `provider-dma-final-preferred` and
`provider-dma-final-intx` under `build/driver-msi-boot/` both booted four CPUs with
HPET enabled, completed 336 Virtio block requests and eight successful xHCI
control transfers after hot-plug, and remained stable for 40 seconds. PCI traces
show xHCI MSI-X enabled/unmasked in the preferred run. Neither run had a panic
or xHCI command error. These initial runs subsequently returned STALL for the
HID descriptor request; the HID fix and keyboard-report validation above
supersede that limitation.
The separately packaged `rpi4b-provider-dma` image booted four CPUs and all six
core services, then remained stable for 40 seconds. Boot logs, results and binary
hashes are retained under `build/`; native test logs use `provider-dma-*.log`.

## Driver interrupt preference (2026-10-22)

AHCI, the shared UHCI/OHCI/EHCI/xHCI setup, virtio-net and virtio-blk now request
one vector with `INTERRUPT_STRATEGY_ANY`: the existing allocator prefers MSI-X,
then MSI, then INTx. PS/2 remains line-only; EDU and Rocker retain their explicit
MSI and MSI-X test modes. Unsupported modes and exhausted controller capacity
can fall back; configuration, transport and cleanup failures still propagate.

Virtio enables the PCI interrupt mode before queue setup, assigns every queue
and configuration changes to MSI-X table entry zero, and checks route readback
before enabling queues or setting DRIVER_OK. Virtio resets preserve the selected
PCI mode in the transport and rebuild the routes. Its MSI-X handlers report both
causes without reading the legacy ISR byte, following the
[Virtio PCI interrupt model](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).
MSI/INTx retain the ISR acknowledgment path. This keeps the current single-event
driver architecture; it does not add per-queue vectors or CPU affinity.

`testing/virtio_msi_test.ps1` tests production common-register programming against
a banked register model and compiles the production fast handler bodies with
their driver headers. Coverage includes shared vector routing, reset restoration,
rejected routes and write failures, MSI-X delivery with a zero ISR byte, and
legacy shared-line rejection. Existing PCI MSI suites cover negotiation and
cleanup. Physical Pi 4/Pi 5 delivery remains to be validated.

Validation passed: both Virtio host suites, the seven PCI MSI/MSI-X suites, the
normal AMD64 build, and ARM64 builds of all seven affected drivers. Inverting
the net handler's MSI-X condition made its test fail; restoring it passed.
Independent implementation and test review found no required changes.

AMD64 QEMU boot validation (2026-10-22), using the current normal build and
hash-verified staged drivers, is recorded in `build/driver-msi-boot/results.json`:

- One CPU with HPET: preferred modes, xHCI MSI fallback, and Virtio/xHCI INTx
  fallback all reached service startup and remained stable for 40 seconds.
- Four CPUs without HPET: preferred modes passed the same boot observation.
- Each successful run mounted `/boot` and `/data` and completed 336 Virtio
  storage requests. The preferred runs delivered 336 MSI-X handler events;
  the INTx run delivered 336 legacy handler events.
- QEMU traces confirmed unmasked MSI-X on Virtio block/network and xHCI, MSI
  on AHCI, and xHCI selecting MSI when MSI-X was disabled. USB event delivery
  and network packet traffic were not established by these boot tests.
- Four CPUs with HPET stalled twice during early startup, before any message
  interrupt mode was enabled. The first capture found CPU 0 in
  `SpinlockAcquireIrq` and the other CPUs halted. Disabling HPET avoided the
  stall; its root cause has not been established. Default SMP boot therefore
  remains an outstanding acceptance failure.

All disks used QEMU snapshots and private firmware variables. These runs did
not modify the normal disk image or change production sources/build options.

## Broadcom driver activation (2026-10-22)

The host-wide `DriversBlocked` field and its checks have been removed. Successfully
initialized Pi 4 and Pi 5 hosts follow normal PCI publication, resolve kernel-issued
message controller IDs, and allow generic endpoint drivers to bind and control
their devices. No opt-in hardware-test mode is required. Failed host initialization
still prevents enumeration/publication. RP1 remains owned by its function handler;
its child interrupt/DMA readiness and function-specific activation checks remain.

Host tests cover Pi 4 binding and bus control, Pi 5 external-host publication and
controller resolution, independent hosts, and RP1 ownership. Provider tests retain
configuration-write protection during active message reservations and cleanup
after function policy changes. Driver interrupt-strategy selection is covered by
the subsequent driver preference work above.
Physical MSI/MSI-X delivery on either board has not yet been validated.

## BCM2711 MSI backend (2026-10-22)

Pi 4 now publishes a kernel-owned controller keyed by the PCI host's FDT node
offset. Discovery validates an enabled `brcm,bcm2711-pcie` host with a self MSI
parent, a zero-cell receiver, a translated register extent, and the named `msi`
interrupt connected to a level-high GIC SPI. The bundled tree uses CPU registers
at `0xfd500000` and GIC ID 180. BCM2711 has one receiver; duplicate host routes
are rejected before publication. Pi 5's MIP discovery and x86 allocation remain
unchanged.

Boot discovery maps the block and reserves/binds the parent without enabling it
or accessing receiver registers. The first message allocation configures the
receiver after deviced's PCI host reset and inbound-window setup. Allocation
must follow device publication: reset, link and BAR readiness checks reject
incomplete hardware setup, but cannot distinguish firmware's earlier ready
state from completion of deviced startup. Host reset, power cycling, and inbound
window changes after allocation require a future coordinated lifecycle.

The receiver checks hardware revision >= 3.3, root-port/link readiness, and the
supported BAR2-only RAM aperture. It chooses `0xfffffffc` if outside that aperture,
otherwise `0xffffffffc`; an aperture covering both is rejected. This uses the
actual rounded hardware window, avoiding a message address that aliases RAM.
The payload is `0x6540 | ordinal`, with 32 vectors. These register settings follow
the [Raspberry Pi Linux PCIe driver](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/pci/controller/pcie-brcmstb.c).
Single-message MSI and MSI-X use the same pool; multi-message MSI is still outside
the current group API.

Software child slots 1024..1055 are separate from GIC IDs 0..1019. GIC setup and
line routing retain their original hardware bounds. The parent services a
bounded pending snapshot, clears each selected bit before invoking its child,
and performs one GIC acknowledgment through the normal outer dispatch. Child
dispatch preserves the parent CPU context and reader lifetime. Messages arriving
during a handler remain pending; disabled children retain pending state until
re-enabled. An atomic claim bitmap protects pending-bit cleanup against competing
allocations; the common core still owns handler slots, synchronization, rollback
and permanent retirement. Retired hardware bits remain unavailable.

Group handles keep their existing encoding. The core rejects dispatch slots
ending in the reserved ten-bit group tag, including slot 1023, so expanding ARM's
table cannot make a group handle identify an interrupt descriptor.

Run `python testing/arm64-kernel/test_bcm2711_msi.py` on Windows with ValiCC.
Use `--compiler <clang> --target x86_64-pc-windows-msvc --asan` for ASan.
The modeled receiver runs production firmware parsing, controller registration,
interrupt dispatch and groups. Coverage includes 22 synthetic trees, both bundled
Pi trees, all 32 payloads, both message addresses, exhausted pools, initialization
and per-allocation failures, W1C/masking, arrival during handling, one parent EOI,
self/peer removal during dispatch, reservation rollback and permanent retirement.
It rejects slot 1023 and checks that a busy claimant cannot clear the current
owner's pending bit. Inverting the pending-bit condition fails the suite.

Native and x64 ASan tests pass (the latter emits existing Windows-on-ARM
interceptor warnings). Pi 5 MIP, common interrupt, PCI MSI/MSI-X, PCI host tests,
three architecture compile checks, and ARM64 kernel/deviced linking also pass.
The full amd64 build passes. A rebuilt four-CPU x86 QEMU boot passes APIC
allocation, 192 EDU MSI messages, 306 Rocker MSI-X messages, and a 40-second
post-startup observation. Normal build options were restored after staging it.
Logs are under `build/interrupt-tests/bcm-msi-*`.

The Broadcom driver-activation gate has been removed. Physical Pi 4 MSI/MSI-X delivery,
posted-write ordering and reset behavior still require hardware acceptance.

## BCM2712 MIP backend (2026-10-22)

The ARM64 GIC startup path now discovers BCM2712 PCI hosts and their MIP
controllers from the retained boot tree. It validates all selected routes before
accessing MMIO: enabled host/provider, zero-cell MSI parent, register extents,
edge-triggered SPI range, message offset/count, hardware GIC capacity, and a
unique inbound PCI window that maps the message address to the MIP register block.
The supported binding places the host and MIP under the same firmware bus.
Absent or unsupported routes receive no controller. Malformed recognized routes
fail boot instead of publishing a partially validated controller.

`DeviceTreeNode.NodeOffset` preserves the FDT structure-block offset already used
by deviced's lookup ABI. Each enabled host receives a kernel-owned controller
handle with that identity. Hosts referencing the same MIP reuse its MMIO mapping
and contend for the same GIC dispatch slots. Independent MIPs have independent
slots, even when their device-visible message addresses are identical.

MIP initialization selects edge delivery to the host and masks delivery to the
VPU. The GIC keeps interrupts disabled until the core enables a bound handler.
For ordinal `i`, the payload is `msi-offset + i`, and the GIC ID is
`32 + msi-ranges SPI base + msi-offset + i`. The address comes from untranslated
`reg[1]`; only the CPU mapping of `reg[0]` is translated through ancestor ranges.
These encodings and register settings follow the
[Linux MIP driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/irqchip/irq-bcm2712-mip.c)
and [Raspberry Pi BCM2712 firmware binding](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm64/boot/dts/broadcom/bcm2712.dtsi).

The backend has no second allocation bitmap. The common core owns exclusive
claims, dispatch gating, synchronization, rollback and permanent retirement.
The existing GIC configuration and acknowledgement paths control each message's
SPI. Single-message MSI and independent MSI-X vectors share this backend.
Initialization is boot-only; mappings and published contexts remain permanent.
An initialization failure after hardware setup or partial publication is fatal
and does not retract already published controllers.

Run `python testing/arm64-kernel/test_mip.py` on Windows with ValiCC installed.
Use `--compiler <clang> --target x86_64-pc-windows-msvc --asan` for ASan. The host
fixture runs the real controller registry, interrupt core and group lifecycle,
with MMIO, GIC access and kernel services modeled. It covers 27 synthetic trees,
the bundled Pi 4 and Pi 5 trees, and the Pi 5 tree with its external PCIe host
enabled. The latter verifies both real MIPs: 64 vectors starting at ID 160 and
eight vectors starting at ID 287, with payload offset eight for MIP1.

Tests cover shared and independent pools, exact message encodings, exhaustion,
allocation rollback at every stage, partial-enable rollback, dispatch suppression,
re-enable, retirement through alias handles, malformed firmware, disabled nodes,
unchanged outputs on decode failure, failed MMIO mapping/readback, and every
controller-publication allocation failure. Inverting the capacity-bound check
fails the suite; restoring it passes. Logs are under `build/interrupt-tests/mip-*`.
Native and x64 ASan runs pass; ASan emits the existing Windows-on-ARM interceptor
warnings. All three architecture boundary checks, the full amd64 build, and the
AArch64 `vali-core`/`deviced` build pass.

The Broadcom driver-activation gate has been removed. This implements
the kernel backend; physical Pi 5 MSI/MSI-X delivery, PCI reset interaction and
pending-message behavior still require hardware acceptance. Pi 4's BCM2711
aggregated MSI backend is documented above. No x86 allocator behavior or public
wire layout changed. Stale `Domain` references, the old resolver syscall number
in the test harness, and a missing ARM thread interrupt include were updated to
match the preceding interrupt refactor.

## Common interrupt reservation path

Standalone callers use `interrupts.h`; group callers use `interrupts_group.h`.
Both reserve through the same interrupt core, which calls `component/ic.h`
for controller resources and routing. Only the controller implementation calls
the architecture resolution/configuration operations. CPU interrupt-state,
priority and acknowledgement primitives remain architecture operations.

`InterruptControllerResource_t` carries the permanent controller association,
dispatch slot, backend token, resolved line properties and immutable message.
Backends allocate this resource without creating child handler IDs. A chained
backend can activate its boot-reserved parent during first allocation, outside
the core locks; its enable/disable callbacks must not re-enter the core.
The core owns IDs, mappings, atomic
dispatch claims, shared-line enable counts, synchronization and tombstones.
The message allocation and line/local resolution paths converge before claiming
the dispatch table. `InterruptReserveMessage` reserves a candidate in a selected
domain; `InterruptReserve` also supports standalone platform MSI, including
fixed-vector and candidate-list requests. Both use the same message encoding.

Backend allocation runs outside the dispatch and controller registry locks.
If the core cannot claim a slot, it returns the backend resource before returning
the failure. This unexposed rollback must succeed. Hardware enable/disable runs
under the dispatch lock with IRQs disabled, must not block, and does not perform
handler synchronization. Backend release preserves ownership on failure and must
support kernel self-unregistration without invalidating in-flight routing.
The core holds the dispatch claim through the reader grace period.

`InterruptRelease(id, retire)` releases resources through the stored controller.
Retirement requires a disabled, unbound message reservation. Successful retirement
transfers backend resources to permanent ownership and retains the core descriptor
with a permanent closed dispatch gate. It cannot be rebound or enabled. Groups
continue retiring enabled user MSI/MSI-X reservations; never-enabled allocations
and kernel test groups remain reusable. `InterruptUnregister` uses ordinary release.

Permanent CPU-local and platform-line routes support early software handlers and
PIC lines before message domains are published. Firmware-routed lines retain
their discovered controller. `interrupt_types.h` shares request/routing types
without making consumer headers depend on architecture interfaces.

The host suite covers common standalone/group contention, fixed and candidate
message selection, backend rollback after a busy claim, release/retirement retry,
retained-vector binding rejection, early CPU-local registration and custom-backend
self-unregistration. Existing sharing, mapping failures, ownership, group gating,
late-message retirement and concurrent-allocation cases remain in the suite.

Validation for this change: native Windows host tests and x64 AddressSanitizer
passed (the existing Windows-on-ARM interceptor warnings remain). Removing the
backend rollback call made the resource-ownership test fail; restoring it passed.
The i386, amd64 and ARM64 boundary compilation checks and the full amd64 build
passed. A fresh four-CPU QEMU image passed kernel APIC checks, 192 EDU MSI messages,
306 Rocker MSI-X messages, INTx storage startup and 40 seconds of observation.
Logs and the staged image are under `build/interrupt-unification`. The normal
configuration was restored with interrupt, EDU and Rocker self-tests OFF, the
optional test modules removed from normal deployment, and the normal build passed.

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

### Kernel diagnostics

The controller component, interrupt core and optional APIC boot self-test use
the kernel's `debug.h` logger. Enable both `__TRACE` and
`__OSCONFIG_LOGGING_KTRACE` to trace controller setup, reservation, binding,
delivery control and teardown. Warnings and errors remain enabled without
tracing. Dispatch and individual handler invocations do not emit trace messages;
expected controller misses and busy allocation candidates are trace-only.

Failures identify the operation, status and relevant interrupt ID, controller,
resource index or mapping. Cleanup failures are logged separately from the
original failure without changing error precedence or retry ownership. The boot
self-test reports actual versus expected failure codes and identifies failed
APIC sends, target CPUs and delivery counts.

The registration host suite checks diagnostic content and severity for routing,
mapping, cleanup and enable failures. It also checks quiet expected misses and
an unlogged dispatch path. Run it with tracing both enabled and disabled; kernel
object checks must include the x86-only boot self-test on i386 and amd64, and
the controller/core on all three architectures.

### Running the suites

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

MSI-X uses the shared `OSIOCTLREQUEST_PCI_INTERRUPT` request described below.
Reserve chooses entries `0..Count-1`; Program supplies one indexed message;
Enable requires every selected entry to be programmed. MSI and MSI-X reservations
exclude each other, and raw PCI configuration writes remain blocked while either
reservation exists.

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


## Unified PCI message request (2026-10-20)

MSI and MSI-X now share `PciInterruptRequest`, `PciInterruptOperation` and
`OSIOCTLREQUEST_PCI_INTERRUPT`. The 36-byte request carries Operation, Allocation,
Strategy, Count, Index, AddressLow, AddressHigh, Data and Reserved. Strategy must
select exactly MSI or MSI-X. Reserve uses Count (exactly one for MSI); Program
uses Index and the message payload. Other fields must be zero when unused.
libddk uses the same Reserve, indexed Program, Enable, Disable and Release
sequence for both strategies. The provider validates the common envelope before
dispatching to separate configuration-register and MSI-X table backends.

MSI Reserve now snapshots configuration and suppresses delivery before Program
writes index zero. Enable rejects an unprogrammed message. Program retries retain
the original supplied payload, including after failed readback, and cannot change
it while delivery might be active. Once Release begins, only Disable and Release
are accepted. Both backends preserve ownership after uncertain writes and keep
cleanup available after policy changes. Kernel-vector retention, handler cleanup
ordering, MSI-X mapping retries, and MSI/INTx negotiation are unchanged.

This replaces the earlier 24-byte MSI and 32-byte MSI-X requests. The separate
MSI-X ioctl (4) is removed, and Program is inserted into the operation enum.
Rebuild libddk, drivers and deviced together; old request lengths and the old
ioctl are rejected rather than interpreted using the new layout.

Validation: native host suites, x64 AddressSanitizer, the full build, and
cross-compilation of the contract and interrupt boundaries for i386, amd64 and
ARM64 pass. Tests cover malformed strategies, unused fields, count/index bounds,
legacy envelopes, lost Program replies, immutable programming retries, uncertain
MSI enables and terminal Release. Existing MSI/MSI-X failure injection and INTx
lifecycle tests pass. Removing the MSI delivery guard makes the regression fail
(`build/interrupt-tests/unified-mutation.log`); restoring it passes.

A fresh `unified-irq-esp` image passed the four-CPU QEMU run at
`build/interrupt-tests/unified-qemu/serial.log`: kernel APIC checks, 306 MSI-X
messages across three vectors, 192 MSI messages, INTx storage startup, and the
40-second observation window. The normal configuration was restored with all
three interrupt self-test options OFF, rebuilt successfully, and the optional
EDU/Rocker drivers removed from its deploy directory.

## Kernel-owned interrupt message domains (2026-10-22)

`kernel/components/ic.c` owns permanent line controllers and message domains in
the machine's component list. The `Line` and `Message` members keep their state
separate. Line iteration and lookup ignore message components, including when
their numeric IDs overlap. Message domains and backend contexts live until
shutdown. Groups hold stable controller pointers; there is no service owner,
device route registry, per-device bind/unbind operation, or userspace controller
registration/removal API.

Kernel platform setup publishes the existing architecture backend after interrupt
initialization. x86 uses one shared domain for its PCI message fabric, retaining
the same software-vector pool, BSP address/data encoding, exclusive claims,
single-message MSI, independent MSI-X messages and retirement of enabled user
vectors. Kernel software tests can use Domain 0 to select this existing platform
domain. User message allocations require a resolved nonzero domain. INTx is
unchanged.

The kernel-only `CreateInterruptMessageController` copies an initialized backend and
its source identity, assigns a nonzero ID and rejects duplicate identities and
ID exhaustion. Publication never creates a domain as a side effect of lookup.
Callbacks execute outside the component lock, must serialize their pool, and
must preserve allocation ownership when cleanup fails. Rollback of unbound,
unexposed reservations must succeed. Permanent controller lifetime keeps cleanup
independent of userspace device-provider lifetime.

`DeviceInterruptControllerResolve` replaces the old controller-control syscall at
slot 78. It requires INTERRUPT_CONTROL and copies a 16-byte query plus a separate
output ID. Version must be 1; Reserved must be zero. Platform selects the shared
PCI message fabric and requires FirmwareNode = 0. DeviceTree identifies a PCI
host by its offset in the boot FDT structure. Unknown firmware identities return
unsupported without falling back to a different controller. Resolution is
read-only, including failed output copies. The ID is an allocator selector, not
a device-access capability, and needs no userspace close operation.

deviced resolves a host domain before starting its drivers, using its firmware
node when present. An unsupported domain leaves the host available for INTx.
PCI interrupt query now returns the domain with strategies and capacities;
libddk passes it as `InterruptGroupRequest.Domain`. DeviceId remains in libddk
for provider requests and is no longer sent to the kernel allocator. Publication
retries reuse the domain, and unpublication does not alter the kernel controller.
Active PCI reservations still prevent provider/BAR removal. A group allocated
before PCI Reserve owns no provider resources: if the provider disappears, its
request fails and the group can still be released safely.

This changes the PCI support reply from 12 to 16 bytes and replaces the first
allocation-request field's DeviceId semantics with Domain. Syscall 78 also has
a new signature. Rebuild the kernel, libddk, drivers and deviced together; this
experimental ABI is not compatible with the former registration/route interface.

This earlier domain-refactor stage did not add a Broadcom or ARM message backend.
The backend and activation work documented above supersedes that limitation.
Controller hot removal remains outside the permanent-domain contract.

Validation covers full normal and self-test amd64 builds; i386, amd64 and ARM64
boundary compilation; interrupt, PCI MSI/MSI-X and firmware host suites; and x64
AddressSanitizer runs of all three suites. Windows emitted the existing ASan
interceptor warnings, without test or sanitizer failures. Tests cover copied
queries, permissions, read-only resolution, missing/invalid IDs, mixed controller
types, shared and independent domains, duplicate identities, ID exhaustion,
allocation rollback, retryable cleanup, unchanged retirement and 1000 concurrent
allocations. PCI tests verify domain propagation, firmware identity, unsupported
publication, repeated publication and teardown without controller mutation.
Inverting the firmware-identity comparison fails the domain test; restoring it
passes (`build/interrupt-tests/domain-mutation.log`).

A fresh `build/interrupt-tests/domain-esp` image passed four-CPU QEMU delivery:
the APIC self-test, 192 EDU MSI messages, 306 Rocker MSI-X messages across three
vectors, INTx storage startup and the full 40-second observation window. Logs
are in `build/interrupt-tests/domain-qemu/serial.log`, with image hashes in
`domain-image-hashes.json`. Normal build options were restored to OFF and the
optional test modules removed before rebuilding the normal deployment image.

## AMD64 driver preference and SMP boot validation (2026-10-22)

The normal image uses MSI-X, then MSI, then INTx for eligible AHCI, USB and
Virtio drivers. The four-CPU HPET boot stall reproduced before storage interrupt
delivery was a handle-set registry race, not an HPET or message-controller fault.
`MarkHandle`, registration and removal released `g_handleSetsLock` before using
an inline bucket list. Robin Hood insertion, removal and resizing could then
move that list and its live spinlock. The captured stacks in
`build/driver-msi-boot/stall-diagnostic-13/` show two CPUs waiting in
`MarkHandle -> list_enumerate -> SpinlockAcquireIrq`; one waiter had ticket 5
while its stale lock contained Current=1 and Next=1.

The registry lock now protects the complete lifetime of each bucket access,
including event enumeration and registration rollback. Registration allocates
its subscription before locking and checks for failed table insertion without
publishing or leaking the subscription. HPET and the interrupt controllers are
unchanged.

`testing/smp_boot_test.ps1` includes `handle_set_relocation_test.c`, which compiles
the production handle-set code, list, hash table and tree. A deterministic
competing mutation runs when the registry lock is released. Tests cover growth
during add/mark/remove, collision displacement, deletion backshift, allocation
failure, duplicate rollback and subsequent event delivery. Retired table storage
is quarantined to catch stale lock access directly. Moving the mark operation's
unlock before enumeration makes this regression fail; restoring it passes.
The full SMP suite, x64 Virtio routing/handler tests and all seven x64 PCI
interrupt suites pass. The Virtio host test now declares the x86 intrinsics
normally supplied by the driver build's precompiled headers.

The rebuilt normal kernel was staged in the disposable ESP with interrupt,
EDU and Rocker self-tests OFF. Eight boots completed all startup milestones,
336 Virtio block completions each and 40 seconds of post-boot observation:

| Runs under `build/driver-msi-boot/` | CPUs | HPET | Interrupt selection |
| --- | --- | --- | --- |
| `fixed-stress-1` through `fixed-stress-4` (concurrent QEMU instances) | 4 | On | Virtio/xHCI MSI-X; AHCI MSI |
| `fixed-msi-smp4` | 4 | On | Virtio MSI-X; xHCI/AHCI MSI |
| `fixed-intx-smp4` | 4 | On | Virtio/xHCI INTx; AHCI MSI |
| `fixed-preferred-smp1` | 1 | On | Virtio/xHCI MSI-X; AHCI MSI |
| `fixed-preferred-smp4-nohpet-repeat` | 4 | Off | Virtio/xHCI MSI-X; AHCI MSI |

No panic, memory-sync timeout, Virtio network initialization failure or capability
denial was observed in those runs. The earlier `fixed-preferred-smp4-nohpet`
run also completed boot but its 90-second budget left only 18 seconds of
observation under host contention; it is recorded as incomplete, not a pass.
The standalone repeat completed the full window.

Results and hashes are in `stall-fix-results.json` and
`stall-fix-image-hashes.json`; each run retains its serial log, PCI programming
trace, command line, QMP stimulus replies, registers and stacks. Trace evidence
confirms Virtio MSI-X/INTx delivery and xHCI mode selection. No xHCI interrupt
delivery was recorded, so these boots do not establish USB traffic coverage or
Pi hardware validation.
