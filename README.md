# TinyEMU
A system emulator for the RISC-V and x86 architectures.

This repository hosts a modified version of Fabrice Bellard's [TinyEMU](https://bellard.org/tinyemu/) and it focus on its port to JavaScript/WebAssembly.


## Features:
- RISC-V system emulator supporting the RV128IMAFDQC base ISA (user level ISA version 2.2, priviledged architecture version 1.10) including:
  - 32/64/128 bit integer registers
  - 32/64/128 bit floating point instructions (using the SoftFP Library)
  - Compressed instructions
  - Dynamic XLEN change
- ~x86 system emulator based on KVM~ (Not supported in JavaScript)
- VirtIO console, network, block device, input and 9P filesystem
- Graphical display with SDL
- JSON configuration file
- Remote HTTP block device and filesystem
- Small code, easy to modify, few external dependancies
- ~Javascript version running Linux and Windows 2000.~ (Not supported in JavaScript)

## Changes from Fabrice Bellard's 2019-12-21 release:
- ToDo
