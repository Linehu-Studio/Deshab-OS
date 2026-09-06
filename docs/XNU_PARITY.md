# XNU parity and “surpass” gates

This document is a gap analysis, not a claim that Deshab currently has XNU
parity. XNU is a production kernel with decades of hardware support, security
hardening, compatibility work, and performance tuning. Deshab is an
experimental x86_64 SAS-R0 system. “Surpass” below means passing a named,
reproducible gate on a narrow axis; it does not mean overall superiority.

## Evidence boundary

### Implemented strengths

- Limine boots `utsm.elf`, which loads staged ELF64 DKM modules and transfers
  control to the DSK kernel.
- DKM has a versioned descriptor/API, manifest-driven boot stages, an ELF64
  ET_REL loader, and working platform, storage, filesystem, network, console,
  and input modules. The current loader reaches 14 external drivers in its
  tested boot configuration.
- UTSM has segment descriptors, generation/epoch checks, capabilities,
  per-CPU key-cache structures, encrypted-buffer read/write paths, dirty-page
  accounting, and self-tests.
- DRR has an in-memory emergency pool, recovery log, CRC64 metadata, and A/B
  checkpoint metadata primitives. This is not yet durable payload rollback,
  an independent watchdog, or complete device recovery.
- The storage path includes AHCI block I/O, a small block-provider registry,
  bootfs/devfs, read-only DKM FAT32, and a limited DSK FAT32 write path.
- The e1000 path and small network stack provide a boot-time/DSK data path for
  Ethernet, ARP, DHCP, UDP, and DNS. They are not a socket stack.
- Linux compatibility code implements a VMX/EPT guest design, virtio-mmio
  backends, and a guest-specific shared-memory/hypercall protocol. The full
  nested-VMX test matrix remains pending in the repository.
- PE32+ loading and a PE32 interpreter exist with a limited Win32 shim.
  Repository test matrices still list unverified cases and known failure
  paths, so this is not general Windows binary compatibility.

### Aspirational designs

- SAS-R0-PCQ per-CPU bitmap scheduling is a design. `sched.h` and
  `sched_stub.c` define an isolated contract only; there is no scheduler boot
  hookup or context switch.
- `ipc_native.h` defines a possible native capability/message ABI. It has no
  queue implementation, syscall transport, policy, or app integration.
- `deshab/syscall.h` reserves an experimental x86_64 native syscall surface.
  There is no syscall entry point, dispatcher, user mode, or `libSystem`
  equivalent.
- UTSM’s current xorshift-derived XOR stream and XOR capability tag are not
  cryptographic confidentiality or integrity. The DRR root key is compiled
  into the image. Claims of sealed-memory security must wait for real key
  management, reviewed primitives, anti-replay checks, and adversarial tests.
- DKM unload, hotplug, driver fault domains, and DRR-driven device
  quiesce/reset/reinit are designs, not demonstrated runtime properties.

## XNU comparison

| XNU area | What XNU provides | Current Deshab state | Parity requirement |
| --- | --- | --- | --- |
| Mach IPC | Kernel-managed ports, port rights, messages, notifications, port sets, RPC patterns, and mature scheduling/accounting behavior | Linux-guest SPSC rings and hypercalls exist. They are transport-specific and are not native task IPC. The new native ABI is declarations only. | Implement lifecycle-safe capability queues, transfer/attenuation, blocking and deadlines, cancellation, notifications, accounting, and stress/fuzz tests. |
| BSD/POSIX | Processes, credentials, signals, file descriptors, sockets, users/groups, resource controls, and a broad syscall contract | Custom Ring0 programs and services; no complete native process, descriptor, signal, credential, or POSIX syscall model | Publish a supported native contract and either implement a measured POSIX subset or explicitly route unsupported Linux behavior through the guest. |
| IOKit/DriverKit | Object/service registry, device matching, lifecycle and power management, user-space driver options, entitlements, and broad hardware support | DKM can stage and relocate native Ring0 modules through a kernel API. Runtime matching, power management, hotplug, and fault isolation are absent. | Add device/service registry, deterministic bind/unbind, hotplug, power transitions, resource revocation, and measured failure recovery. |
| Mach VM/pmap | Per-task VM maps, objects/pagers, copy-on-write, protections, wired memory, shared mappings, architecture pmap, TLB coherency, and mature accounting | HHDM/MMIO mappings, DMA allocation, UTSM segments, and capability checks exist. SAS-R0 intentionally does not provide XNU-style task isolation. There is no mature VM object/pager/COW layer. | Define SAS-R0 protection semantics, complete page lifecycle and SMP TLB rules, support mapped objects/COW where promised, and prove bounds/fault behavior. |
| VFS | Uniform vnode/name cache/mount model, permissions, locking, many filesystems, journaling integration, and scalable concurrent I/O | Small bootfs/devfs/VFS paths and limited FAT32 support; path, mount, locking, cache, and durability semantics are incomplete | Specify vnode-like object and mount contracts, concurrent lookup/I/O, cache invalidation, permissions policy, recovery, and filesystem conformance tests. |
| Networking | Sockets, TCP/IP and IPv6, routing, interfaces, packet filters, QoS, observability, and many drivers | e1000 plus minimal Ethernet/ARP/DHCP/UDP/DNS functionality; virtio-net is incomplete and there is no general socket/TCP/IPv6 API | Deliver sockets, TCP, IPv6, routing, DNS behavior, packet filtering, interrupt-driven drivers, loss/reorder tests, and sustained-load benchmarks. |
| kext | Historically loadable kernel extensions with dependency, code-signing, policy, lifecycle, and diagnostics; modern macOS moves many drivers out of the kernel | DKM’s small versioned ABI and staged ET_REL loader are a useful narrow base. Signing, policy, unload/hotplug, ABI evolution tests, and containment are absent. | Add authenticated modules, compatibility negotiation, dependency/lifecycle tests, unload safety, and recoverable driver fault domains. |
| dyld/libSystem | Stable process startup, dynamic loader, shared cache, libc/POSIX runtime, Objective-C/Swift runtime integration, and tooling | PIE loading exists in selected internal paths, but native apps have no general dynamic loader, libc, stable syscall dispatcher, or ABI toolchain contract | Provide crt startup, ELF dynamic linking/TLS, libc/runtime services, versioning, diagnostics, and an SDK conformance suite. |

Architectural difference matters: XNU uses protected user tasks and multiple
security boundaries, while Deshab prioritizes a single-address-space Ring0
model. Lower transition overhead in SAS-R0 cannot be counted as a win unless
fault propagation, revocation, recovery, and whole-system tail latency are
measured too.

## Measurable “surpass” targets

Every result must record source revision, compiler flags, firmware, CPU model,
core count, memory, timer source, warm-up, sample count, and raw data. XNU
comparisons must use the same physical machine where possible and equivalent
release builds. Passing one gate permits only the corresponding narrow claim.

### S1 — DRR recovery and rollback

- Run at least 10,000 deterministic fault-injection trials per class: dirty
  page corruption, torn inactive metadata slot, torn active-slot switch,
  exhausted normal allocator, task fault, and recoverable driver fault.
- Resume a protected service with correct pre-fault state at p99.9 within
  10 ms for page rollback, 50 ms for segment rollback, and 2 s for system-slot
  rollback; report maximum as well as percentiles.
- Produce zero undetected corruptions, zero selection of a bad checkpoint, and
  zero allocations from the normal heap on the recovery path.
- On the same workload, beat XNU’s closest service restart or OS recovery path
  in p99.9 unavailable time by at least 2x. If the semantics are not
  equivalent, publish only the absolute Deshab result.

### S2 — UTSM confidentiality and integrity

- Remove the xorshift/XOR stream, XOR `auth_tag`, and compiled root key.
  Adopt reviewed KDF/encryption/authentication constructions with unique
  nonces or tweaks, explicit key erasure, boot-derived root-key handling, and
  replay-protected epochs.
- Pass published known-answer tests plus at least 10 million randomized
  read/write/rekey/rollback operations under ASan/UBSan in a host harness.
- Detect 100% of injected ciphertext, metadata, tag, and stale-epoch changes
  before plaintext release; demonstrate no plaintext in UTSM-managed backing
  pages outside documented access windows. DMA and framebuffer exceptions
  must be enumerated.
- Keep p99 4 KiB read and write overhead below 15% versus an unsealed Deshab
  buffer on hardware with the selected acceleration enabled. Report a scalar
  fallback separately; never trade away verification to meet this number.

### S3 — O(1) per-CPU scheduler

- Demonstrate by instrumentation that enqueue, dequeue, wake, and pick-next
  touch a bounded number of bitmap/list words independent of runnable-task
  count; no capability scan, MAC, checkpoint, allocation, or re-encryption may
  occur on the switch path.
- Across 1, 64, 1,024, and 65,536 synthetic runnable tasks, keep p99
  pick-next below 250 cycles and variation between the smallest and largest
  set below 5% on the reference CPU.
- Keep p99 same-CPU context switch below 1 microsecond and publish fairness,
  starvation, affinity, timer, and 24-hour stress results.

### S4 — Native capability IPC

- Validate malformed sizes, stale generations, forged object IDs, rights
  escalation, queue destruction races, cancellation, and capability transfer
  with fuzzing and at least 100 million stress operations.
- For 64-byte inline request/reply on one core, achieve median below 1
  microsecond, p99 below 3 microseconds, and at least 2 million round trips per
  second per core without dropping messages.
- Against `mach_msg` with equivalent payload, queue depth, copy semantics, and
  synchronization on the same machine, achieve at least 1.5x throughput and
  at most 0.67x p99 latency. Publish copy, transfer, and cross-core cases
  separately.

### S5 — DKM hotplug and fault containment

- Complete 1,000 attach/use/detach cycles for every hotpluggable test device
  with zero leaked mappings, vectors, DMA pages, capabilities, or callbacks.
- Inject faults at every driver entry point and DMA completion boundary.
  Restore service at p99 within 100 ms without reboot and without modifying
  memory outside the driver’s declared UTSM segments.
- Because all native code currently runs Ring0, document the mechanism that
  makes the containment claim true (for example checked entry gates plus
  recoverable memory domains, or a stronger execution boundary). A caught
  exception alone is not containment.

### S6 — Linux and PE compatibility breadth

- Linux: on a published Arch package corpus of at least 100 CLI applications,
  install and pass upstream smoke tests for at least 99%, including filesystem,
  process, signal, networking, and terminal workloads. Complete 1,000
  park/resume cycles and a 24-hour mixed workload with no Deshab crash.
- PE: on a published corpus of at least 100 PE32/PE32+ applications, load at
  least 95% and pass functional tests for at least 90%; include malformed
  binary rejection, TLS, exception, GUI/input, file, and network cases.
- A Linux guest success is reported as guest compatibility, not a native
  Linux syscall implementation. A PE image that only reaches its entry point
  is reported as “loads,” not “works.”
- This gate can support a broader-binary-coverage claim than stock XNU only
  for the exact tested corpus; it does not establish general Linux or Windows
  compatibility.

## First isolated slice

This slice intentionally changes no boot order or build integration:

- `CODE/UTSM/include/utsm/sched.h` reserves a scheduler contract and TCB/run
  queue state.
- `CODE/UTSM/sched/sched_stub.c` makes every scheduler operation fail with an
  explicit unsupported result.
- `CODE/UTSM/include/utsm/ipc_native.h` defines a bounded native IPC wire ABI,
  distinct from `CODE/utsm-ipc/ipc_proto.h`.
- `CODE/sdk/include/deshab/syscall.h` reserves experimental native syscall
  numbers and an x86_64 instruction wrapper.

None of these files demonstrates a scheduler, native IPC, syscall entry, XNU
parity, or a “surpass” result. Those claims begin only after the gates above
are implemented and reproduced.
