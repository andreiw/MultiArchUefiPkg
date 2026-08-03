/** @file

    Copyright (c) 2017, Linaro, Ltd. <ard.biesheuvel@linaro.org>
    Copyright (c) 2022-2023, Intel Corporation. All rights reserved.<BR>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

**/

#include <unicorn.h>
#include "Emulator.h"

typedef union {
  UINT64 (*NativeFn)(
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64,
    UINT64
    );
  UINT64 (*NativeFnVa)(
    UINT64,
    ...
    );
  UINT64 (*WrapperFn)(
    UINT64  OriginalProgramCounter,
    UINT64  ReturnAddress,
    UINT64  *Args
    );
  UINT64    ProgramCounter;
} Fn;

/*
 * Which flavor of the above is used to perform the emulated -> native call.
 *
 * On e2k the variadic calling convention is a strict superset of the fixed
 * one: arguments 0-7 are passed in the register window *and* every argument
 * is mirrored into the memory parameter area at [callee-SP + 0x00 ...],
 * which is exactly where a callee's va_start()/va_arg() picks them up from
 * (arguments 8+ live at [callee-SP + 0x40 ...] under both conventions).
 *
 * A variadic callee - e.g. EDK2's gBS->InstallMultipleProtocolInterfaces -
 * therefore reads pure garbage when it is invoked through a fixed-argument
 * prototype, because the mirror area is never written by the caller. Since
 * we cannot know whether the native target is variadic, always call through
 * a variadic prototype on e2k: that satisfies both kinds of callee.
 *
 * Other hosts pass variadic and fixed arguments identically, so they keep
 * using the plain prototype.
 */
#ifdef MDE_CPU_E2K
  #define NATIVE_CALL_FN  NativeFnVa
#else
  #define NATIVE_CALL_FN  NativeFn
#endif

EFI_STATUS
EFIAPI
NativeUnsupported (
  IN  UINT64  OriginalProgramCounter,
  IN  UINT64  ReturnAddress,
  IN  UINT64  *Args
  )
{
  UNUSED CpuContext  *Cpu = CpuGetTopContext ()->Cpu;

  DEBUG ((
    DEBUG_ERROR,
    "Unsupported native call 0x%lx from %a PC 0x%lx\n",
    OriginalProgramCounter,
    Cpu->Name,
    ReturnAddress
    ));
  EmulatorDump ();
  return EFI_UNSUPPORTED;
}

STATIC
UINT64
NativeValidateSupportedCall (
  IN  UINT64  ProgramCounter
  )
{
  /*
   * Prevent things that won't work in principle or that
   * could kill the emulator.
   */
  if (ProgramCounter < EFI_PAGE_SIZE) {
    DEBUG ((DEBUG_ERROR, "NULL-pointer native call to 0x%lx\n", ProgramCounter));
    return (UINT64)&NativeUnsupported;
  }

  return EfiWrappersOverride (ProgramCounter);
}

STATIC
VOID
NativeThunkCheckLeakedContexts (
  IN  CpuRunContext  *Context
  )
{
  if (CpuGetTopContext () != Context) {
    /*
     * Consider the following sequence:
     * - emulated->native call
     * - native does SetJump
     * - native->emulated call
     * - emulated->native call
     * - native does LongJump
     *
     * This isn't that crazy - e.g. an emulated binary starting another
     * emu binary, which calls gBS->Exit. While we can handle gBS->Exit
     * cleanly ourselves, let's detect code that does something similar,
     * which will result in UC engine state being out of sync with the
     * expected context state.
     */
    CpuCompressLeakedContexts (Context, FALSE);
  }
}

#ifdef MAU_SUPPORTS_AARCH64_BINS
UINT64
NativeThunkAArch64 (
  IN  CpuRunContext  *Context,
  IN  UINT64         ProgramCounter
  )
{
  UINT64      *StackArgs;
  BOOLEAN     WrapperCall;
  UINT64      Lr, Sp, X0, X1, X2, X3, X4, X5, X6, X7;
  Fn          Func;
  CpuContext  *Cpu;

  Cpu                 = Context->Cpu;
  Func.ProgramCounter = NativeValidateSupportedCall (ProgramCounter);
  WrapperCall         = Func.ProgramCounter != ProgramCounter;

  Lr = REG_READ (Cpu, UC_ARM64_REG_LR);
  Sp = REG_READ (Cpu, UC_ARM64_REG_SP);
  X0 = REG_READ (Cpu, UC_ARM64_REG_X0);
  X1 = REG_READ (Cpu, UC_ARM64_REG_X1);
  X2 = REG_READ (Cpu, UC_ARM64_REG_X2);
  X3 = REG_READ (Cpu, UC_ARM64_REG_X3);
  X4 = REG_READ (Cpu, UC_ARM64_REG_X4);
  X5 = REG_READ (Cpu, UC_ARM64_REG_X5);
  X6 = REG_READ (Cpu, UC_ARM64_REG_X6);
  X7 = REG_READ (Cpu, UC_ARM64_REG_X7);

  StackArgs = (UINT64 *)Sp;

  /*
   * EFIAPI (MS) AArch64 Stack Layout (in UINT64's):
   *
   * ----------------
   *   ...
   *   arg9
   *   arg8
   * ----------------
   */

  DEBUG ((
    DEBUG_VERBOSE,
    "%a 0x%lx -> %a 0x%lx(%lx, %lx, %lx, %lx, %lx, %lx, %lx, %lx, %lx)\n",
    Cpu->Name,
    Lr,
    WrapperCall ? "wrapper" : "native",
    Func.ProgramCounter,
    X0,
    X1,
    X2,
    X3,
    X4,
    X5,
    X6,
    X7,
    StackArgs[0]
    ));

  if (WrapperCall) {
    UINT64  WrapperArgs[MAX_ARGS] = {
      X0,           X1,           X2, X3, X4, X5, X6, X7,
      StackArgs[0], StackArgs[1],
      StackArgs[2], StackArgs[3],
      StackArgs[4], StackArgs[5],
      StackArgs[6], StackArgs[7]
    };
    X0 = Func.WrapperFn (ProgramCounter, Lr, WrapperArgs);
  } else {
    X0 = Func.NATIVE_CALL_FN (
                X0,
                X1,
                X2,
                X3,
                X4,
                X5,
                X6,
                X7,
                StackArgs[0],
                StackArgs[1],
                StackArgs[2],
                StackArgs[3],
                StackArgs[4],
                StackArgs[5],
                StackArgs[6],
                StackArgs[7]
                );
  }

  NativeThunkCheckLeakedContexts (Context);

  REG_WRITE (Cpu, UC_ARM64_REG_X0, X0);

  return REG_READ (Cpu, UC_ARM64_REG_LR);
}

#endif /* MAU_SUPPORTS_AARCH64_BINS */

UINT64
NativeThunkX64 (
  IN  CpuRunContext  *Context,
  IN  UINT64         ProgramCounter
  )
{
  UINT64      *StackArgs;
  BOOLEAN     WrapperCall;
  UINT64      Rax, Rsp, Rcx, Rdx, R8, R9;
  Fn          Func;
  CpuContext  *Cpu;

  Cpu                 = Context->Cpu;
  Func.ProgramCounter = NativeValidateSupportedCall (ProgramCounter);
  WrapperCall         = Func.ProgramCounter != ProgramCounter;

  Rsp = REG_READ (Cpu, UC_X86_REG_RSP);
  Rcx = REG_READ (Cpu, UC_X86_REG_RCX);
  Rdx = REG_READ (Cpu, UC_X86_REG_RDX);
  R8  = REG_READ (Cpu, UC_X86_REG_R8);
  R9  = REG_READ (Cpu, UC_X86_REG_R9);

  StackArgs = (UINT64 *)Rsp;

  /*
   * EFIAPI (MS) x86_64 Stack Layout (in UINT64's):
   *
   * ----------------
   *   ...
   *   arg9
   *   arg8
   *   arg7
   *   arg6
   *   arg5
   *   arg4
   *   home zone (reserved for called function)
   *   home zone (reserved for called function)
   *   home zone (reserved for called function)
   *   home zone (reserved for called function)
   *   return pointer
   * ----------------
   */

  DEBUG ((
    DEBUG_VERBOSE,
    "%a 0x%lx -> %a 0x%lx(%lx, %lx, %lx, %lx, %lx, %lx, %lx, %lx, %lx)\n",
    Cpu->Name,
    StackArgs[0],
    WrapperCall ? "wrapper" : "native",
    Func.ProgramCounter,
    Rcx,
    Rdx,
    R8,
    R9,
    StackArgs[5],
    StackArgs[6],
    StackArgs[7],
    StackArgs[8],
    StackArgs[9]
    ));

  if (WrapperCall) {
    StackArgs[1] = Rcx;
    StackArgs[2] = Rdx;
    StackArgs[3] = R8;
    StackArgs[4] = R9;
    Rax          = Func.WrapperFn (ProgramCounter, StackArgs[0], StackArgs + 1);
  } else {
    Rax = Func.NATIVE_CALL_FN (
                 Rcx,
                 Rdx,
                 R8,
                 R9,
                 StackArgs[5],
                 StackArgs[6],
                 StackArgs[7],
                 StackArgs[8],
                 StackArgs[9],
                 StackArgs[10],
                 StackArgs[11],
                 StackArgs[12],
                 StackArgs[13],
                 StackArgs[14],
                 StackArgs[15],
                 StackArgs[16]
                 );
  }

  NativeThunkCheckLeakedContexts (Context);

  REG_WRITE (Cpu, UC_X86_REG_RAX, Rax);

  return CpuStackPop64 (Cpu);
}
