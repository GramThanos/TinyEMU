# TinyEMU Firmware / BIOS Requirements

This document describes what firmware, BIOS images, or kernel files are
required to run each machine type supported by TinyEMU.

---

## RISC-V (32 / 64 / 128-bit)

### Required files
| File | Purpose |
|------|---------|
| Linux kernel (`bbl` or `vmlinux`) | RISC-V Linux kernel binary |
| Root filesystem image (optional) | Block device image (`ext2`, etc.) |
| Initrd (optional) | Initial ramdisk |

### Notes
- **No separate BIOS is needed.** TinyEMU contains a built-in minimal
  RISC-V BIOS (in `riscv_machine.c`) that sets up the hardware description
  table (FDT), loads OpenSBI or the kernel directly into RAM, and jumps to
  it.
- A pre-built OpenSBI firmware (`fw_jump.bin`) can optionally be used as
  the boot loader. When not provided, TinyEMU generates a minimal FDT and
  jumps directly to the kernel entry point.
- The RISC-V machine supports RV32, RV64, and RV128 (experimental).

### Example configuration file (`root-riscv64.cfg`)
```json
{
    "version": 1,
    "machine": "riscv64",
    "memory_size": 128,
    "bios": "bbl64.bin",
    "kernel": "kernel-riscv64.bin",
    "cmdline": "root=/dev/vda rw console=hvc0",
    "drive0": { "file": "root-riscv64.bin" },
    "eth0": { "driver": "user" }
}
```

### Where to find images
- Pre-built RISC-V images: <https://bellard.org/jslinux/>
- The `diskimage-linux-riscv-yyyy-mm-dd.tar.gz` archive from
  <https://bellard.org/tinyemu/> contains ready-to-run images.

---

## x86 (32-bit protected mode)

### Required files
| File | Purpose |
|------|---------|
| Linux `bzImage` | 32-bit or 64-bit Linux kernel |
| Root filesystem image (optional) | VirtIO block device |
| Initrd (optional) | Initial ramdisk |

### Notes
- **No BIOS image is needed.** TinyEMU loads the `bzImage` directly into
  RAM and sets up the CPU in 32-bit protected mode (flat segments,
  `CS.flags = 0xc09b`, `DS.flags = 0xc093`), bypassing the BIOS entirely.
- The kernel must be compiled for x86 (`bzImage`) and must support VirtIO
  devices (VirtIO block, VirtIO net, VirtIO console, 9P filesystem).
- The machine emulates a standard PC with:
  - i440FX PCI host bridge
  - PIIX3/PIIX4 IDE / ISA bridge
  - Standard 8259A PIC, 8254 PIT, RTC (MC146818)
  - VGA framebuffer (simple framebuffer at `0xC0000000`)
  - PS/2 keyboard and mouse
  - VirtIO PCI devices

### Kernel command-line note
PCI interrupts are wired by TinyEMU without a BIOS, so kernels that rely
on ACPI or PCI BIOS for interrupt routing may need `pci=noacpi` on the
command line.

### Example configuration file (`x86-linux.cfg`)
```json
{
    "version": 1,
    "machine": "pc",
    "memory_size": 256,
    "kernel": "bzImage",
    "cmdline": "root=/dev/vda rw console=ttyS0",
    "drive0": { "file": "rootfs.bin" },
    "eth0": { "driver": "user" }
}
```

### Where to find images
- Pre-built x86 images (including Windows 2000):
  <https://bellard.org/jslinux/>
- Standard Linux `bzImage` kernels with VirtIO support also work.

---

## x86-64 (64-bit long mode)

### Required files
| File | Purpose |
|------|---------|
| Linux `bzImage` (64-bit) | x86-64 Linux kernel |
| Root filesystem image (optional) | VirtIO block device |
| Initrd (optional) | Initial ramdisk |

### Notes
- **No BIOS image is needed.** Like the 32-bit x86 path, TinyEMU loads the
  `bzImage` directly, starts the CPU in 32-bit protected mode, and the
  kernel's decompressor transitions the CPU into 64-bit long mode
  (IA-32e mode) automatically.
- The x64 software emulator supports:
  - Full 64-bit register file (RAX–R15, RIP, RFLAGS)
  - REX prefixes (REX.W for 64-bit operations, REX.R/X/B for register
    extension to R8–R15)
  - 4-level (PML4) page tables for up to 256 TB virtual address space
  - `SYSCALL`/`SYSRET` fast system call path (STAR/LSTAR MSRs)
  - `SYSENTER`/`SYSEXIT` (32-bit compatibility path)
  - FSBASE/GSBASE MSRs (used by glibc for TLS)
  - `MOVSXD`, `SWAPGS` and other 64-bit specific instructions
- The machine hardware is identical to the x86 32-bit machine.

### Example configuration file (`x86_64-linux.cfg`)
```json
{
    "version": 1,
    "machine": "pc",
    "memory_size": 512,
    "kernel": "bzImage-x86_64",
    "cmdline": "root=/dev/vda rw console=ttyS0 nokaslr",
    "drive0": { "file": "rootfs-x86_64.bin" },
    "eth0": { "driver": "user" }
}
```

### Where to find images
- Pre-built x86-64 Linux images: <https://bellard.org/jslinux/>
- Any standard x86-64 `bzImage` compiled with VirtIO support works.
- The `nokaslr` kernel parameter is recommended when using the software
  emulator for easier debugging.

---

## Summary table

| Machine | Architecture | BIOS needed? | Kernel format | Notes |
|---------|-------------|--------------|---------------|-------|
| `riscv32` | RISC-V RV32 | No | `bbl32.bin` / `vmlinux` | Built-in BIOS |
| `riscv64` | RISC-V RV64 | No | `bbl64.bin` / `vmlinux` | Built-in BIOS |
| `riscv128` | RISC-V RV128 | No | `vmlinux` | Experimental |
| `pc` (x86) | x86 32-bit | No | `bzImage` (x86) | KVM or software |
| `pc` (x86-64) | x86-64 | No | `bzImage` (x86-64) | Software emulator |

---

## Building TinyEMU

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install libssl-dev libcurl4-openssl-dev libsdl2-dev

# Build
make

# Run an x86-64 image (software emulation, no KVM)
./temu -no-accel x86_64-linux.cfg
```

The `-no-accel` flag disables KVM and forces the software x86/x86-64
emulator.
