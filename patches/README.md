# QEMU Device Patch

This patch contains the custom Philip DMA device and its microvm,
ACPI, and build integration.

Base release: QEMU v10.2.4.

The exact base commit is recorded in `QEMU_BASE_COMMIT`.

## Apply

Run from the qemu-dma-lab repository root, using a fresh QEMU checkout:

```bash
git clone --branch v10.2.4 --depth 1 \
    https://gitlab.com/qemu-project/qemu.git qemu-src

git -C qemu-src rev-parse HEAD
cat patches/QEMU_BASE_COMMIT

```
