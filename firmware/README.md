# Firmware resource descriptions

## Shared device tree

`FirmwareInitialize` owns the single DT mapping for deviced. `FdtTreeCreate`
validates the complete structure once, caches properties and decoded resource
records, and builds the provider index. Node offsets index the preorder records;
provider lookup uses sorted phandles and rejects duplicates, including disabled
duplicates. Console aliases and `stdout-path` options resolve during construction.

PCI hosts and the native platform inventory retain this tree. RP1 uses its PCI
host's tree and visits only that host's subtree. Resource visitors receive cached
root-to-node records in preorder; they do not invoke the DT parser. Property
views require both the parsed inventory and its immutable mapping to stay alive.
`FirmwareShutdown` refuses teardown while consumers hold references and preserves
ownership after a failed unmap for retry.

Run `testing/firmware/shared_tree_test.ps1 -Python <python-executable>` on Windows
or `bash testing/firmware/shared-tree-test.sh` on Unix. These cover both bundled Pi
DTBs, console selection, indexed lookups across entry-array growth, subtree
boundaries, one parse with no lookup reparsing, allocation failure at every
construction/publication step, provider lifetime, and failed-unmap retry.
The common `FdtDescribePlatformDevice` API owns native resource decoding and
validation. Direct tests cover translated register extents, overflow, incomplete
interrupt tuples, extended interrupt precedence, unsupported sources, capacity
limits and unchanged output after errors. Platform enumeration consumes these
descriptions and supplies publication policy and provider readiness flags.
The existing PCI/RP1 boundary fixtures explicitly rebuild snapshots after editing
their test bytes; production consumers never mutate or rebuild the live tree.

Broadcom MSI/reset/clock/link bindings and RP1 child-register, interrupt and DMA
bindings are implemented in `services/deviced/firmware/bcm.c` and `rp1.c`.
The firmware library has no dependency on the PCI/RP1 bus libraries or their
headers. RP1 bus code selects assigned memory BARs and passes CPU ranges to
the decoder; the decoder validates every child's register extent against those
ranges before emitting descriptions. Boundary tests reject missing, truncated
and overflowing ranges without emitting children, and ensure unassigned BARs
and I/O BARs do not become memory grants.

## Native platform runtime

Native platform providers now implement the resource lifecycle in addition to
publication. `DevicePrepare` resolves the device's and ancestor buses' dependency
plans in the common firmware layer, then acquires provider tokens in power,
supply, clock, reset, default-pin-state and PHY order for each ancestor. Release
runs in reverse order. Provider callbacks run under the platform lock, may not
re-enter platform APIs, and must implement their hardware's shared-user rules.
Failed acquisition unwinds earlier tokens; failed release preserves that token
and its prerequisites for retry. Unknown owners return unsupported before any
acquisition. `PlatformDependencyRegister` is a trusted service integration API,
not an unprivileged claim of arbitrary firmware nodes.

Dependency-free devices and validated ungated fixed clocks need no additional
backend. Programmable clocks, resets, power, regulators, pins and PHYs require a
registered hardware owner. Assigned-clock programming and legacy GPIO/PHY
activation bindings remain explicitly unsupported. The framework does not infer
readiness from firmware having left a device running. `DeviceStart` validates that
native preparation completed; the peripheral driver starts its own engine. `DeviceStop`
requires a halted engine and released IRQ/DMA leases before releasing dependencies.
`Pending` is the publication snapshot; successful runtime operations establish
readiness, rather than editing a descriptor copy.

Direct native interrupts use one kernel fixed-line group per firmware source.
`DeviceInterruptSelectSource` chooses the list index before Allocate. The query
resolves the GIC with the kernel; allocation explicitly validates that controller
again. A provider attachment checks process, device, INTID and trigger flags. The
kernel owns mask/unmask, handler synchronization and EOI. No provider completion
callback is needed for a direct SPI. The platform owner retains each attachment
until Close succeeds; uncertain replies keep cleanup retryable. PCI/manual INTx
allocations retain their previous routing behavior. GPIO/cascaded domains require
their own interrupt providers.

Native DMA begins with validated DT RAM extents and composes every ancestor's
explicit simple-bus `dma-ranges`. Missing mappings, PCI/IOMMU paths and invalid
RAM fail without changing output. Cache policy remains software-managed. A
session must match the activation principal. The registry pairs successful
PrepareDma with ReleaseDma, including commitment failure, retaining the provider
through rollback. Native activation cannot stop while a DMA guard remains.
Drivers still use the existing kernel DMA buffer API and must halt DMA before
closing contexts; addressability alone does not start or authorize an engine.

`shared_tree_test.ps1` covers acquisition order, every acquisition failure,
release retries, missing owners, immutable clocks, ancestor dependencies, DMA
translation/rejection, principal isolation and busy shutdown. The interrupt
registration suite runs the real DDK and kernel group lifecycle for native SPIs,
including incorrect controller/line rejection and handler delivery. Run
`native_platform_compile.ps1` for separate translation-unit and wire-layout
checks on i386, amd64 and ARM64. Rebuild kernel, DDK and deviced together because
the interrupt request/query/attachment layouts changed. Tests use hardware
models; physical-board acceptance remains required.

## Device-service diagnostics

Startup returns `DmStartupResult`, with separate error and outcome fields.
The service schedules another attempt only for `DmStartupRetry`. Combining
independent owners preserves the first error and requests retry whenever any
owner has unfinished work. Cleanup failures request retry even when their error
code would normally indicate an unsupported or invalid setup.

Terminal firmware errors are remembered until `FirmwareShutdown` succeeds.
Terminal PCI host errors stay blocked until explicit unpublication or destruction;
other hosts can still finish. Platform discovery remembers terminal errors for
the service lifetime. Publication still waits for the complete host inventory and
all of its hardware preparation before enabling driver binding.

Service startup supplies the ordered PCI function-handler list. Repeated PCI
initialization preserves the original list and live hosts. PCI has no dependency
on RP1; the service links both. BCM policy decoding runs only for BCM host types,
so BCM-only properties cannot reject an otherwise valid ECAM description.

`publication_service_test.c` and `startup_outcome_test.inc` cover explicit outcomes,
mixed terminal/retryable failures, stable IDs, handler selection and binding order.
The shared-tree fixture checks terminal parse caching across cleanup retries;
the PCI fixture checks BCM policy rejection and ECAM policy isolation.

The device service uses `ddk/utils.h` for diagnostics. Enable `__TRACE` in the
relevant source file, or define it for a diagnostic build, to trace discovery,
publication, resource setup and teardown. Warnings and errors remain enabled
without tracing. Register polling and ordinary absent devices/properties do not
produce per-iteration warnings.

Failures identify the operation and, where applicable, the register, entry,
resource or firmware property involved. Best-effort cleanup logs secondary
failures without replacing the original result; retryable teardown retains its
existing ownership rules.

The reader, DMA, RP1 interrupt, PCI host, platform-device and device-provider
fixtures capture diagnostics as well as checking return codes and state. The
PCI fixture checks that injected BCM2711 and BCM2712 write failures name the
failing setup function and register. The firmware fixtures also check that
absent optional properties remain quiet.
Compile with and without `-D__TRACE` to exercise both diagnostic configurations.
These are host-model checks, not hardware boot or DMA acceptance tests.

The platform-device test binary accepts `--diagnostics-only` to run its focused
diagnostic checks separately from the full fixture. Without this argument, the
full fixture still runs.

## Running the fixtures

Run the PCI resource and interrupt descriptor checks with:

```sh
ASAN_OPTIONS=detect_leaks=0 bash testing/firmware/test.sh --resources-only
```

The default invocation also runs the kernel firmware user-copy tests. Leak
checking is disabled in the command above because LeakSanitizer cannot run under
the sandbox's process tracing; address and undefined-behavior checks remain on.

Run the isolated descriptive DMA tests with:

```sh
ASAN_OPTIONS=detect_leaks=0 bash testing/firmware/test.sh --dma-only
```

This mode runs independent header/source compilation, the reader and pure DMA
tests, RP1 interrupt checks, and the DMA-specific PCI fixture entry point in both
firmware-only and legacy-PCI builds. It includes modeled host setup for the
configured DMA queries, but does not run the complete controller or registry
integration suites. `dma_test.c` links the actual decoder/composer as
separate translation units. Cases cover simple/PCI encodings, attribute handling,
missing versus empty properties, aliases, partial intersections, gaps, sorted
results, one-byte boundary crossings, maximum addresses, invalid lengths,
overlap, capacity limits and unchanged outputs after failure.

`rp1_dma_test.inc` adds both bundled Pi 5 USB controllers, relocated/clipped
windows, peer/MSI exclusion, unsupported IOMMU/child-bus paths, disabled/missing
children, exact host ancestry, independent hosts and duplicate RP1 buses. It also
checks shared traversal behavior: mismatched host types fail, a late invalid
interrupt produces no enumeration callbacks but does not block DMA resolution,
and missing DMA ranges do not block enumeration. Resolution performs no
controller writes, publication or binding.
The ordinary PCI suite includes these cases too. No test establishes actual
hardware DMA or cache visibility.

`pci_dma_test.inc` reuses the PCI register model and the real host initialization
functions. It checks the configured description before and after registration,
unsupported ECAM/legacy backends, separate inbound setup availability, and failure
both during and after inbound programming. Two independently registered hosts
verify identity, relocated PCI ranges and continued operation after a sibling is
destroyed. Pi 4's 3 GiB range remains 3 GiB in the result despite a 4 GiB hardware
mapping. The Pi 5 test uses its bundled firmware and a smaller, relocated RAM
window actually passed to the setup code: both RP1 USB queries must use that
effective map rather than the larger inventory firmware description. Peer and
interrupt windows stay excluded, failures preserve the caller's output, queries
perform no hardware writes, and RP1 child activation remains pending. These tests
establish software behavior against modeled registers, not board acceptance.

On the 2026-10-08 working tree, the broader `--resources-only` run stops at
`__BcmAcceptance`'s sleep-failure assertion (`g_sleepFailure = -1`): the current
`BcmPciDelay` helper returns void rather than propagating that injected failure.
The DMA-only mode passes independently; that existing controller mismatch is
not changed by the DMA resolver.

`pci_host_test.c` checks synthetic trees and the bundled Pi 4/Pi 5 DTBs in both
legacy-PCI and firmware-only builds. It exercises high addresses, prefetchable
windows, inherited and extended named interrupts, MIP ranges and offsets, bridge
reset selectors, clocks, and link policy. Providers can occur after their users.
Disabled providers and ancestors, missing and duplicate phandles, truncated
specifiers, malformed scalars, invalid counts, and address overflow are rejected.
Host registration checks stable `PciHostIdentification.HostId` values, overlapping
and adjacent bus intervals, identical ranges across segments, repeated registration,
invalid operations, ID exhaustion, and allocation failure without ownership transfer.
Removing a host permits its range to be registered again with a different ID.
Firmware tests mix explicit and automatic domains in both traversal orders,
including disabled hosts and a maximum-width explicit domain.
The Broadcom regression runs two segments with identical bus/device addresses
and separate register banks. It checks per-host locks, root ownership, interrupt
swizzling, MMIO and DMA translation, normal endpoint activation, failed initialization,
and destruction of one host while the other remains usable. The shared firmware
mapping is released only after the last host is destroyed. BCM2712 tests check
bridge reset bank isolation, shared RESCAL, PHY setup and acknowledgement,
bounded link training and failure cleanup, all ten inbound slots, and 64-bit
outbound/bridge programming. The real Pi 5 tree's `0xfffffffc` window succeeds,
but its rounded hardware padding never translates into usable resources.
The firmware-only build also models enabling the external link in a copy of the
Pi 5 tree and discovers it alongside RP1 through the real host callback and
generic scanner. BAR tests cover assigned 32/64-bit resources, explicit
unassigned-resource diagnostics, and complete-resource boundary checks.
See [the BCM2712 guide](../../docs/bcm2712-pcie.md) for a short explanation and
the detailed controller sequence. Physical Pi 5 acceptance remains outstanding.

Run only the Pi 5 controller and related firmware checks with:

```sh
ASAN_OPTIONS=detect_leaks=0 bash testing/firmware/test.sh --bcm2712-only
```

This uses the existing Pi 5 tests without running Pi 4 controller assertions.
It still compiles headers and service sources separately in both PCI build
configurations and runs the reader, address-description and RP1 interrupt tests.
The Pi 5 controller tests then run in both configurations; the firmware-only
build also runs discovery and scanning of RP1 and the enabled external link.
The reset-register mock rejects a second registration of the same provider
range. Both hosts must instead retain one shared mapping. Tests verify both
shutdown orders, a sibling failing after it borrows an already-live mapping,
first-registration and acquisition failures, and final mapping cleanup. These
checks model kernel exclusivity and software ownership, not physical hardware.
This mode exits before the platform registry and kernel user-copy tests.

`rp1_test.inc` adds real Pi 5 child resource checks and scanner attachment to
`pci_host_test.c`. It covers both USB controllers, Ethernet, GPIO, disabled
children, assigned BAR boundaries, relocation, malformed interrupts, duplicate
controller phandles, inventory ownership and the RP1 activation gate. Handler
checks cover ID/revision matching, rejected firmware associations and BARs,
opaque attachment teardown, and blocked activation/configuration writes even
without an attachment. Failure injection at every parent/child publication step
checks complete rollback without binding. Attachment builds inventory only;
PCI stages ancestry and RP1 children before enabling child binding.
`rp1_interrupt_test.c` models RP1 source controls, parent masking and
synchronization, posted-write ordering, edge/level handling, independent
controllers, and failure cleanup. These tests do not supply a runtime MIP/MSI-X
transport. See [RP1 support](../../docs/rp1.md) for the integration boundary.

`platform-test.sh` generates the real device/driver bindings and exercises
`platform_device_test.c` against the actual registry, discovery and YAML parser.
It verifies compatible preference, numeric/platform separation, bounded platform
descriptors, 64-bit serialization, pending readiness flags, late binding, failed
startup/send retries and cancellation on removal. Host libyaml is required.
The main resource suite invokes it automatically. RP1 publication tests inject
failure at each parent/child creation and check rollback before binding begins.

`rp1_provider_test.inc`, included by `device_provider_test.c`, runs the actual
RP1 provider callbacks against the actual registry and publication group. Small
inventories are supplied directly because other tests cover firmware parsing.
It checks parent association, failed reference acquisition, registration rollback
before and after retaining a provider, unsupported control/register requests,
unchanged pending flags, and removal while a registry request is active. An extra
provider reference must keep the inventory alive after all IDs have been removed.
Two independent parents check that releasing one inventory cannot release another.
The tests also check that callbacks run outside the registry lock.

`rp1_lifetime_test.inc` uses a real Pi 5 firmware inventory in the PCI host model.
It fails every publication position and checks balanced references after rollback.
It then removes registry entries while retaining one child provider: host teardown
must return busy and keep the endpoint, inventory and firmware mapping intact.
An injected busy attachment callback checks that PCI honors destruction errors
before unlinking the endpoint. Releasing the reference and retrying cleanup must
release the firmware exactly once. This test runs in `--dma-only` in both PCI
configurations. Run `platform-test.sh` as well for the real registry coverage.

`interrupt_identity_test.c` checks device identity and line routing propagation,
the PCI predicate derived from the bus type, and empty initialization.

The Pi 4 DTB in the repository is a template with an empty RAM bank. Its DMA
aperture remains unknown until the test models firmware's memory-size fixup.
The Pi 5 tree describes a peer aperture, RAM aperture, and MIP doorbell; their
ordering does not determine their classification.

## Module and boundary checks

See [firmware discovery](../../services/deviced/firmware/README.md) for the reader,
resource, PCI, Broadcom, and RP1 boundaries and the shared parser build contract.
`reader_test.c` links the reader, resource decoder, and shared parser as separate
translation units. It checks unknown bindings, copied borrowed views, forward
provider lookup, generic provider counts, unchanged outputs on failure, duplicate
identity, truncation, and rejection of late errors before callbacks.

BAR tests also inspect probe results before registration: bus/CPU translation,
64-bit attributes and lower-slot identity, retained unassigned sizes, assignments
outside windows, complete restoration, and rejection of a 64-bit BAR in the last
slot. RP1 receives the same `PciBar` description used by diagnostics and resource
registration.

## Description API

`services/deviced/firmware/fdt.h` includes the separate adapter descriptions:

- `FdtPciWindow.Attributes` retains the complete PCI `phys.hi` cell, including
  `FDT_PCI_PREFETCHABLE`. Bus addresses, physical addresses, and lengths are
  64-bit throughout.
- `FdtPciWindow.Kind` distinguishes RAM ranges, peer mappings, MSI doorbells,
  and unknown mappings. A RAM aperture is identified by its relationship to an
  enabled firmware memory bank; it can exceed installed memory and does not
  authorize allocation throughout the aperture. BCM2711 DMA consumers require
  a RAM classification.
- `FdtResolvePciNamedInterrupt` resolves a named GIC SPI, preferring
  `interrupts-extended` over `interrupts` and validating name/specifier counts.
- `FdtResolvePciMsi` follows `msi-parent` in the same blob. Supported providers are
  Broadcom host MSI controllers and BCM2712 MIP. It describes resources without
  allocating vectors or programming hardware. Unsupported providers fail closed.
- MIP's first register is translated to CPU physical space. Its second register
  is retained as the PCI message address. `Interrupt.Line` is the declared SPI
  base including the GIC SPI bias; message index `n` targets
  `Interrupt.Line + Offset + n`. `InterruptCount` is the number of messages.
- `FdtResolvePciDependencies` resolves named RESCAL and bridge reset entries using
  each provider's cell count, including the bridge reset ID and physical range.
  It also retains support for the named fixed reference clock.
- `FdtPciHost.Link` records maximum speed, lane count, L0s policy, spread-spectrum
  enablement, and CLKREQ mode. Absent speed/width values stay zero.

All references resolve against the supplied blob, including disabled-ancestor
checks. Keep that blob mapped for the lifetime of any borrowed descriptions.
Resolver output is published only after the requested description validates.

The MIP message/register distinction and offset semantics follow the
[Raspberry Pi MIP driver](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/irqchip/irq-bcm2712-mip.c).
Runtime discovery does not use bundled paths, hardcoded phandles, or board
addresses.

`BusDevice_t.Type` identifies generic (`BusTypeGeneric = 0`) and PCI
(`BusTypePci = 1`) devices. `DeviceInterrupt_t.IsPci` is derived using an explicit
PCI comparison; segment/BDF coordinates remain bus metadata. The protocol uses
the matching `sys.bus_type` enum and retains the former 0/1 wire encoding.
Rebuild generated protocols, the device service, runtime and drivers together
for the renamed field. ARM64 MSI allocation/programming remains unsupported;
these descriptions supply the identity and resources needed by that
implementation.

`pci_boundaries_test.inc` covers unregistered constructors without scan/publication
side effects, failed mapping cleanup, repeated initialization with live hosts,
registration rejection cleanup, sparse legacy root functions and duplicate scans.
It also checks retained host/bridge/endpoint IDs, publication rollback, idempotent
binding, and a busy descendant retaining host ownership until teardown is retried.

Tests for the unavailable `DeviceInterruptMsiControllerRegister` and
`DmPciQuiesceDevice` APIs were removed from the current host harness. Firmware
MSI description and interrupt identity coverage remains; runtime MSI activation
is still gated and is not claimed by these tests.

`device_provider_test.c` uses the real registry and PCI provider. It checks two
provider implementations, missing operations, wrong description kinds, failed
allocation/retention, and rejection of children under a removed parent. Requests
that attempt removal must finish before the provider reference is released; new
requests and binding are rejected until removal is retried. The checks also cover
USB compatibility, driver-forwarded requests, and PCI control without a lookup
list, including configuration limits and host/handler activation checks.
`pci_boundaries_test.inc` verifies that a retained PCI function prevents host
teardown even after its registry entry has been removed.

`publication_test.inc` exercises the shared publication group with the real device
registry. It checks failures while allocating group records or registry entries,
provider reference cleanup, driver matching only after the group is finished,
and binding retries that skip earlier successes. Removal during a provider
request removes a newer sibling first, keeps the busy child and its parent alive,
and blocks publication and binding until removal finishes. The PCI/RP1 tests
also check that every description exists before binding begins, including when
binding a later RP1 child fails and is retried.

### Header and lifecycle boundaries

Every PCI, host, RP1, and fixed-device header is compiled on its own, with and
without legacy PCI enabled. PCI, RP1, and firmware sources are also compiled independently;
`platform-test.sh` checks the registry, discovery, and service entry point. No PCI-directory include
path is supplied. The production CMake target remains one service.

| Boundary | Harness coverage |
| --- | --- |
| Invalid firmware and validation before callbacks | `reader_test.c`, `pci_host_test.c`, `rp1_test.inc` |
| Controller construction failures and conflicting host ranges | `pci_boundaries_test.inc`, `pci_host_test.c` |
| Self-loops, multi-bridge cycles, duplicate routes, out-of-range buses | `pci_boundaries_test.inc` through the real PCI scanner |
| Publication failure after several children | `rp1_test.inc` at every add step; `publication_test.inc` with real registry entries |
| Failed removal preserves ownership | `pci_boundaries_test.inc`, `publication_test.inc` |
| Requests overlap removal | `device_provider_test.c`, `publication_test.inc` with the real registry |
| No driver binding before the group is finished | `publication_test.inc`; `platform_device_test.c` with real registry and driver discovery |
| Exact vendor/product match wins over a class match | `platform_device_test.c`, in both driver list orders |
| Firmware-compatible preference | `platform_device_test.c` with real driver selection |

The host/RP1 hardware models still use a registry substitute to inject hardware
and publication failures. Registry ownership, removal, and matching checks run
separately against the real implementation. Physical hardware validation remains
a separate step.

### DMA description leases

`platform-test.sh` exercises `DmDevicePrepareDma` against the real registry in
`dma_lease_test.inc`. It checks unsupported providers, absent devices, allocation
failure, provider failure, count overflow, multiple independent leases, occupied
output slots and repeated release of the same empty slot. Removal is invoked
inside the provider callback to reproduce the point where a concurrent removal
can mark the device unavailable. Both successful and failed callbacks leave
removal retryable, with no escaped lease or early provider release. These tests
check that callbacks run outside the registry lock; they do not stress a real
thread scheduler.

`rp1_provider_test.inc` prepares through the real RP1 provider and registry, with
a controlled configured-query substitute that checks the exact parent and child
node. It verifies that the lease keeps publication removal busy, retains the
parent, and preserves both pending flags. Releasing it allows cleanup to finish.
The DMA-only PCI fixture separately exercises that callback with the real
configured RP1 query and modelled Broadcom registers, checks the copied map and
host identity, and rejects a stopped host without new hardware writes. These are
service lifetime and description tests, not evidence of hardware DMA readiness.

### RP1 peripheral ownership

RP1 publishes the original ordered firmware compatible list and complete validated
register ranges. Its resource response includes the retained PCI parent's identity;
the `DeviceResources` ioctl layout requires providers and drivers to be rebuilt
together. Peripheral activation does not program DWC3 registers or disable shared
bus mastering when one child stops.

The xHCI driver's DWC3 integration accepts only the supported RP1 C0 parent and
host-only DWC3 identity. It owns the PHY, host-mode and AXI settings and supplies a
bounded xHCI register window. Other DWC3 integrations require their own supported
configuration before this driver will touch their registers.

`device_resource_test.inc` checks full resource ranges, parent metadata, generic
activation and resource-destruction retries. `rp1_provider_test.inc` checks that
publication preserves firmware identity. `../dwc3_test.c` exercises unsupported
parents and IP, partial preparation, readback failures and restoration retries.
`../usb_controller_cleanup_test.c` checks that wrapper restoration happens after
xHCI, interrupt and DMA shutdown and before provider stop, including failed setup
and later stop/unmap retries. It also bounds legacy capabilities to the xHCI
window. Run these with `rp1_runtime_test.ps1` and `../device_dma_test.ps1`.
