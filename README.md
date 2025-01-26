# MultiArchUefiPkg - Multi-Architecture UEFI Environment Driver

This repo implements a UEFI driver that allows non-native UEFI boot
service drivers and applications to be executed in 64-bit UEFI environments.

> [!NOTE]
> Relation to https://github.com/intel/MultiArchUefiPkg - this is a direct continuation
> of the work I've done at Intel before I left. Most likely, the Intel repo is dead,
> as I was the owner and the only remaining contributor to the project.

The primary motivation is bringing together the IHV device and
standardized AARCH64 and RISCV64 ecosystems. This driver allows plugging
off the shelf storage, network and video cards into non-x86 machines
and using them with UEFI for OS boot.

Today, AArch64 and RISC-V are supported as host environments.
In AArch64 environments, EmulatorDxe supports X64 (aka x86_64, AMD64)
UEFI binaries. On RISCV64, EmulatorDxe can be built with X64 and AARCH64
UEFI binary support.

It's derived from [X86EmulatorPkg](https://github.com/ardbiesheuvel/X86EmulatorPkg),
yet is otherwise a reimplementation using a [UEFI-enabled version](https://github.com/andreiw/unicorn-for-efi)
of the [Unicorn Engine](https://www.unicorn-engine.org/) - a flexible
CPU emulator framework.

MultiArchUefiPkg has competitive performance, portability and size
(2/3rds the binary size on AArch64, even smaller for RISCV64!),
support for multiple emulated ISAs, a regression test suite, and
improved (but not perfect) correctness in modeling the emulated
UEFI Boot Service environment.

[See a demo](https://youtu.be/ntZ177E3lRY).

## Updates

| When | What |
| :-: | ------------ |
| May 2024 | Deal with reported issue for a Micron NVMe OpRom, which uses `rdrand` instruction and scribbles to low RAM on success. |
| December 2023 | More docs, conversion to UEFI DM, MAU_TRY_WITHOUT_MMU as an explicit build option (still enabled by default for RISC-V). LoadOpRom.efi tool. MAU_STANDALONE_LOGGING choices (and EFI_SERIAL_IO_PROTOCOL-backed implementation). SetCon tool. |
| November 2023 | Doc and build system updates. |
| October 2023 | Presented at the UEFI Fall 2023 Developers Conference and Plugfest. See the [presentation slides](Docs/Uefi2023/multi_isa_fw_compat.pdf). |
| September 2023 | Move away from TPL manipulation and reduce RISC-V binary size by half. |
| July 2023 | Support for building with MAU_EMU_RAZ_WI_PIO. |
| June 2023 | Presented at the RISC-V Summit Europe, Barcelona. See the [extended abstract](Docs/RviSummitMarch2023/2023-06-08-Andrei-WARKENTIN-abstract.pdf), [poster](Docs/RviSummitMarch2023/multi_isa_uefi_compat_poster.pdf) and [presentation slides](Docs/RviSummitMarch2023/multi_isa_uefi_compat.pdf). |
| May 2023 | Project reported to RISE Board as active priority. |
| March 2023 | Released to GitHub |

## Theory Of Operation

UEFI code uses a pretty narrowly-defined ABI, which makes it
easy to thunk x64/AArch64 client code making EFIAPI calls to
native code: no FP/SIMD, no returning large values, etc. E.g. calls like:

        UINT64
        EFIAPI
        Fn(UINT64, UINT64, UINT64, UINT64,
           UINT64, UINT64, UINT64, UINT64,
           UINT64, UINT64, UINT64, UINT64,
           UINT64, UINT64, UINT64, UINT64);

...with up to 16 arguments are supported both client -> native
and native -> client, which covers all UEFI needs.

Seamless thunking from native code to emulated code relies on MMU
support in the UEFI firmware, specifically the ability to mark a
range non-executable.

MultiArchUefiPkg won't work with just any UEFI implementations, but only
with implementations that provide the EDKII_PECOFF_IMAGE_EMULATOR_PROTOCOL
interface, which is the magic that enables loading foreign ISA binaries.
Today this means you must use TianoCore EDK2 or a derived implementation.

The emulator presents an x64 and/or AArch64 UEFI Boot Services
environment appropriate for running Boot Service drivers (e.g. OpRom
drivers for video cards, NICs) and UEFI applications that aren't OS
loaders. Certain Boot, Runtime and DXE services are filtered or disabled.

## Quick Start

To quickly build a RISC-V version:

        $ git clone https://github.com/tianocore/edk2.git
        $ cd edk2
        $ git submodule add https://github.com/andreiw/unicorn-for-efi.git unicorn
        $ git submodule add https://github.com/andreiw/MultiArchUefiPkg.git
        $ git submodule update --init
        $ export GCC_RISCV64_PREFIX=... (if you are on a non-RISCV64 system)
        $ build -a RISCV64 -t GCC -p MultiArchUefiPkg/Emulator.dsc -b RELEASE

This will produce `Build/MultiArchUefiPkg/RELEASE_GCC/RISCV64/EmulatorDxe.efi`.

Also see the documents on [building](Docs/Building.md) and [running](Docs/Running.md).

## Modeled Environment

The emulator presents UEFI Boot Services environment appropriate
for running Boot Service drivers (e.g. OpRom drivers for video cards, NICs)
and UEFI applications that aren't OS loaders.

The details are described by [a separate document](Docs/EmulatedEnvironment.md).

## Tested Platforms

The emulator is known to work on Qemu `-M virt` for both AArch64 and RISCV64, Ampere Altra, Raspberry Pi 4 and StarFive VisionFive 2 systems.

## License

MultiArchUefiPkg is approved for release under GPLv2 and LGPLv2.1+ for the respective components under those licenses. EmulatorDxe itself is covered by the [LGPL v2.1+](LICENSE), but it is statically linked to the [Unicorn Engine](https://www.unicorn-engine.org/) library, which has a mix of LGPL and GPLv2 code.

## Security Policy

I am committed to addressing security vulnerabilities with a reasonable response time for an open-source/hobby project and providing clear guidance on the solution, impact, severity and mitigation.

## Reporting A Vulnerability
If you have discovered potential security vulnerability, please validate with the latest commit in the repo. If the issue reproduces - raise an issue. It is important to include the following details:
- Detailed description of the vulnerability, with a repro if possible.
- Information on known exploits.

## Contribute

Contributions are welcome. Please raise issues and pull requests.

Please see the [policy on contributions](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md).
