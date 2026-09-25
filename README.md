# QEMU DMA Lab

A custom QEMU DMA device and Linux platform driver implementing memory-mapped registers, interrupt-driven completion, coherent DMA buffers, and a userspace ioctl interface.

The project follows the complete path from low-level bare-metal bring-up to ACPI enumeration, Linux driver integration, and userspace benchmarking.

Besides normal transfers, validation covers invalid requests, timeout recovery, module unload/reload, and concurrent requests from two processes using distinct data patterns.

---

# Features

- Custom QEMU SysBus DMA device
- MMIO register interface
- Timer-based asynchronous device completion
- Interrupt-driven DMA transfers
- 32-bit bare-metal DMA bring-up
- ACPI device enumeration
- Linux platform driver
- Coherent DMA source and destination buffers
- Completion-based waiting
- Mutex protection for shared device resources
- Misc device exposed as `/dev/philip_dma`
- Synchronous userspace ioctl interface
- Invalid request validation
- Timeout recovery with ABORT
- Normal module unload/reload validation
- Latency percentile and throughput reporting
- Two-process data-integrity testing

---

# Project Structure

```text
qemu-dma-lab/
├── README.md
├── .gitignore
│
├── qemu-src/                         # Local QEMU checkout
│   ├── hw/i386/
│   │   ├── acpi-microvm.c
│   │   └── microvm.c
│   ├── hw/misc/
│   │   ├── meson.build
│   │   └── philip_dma.c
│   └── include/hw/misc/
│       └── philip_dma.h
│
├── baremetal/                        # Low-level DMA bring-up
│   ├── Makefile
│   ├── boot.S
│   ├── start.S
│   ├── linker.ld
│   ├── main.c
│   └── dma_test.c
│
├── driver/                           # Linux platform driver
│   ├── Makefile
│   └── philip_dma_drv.c
│
├── include/                          # Shared kernel/userspace ABI
│   └── philip_dma_uapi.h
│
├── userspace/                        # Userspace benchmarks
│   ├── philip_dma_test.c
│   └── philip_dma_concurrent.c
│
├── tests/                            # Functional and negative tests
│   ├── philip_dma_negative.c
│   └── philip_dma_verify.c
│
├── linux-guest/
│   ├── init
│   └── initramfs/                    # Generated guest root filesystem
│
├── patches/                          # Reproducible QEMU changes
│   ├── README.md
│   ├── QEMU_BASE_COMMIT
│   └── 0001-philip-dma.patch
│
└── docs/images/
    └── architecture.svg
```

| Directory | Purpose |
| --- | --- |
| `qemu-src/` | Local QEMU source tree containing the custom DMA device |
| `baremetal/` | Boot code, linker script, MMIO tests, and interrupt validation |
| `driver/` | Linux platform driver and kernel-module Makefile |
| `include/` | Shared kernel/userspace ioctl definitions |
| `userspace/` | Transfer benchmark and concurrent correctness test |
| `tests/` | Negative tests and additional data verification |
| `linux-guest/` | Guest initialization and initramfs packaging |
| `patches/` | Patch reproducing the custom QEMU changes from v10.2.4 |
| `docs/images/` | Architecture diagram |

The local QEMU checkout and generated initramfs are development artifacts. The reproducible QEMU changes are preserved in `patches/0001-philip-dma.patch`.

---

# System Architecture

![QEMU DMA Lab Architecture](docs/images/architecture.svg)

Userspace submits a synchronous ioctl request through `/dev/philip_dma`. The Linux driver prepares coherent buffers and configures the QEMU DMA device through MMIO. The device performs the memory copy and signals completion through an IRQ.

---

# Bare-Metal DMA Bring-up

Before Linux integration, the custom DMA device was validated with 32-bit bare-metal code running on QEMU microvm. This stage exercises the hardware/software boundary without Linux APIs, platform-driver helpers, or a filesystem.

This is relevant to SoC, NPU, accelerator, and firmware work because it demonstrates how software brings up an engine directly through memory-mapped registers, guest memory, and interrupts before full operating-system integration.

## Validation Scope

- Assembly entry and linker-script controlled memory layout
- Direct MMIO register access
- DMA source, destination, and length programming
- Device status polling
- Guest-memory copy verification
- IOAPIC routing and interrupt-vector setup
- IDT and ISR execution
- Completion flags and interrupt counters

## Memory-Copy Test

| Parameter | Value |
| --- | --- |
| Source address | `0x110000` |
| Destination address | `0x120000` |
| Transfer length | 16 bytes |

QEMU monitor inspection confirmed that the destination contained the expected copied data.

## Interrupt Test

The interrupt test uses GSI 16 and routes it to vector `0x2E`.

| Variable | Result |
| --- | --- |
| `dma_irq_count` | 0 → 1 |
| `dma_irq_seen` | 0 → 1 |
| `dma_test_pass` | 0 → 1 |
| `dma_test_fail` | Remained 0 |

```text
Servicing hardware INT=0x2e
```

This stage validated MMIO, memory movement, interrupt routing, and ISR execution before ACPI enumeration and Linux driver integration.

---

# QEMU DMA Device Model

The device is implemented as a QEMU SysBus device and integrated into the microvm machine.

| Resource | Configuration |
| --- | --- |
| MMIO base | `0xFE800000` |
| MMIO size | `0x1000` bytes |
| Source register | `0x00`, 64-bit |
| Destination register | `0x08`, 64-bit |
| Length register | `0x10` |
| Control register | `0x14` |
| Status register | `0x18` |
| IRQ status | `0x1C`, write-one-to-clear |
| IRQ enable | `0x20` |
| Completion model | QEMU virtual-clock timer |

The latest confirmed source setting is:

```c
#define DMA_DELAY_NS 10000ULL
```

This represents a nominal 10 us simulated device delay.

---

# Linux Driver Integration

The device is described through ACPI HID `PHL0001` and matched by a Linux platform driver.

| Component | Implementation |
| --- | --- |
| Enumeration | ACPI platform device |
| Register mapping | `devm_ioremap_resource()` |
| DMA memory | Coherent source and destination buffers |
| Completion | Linux `completion` object |
| Interrupt | Linux IRQ handler with W1C acknowledgement |
| Userspace interface | Misc device `/dev/philip_dma` |
| Shared-engine lock | `xfer_lock` mutex |

## Transfer Path

1. Userspace fills `src[]` and calls `PHILIP_DMA_IOC_TRANSFER`.
2. The driver copies the request into a private kernel allocation.
3. `xfer_lock` serializes access to the shared DMA engine.
4. Source data is copied into the coherent source buffer.
5. DMA addresses, length, and control registers are programmed.
6. The driver waits with `wait_for_completion_timeout()`.
7. The IRQ handler clears `IRQ_STATUS` and calls `complete()`.
8. Destination data is copied into the private request and returned to userspace.

The shared request structure is:

```c
struct philip_dma_ioc_transfer {
    __u32 len;
    __u32 reserved;
    __u8 src[PHILIP_DMA_MAX_LEN];
    __u8 dst[PHILIP_DMA_MAX_LEN];
};
```

The maximum transfer length is 4096 bytes.

## Timeout Recovery

The driver handles delayed completion and missing completion interrupts. Recovery aborts the previous operation, clears device state, and returns an error for the timed-out request. A later normal transfer can then succeed.

---

# Build and Run

## Linux Driver

```bash
make -C /lib/modules/"$(uname -r)"/build \
    M="$PWD/driver" modules
```

## Concurrent Test

```bash
gcc -O2 -Wall -Wextra -std=c11 -static \
    -I./include \
    userspace/philip_dma_concurrent.c \
    -o userspace/philip_dma_concurrent
```

## Install into the Initramfs

```bash
cp driver/philip_dma_drv.ko \
   linux-guest/initramfs/lib/modules/

cp userspace/philip_dma_concurrent \
   linux-guest/initramfs/bin/
```

```bash
(
    set -o pipefail
    cd linux-guest/initramfs || exit 1
    find . -print0 |
        cpio --null -o --format=newc |
        gzip -9 > ../initramfs.cpio.gz
)
```

## Run in the Guest

```sh
insmod /lib/modules/philip_dma_drv.ko

/bin/philip_dma_concurrent
echo "exit code=$?"

dmesg | tail -n 40
```

---

# Performance Evaluation

The experiments run in a Linux guest under QEMU TCG, with QEMU itself running inside an Ubuntu VirtualBox VM.

Reported performance includes the effects of the driver, device emulation, interrupt delivery, and scheduling across the virtualized environment. These measurements do not represent physical DMA hardware performance.

The benchmark reports:

- Average latency
- Minimum latency
- P50, P95, and P99
- Maximum latency
- Operations per second
- Throughput in MiB/s

Earlier work included logging-on/off comparisons, five repeated runs for 16-, 64-, and 256-byte requests, and a nominal device-delay sweep from 1 ms to 100 us and 10 us.

The latest confirmed source delay was `DMA_DELAY_NS = 10000ULL` (10 us).

---

# Benchmark Methodology

## Two-Process Transfer Benchmark

Two instances of the benchmark are launched in the background.

```sh
/bin/philip_dma_test 4096 > /tmp/dma_A.log 2>&1 &
pid_a=$!

/bin/philip_dma_test 4096 > /tmp/dma_B.log 2>&1 &
pid_b=$!

wait "$pid_a"
rc_a=$?

wait "$pid_b"
rc_b=$?

echo "A exit code=$rc_a"
echo "B exit code=$rc_b"

cat /tmp/dma_A.log
cat /tmp/dma_B.log
```

Configuration:

- Processes: 2
- Transfer size: 4096 bytes
- Iterations per process: 1000
- Interface: synchronous ioctl
- Shared DMA engine protected by `xfer_lock`

## Concurrent Data Integrity

The dedicated correctness test uses `fork()` to create two processes. Each independently opens the device.

Before every transfer, the processes exchange ready tokens through `socketpair()`.

Each source pattern includes:

- A process identifier
- An iteration number
- Position-dependent data

After every ioctl, all 4096 destination bytes are compared against an independent expected buffer.

---

# Benchmark Results

## Two-Process Transfer Benchmark

Recorded run: 4096-byte requests, 1000 iterations per process.

| Metric | Process A | Process B |
| --- | ---: | ---: |
| Average | 642.305 us | 735.539 us |
| Minimum | 77.051 us | 148.794 us |
| P50 | **521.272 us** | **593.104 us** |
| P95 | 1427.140 us | 1575.929 us |
| P99 | **2165.106 us** | **2380.764 us** |
| Maximum | 23504.497 us | 26662.786 us |
| Operations/s | 1556.89 | 1359.55 |
| Throughput | 6.082 MiB/s | 5.311 MiB/s |
| Exit code | 0 | 0 |

Both processes completed successfully.

Median latency was approximately **0.52–0.59 ms**, while P99 reached approximately **2.17–2.38 ms**.

Maximum values reached approximately **23.5–26.7 ms**, showing a substantial latency tail in this run.

## Concurrent Data Integrity

| Metric | Process A | Process B |
| --- | ---: | ---: |
| PID | 72 | 73 |
| Transfer size | 4096 bytes | 4096 bytes |
| Verified transfers | 1000 / 1000 | 1000 / 1000 |
| Data mismatches | 0 | 0 |
| Elapsed time | 0.831 s | 0.835 s |

Recorded output:

```text
[A] ready: PID=72, len=4096, rounds=1000
[B] ready: PID=73, len=4096, rounds=1000
[A] PASS: 1000/1000 transfers verified, 4096 bytes each, elapsed=0.831 s
[B] PASS: 1000/1000 transfers verified, 4096 bytes each, elapsed=0.835 s
OVERALL PASS: two processes, 2000 transfers, distinct patterns, zero mismatches
exit code=0
```

**All 2000 transfers passed byte-by-byte verification.**

Elapsed time includes pattern generation, synchronization, ioctl execution, and verification. It is not a measurement of pure DMA execution time.

---

# Functional Validation

| Test | Observed Result | Status |
| --- | --- | --- |
| Bare-metal memory copy | Expected data observed in destination memory | PASS |
| Bare-metal interrupt and ISR | IRQ counter and completion flags updated | PASS |
| Zero-length request | `EINVAL`; subsequent normal transfer succeeds | PASS |
| Oversized request: 4097 bytes | `EINVAL`; subsequent normal transfer succeeds | PASS |
| Unsupported ioctl | `ENOTTY`; subsequent normal transfer succeeds | PASS |
| NULL request pointer | `EFAULT`; subsequent normal transfer succeeds | PASS |
| Delayed DMA completion | Timeout, ABORT recovery, then successful normal transfer | PASS |
| Missing completion IRQ | Timeout cleanup, then successful normal transfer | PASS |
| Normal module unload | ABORT, idle engine, and IRQ handler release confirmed | PASS |
| Device node after unload | `/dev/philip_dma` disappears | PASS |
| Reload | Normal transfer succeeds | PASS |
| Unload with an open descriptor | Rejected; exit code 1 | PASS |
| Unload after closing the descriptor | Successful; exit code 0 | PASS |
| Concurrent distinct-pattern transfers | 2000 transfers; zero mismatches | PASS |

Confirmed unload messages:

```text
ABORT processed, timer cancelled, state cleared
remove: engine idle, IRQ handler released
driver removed
```

---

# Discussion

## Bare-Metal and Linux Validation

Bare-metal testing validated the device directly through MMIO, memory access, and interrupts.

Linux integration added ACPI resource discovery, coherent DMA allocation, process-context waiting, IRQ completion, timeout recovery, and a userspace ABI.

Together, the two stages demonstrate both low-level firmware-style bring-up and operating-system driver integration.

## Userspace-Visible Latency

Transfer latency can include request copying, mutex waiting, coherent-buffer preparation, MMIO access, emulated device delay, interrupt processing, scheduling, and result copying.

A configured device delay is not the same as observed ioctl latency.

## Shared Resource Synchronization

The two-process correctness test supports the effectiveness of the current mutex-protected transfer path under the tested workload.

No cross-process or stale-pattern corruption was observed.

---

# Current Status

Implemented and validated:

- QEMU DMA device and MMIO register interface
- Bare-metal DMA and interrupt validation
- ACPI enumeration and Linux driver integration
- Userspace ioctl transfers
- Latency percentile and throughput reporting
- Invalid request handling
- Timeout recovery for delayed completion and missing IRQ
- Normal module unload/reload
- Open-descriptor module unload protection
- Two-process correctness testing with distinct patterns

Documentation follow-up:

- Archive the original benchmark and validation logs.
- Record matching source commits and binary identities.
- Record the exact guest configuration.
- Consolidate historical logging comparisons and delay-sweep results.
- Document the complete QEMU build and guest launch command.

---

# Future Work

Potential extensions include:

- More processes and mixed transfer-size stress tests
- sysfs unbind and outstanding-descriptor lifetime handling
- Transfer/removal concurrency testing
- CPU affinity and scheduling experiments
- Native Linux or KVM comparison where available
- Kernel and QEMU tracing to separate latency contributions
- Asynchronous userspace submission and completion notification
- CSV result export and automated benchmark visualization

---

# License

MIT License
