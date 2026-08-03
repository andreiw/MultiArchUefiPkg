/** @file

    Copyright (c) 2017, Linaro, Ltd. <ard.biesheuvel@linaro.org>
    Copyright (c) 2022-2023, Intel Corporation. All rights reserved.<BR>
    Copyright (c) 2026, MCST, All rights reserved.<BR>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.
**/

#include "Emulator.h"

#define E2K_SC_CR0HI_INDEX  5
#define E2K_EXC_ILLEGAL_OPCODE_NUM   0
#define E2K_EXC_INSTR_PAGE_MISS_NUM  20
#define E2K_EXC_INSTR_PAGE_PROT_NUM  21

//
// The saved PC (%cr0.hi) slot inside the trap context. Two flavors of
// EFI_SYSTEM_CONTEXT_E2K exist and both are ABI mirrors of uefi-e2k
// internals (we read them, we do not edit that tree):
//   native (PnsLoader)   - a 7-entry UINT64 control-register array,
//                          PC at index 5.
//   hosted (LinuxLoader) - { INT32 Signal; ucontext_t *Ucontext;
//                          siginfo_t *Siginfo; }; PC lives at
//                          Ucontext->uc_mcontext.cr0_hi, byte offset 48
//                          on E8C2 glibc (measured). Build with
//                          -DMAU_E2K_HOSTED.
//
#ifdef MAU_E2K_HOSTED
typedef struct
{
  INT32    Signal;
  VOID     *Ucontext;
  VOID     *Siginfo;
} E2K_HOSTED_CONTEXT;
#define E2K_HOSTED_UC_CR0HI_OFFSET  48
#endif

STATIC
UINT64 *
Cr0HiSlot (
  IN  EFI_SYSTEM_CONTEXT  SystemContext
  )
{
#ifdef MAU_E2K_HOSTED
  E2K_HOSTED_CONTEXT  *Ctx = (E2K_HOSTED_CONTEXT *)SystemContext.SystemContextE2K;

  return (UINT64 *)((UINT8 *)Ctx->Ucontext + E2K_HOSTED_UC_CR0HI_OFFSET);
#else
  return &((UINT64 *)SystemContext.SystemContextE2K)[E2K_SC_CR0HI_INDEX];
#endif
}

UINT64
EmulatorVmEntry (
  IN  UINT64       ProgramCounter,
  IN  UINT64       *Args,
  IN  ImageRecord  *Record,
  IN  UINT64       Lr
  );

STATIC UINT64  mRedirectProgramCounter;

STATIC
UINT64
EmulatorThunk (
  IN  UINT64  Arg0,
  IN  UINT64  Arg1,
  IN  UINT64  Arg2,
  IN  UINT64  Arg3,
  IN  UINT64  Arg4,
  IN  UINT64  Arg5,
  IN  UINT64  Arg6,
  IN  UINT64  Arg7,
  IN  UINT64  Arg8,
  IN  UINT64  Arg9,
  IN  UINT64  Arg10,
  IN  UINT64  Arg11,
  IN  UINT64  Arg12,
  IN  UINT64  Arg13,
  IN  UINT64  Arg14,
  IN  UINT64  Arg15
  )
{
  UINT64       ProgramCounter = mRedirectProgramCounter;
  ImageRecord  *Record        = ImageFindByAddress (ProgramCounter);

  UINT64  Args[MAX_ARGS] = 
  {
    Arg0, Arg1, Arg2,  Arg3,  Arg4,  Arg5,  Arg6,  Arg7,
    Arg8, Arg9, Arg10, Arg11, Arg12, Arg13, Arg14, Arg15
  };

  ASSERT (Record != NULL);
  return EmulatorVmEntry (ProgramCounter, Args, Record, 0);
}

/**
  Recover the low bits of an emulated entry point lost by the trap.

  An e2k instruction address is a bundle address: %cr0.hi has no room for
  the low 3 bits, and neither does the %ctpr the call was prepared through
  (both were measured - see ~/e2k-abi-exp/exp7_ctpr.c). So when native code
  calls an emulated function whose entry is not 8-byte aligned - x64 code
  has no alignment requirement, and e.g. efifs puts entry points on odd
  addresses - the trap reports the target rounded down to 8 bytes, which
  lands in the middle of an instruction.

  The true target is therefore somewhere in [Pc, Pc + 7]. Every x64 binary
  built with CET support - which is every modern toolchain default - starts
  each indirectly callable function with endbr64, so look for that. If no
  candidate is found, keep the address as reported: it is either already
  correct (aligned entry) or nothing better is knowable.

  @param  Pc      Program counter as reported by the trap, bundle aligned.
  @param  Record  Emulated image containing Pc.

  @return The recovered entry point.
**/
STATIC
UINT64
RecoverEntryPoint (
  IN  UINT64       Pc,
  IN  ImageRecord  *Record
  )
{
  STATIC CONST UINT8  Endbr64[] = { 0xF3, 0x0F, 0x1E, 0xFA };
  UINT64              Limit;
  UINT64              Candidate;

  if (Record->Cpu->EmuMachineType != EFI_IMAGE_MACHINE_X64) {
    return Pc;
  }

  Limit = Record->ImageBase + Record->ImageSize;

  for (Candidate = Pc; Candidate < (Pc + NATIVE_INSN_ALIGNMENT); Candidate++) {
    if ((Candidate + sizeof (Endbr64)) > Limit) {
      break;
    }

    if (CompareMem ((VOID *)Candidate, Endbr64, sizeof (Endbr64)) == 0) {
      if (Candidate != Pc) {
        DEBUG ((
          DEBUG_VERBOSE,
          "Unaligned %a entry 0x%lx recovered from 0x%lx\n",
          Record->Cpu->Name,
          Candidate,
          Pc
          ));
      }

      return Candidate;
    }
  }

  return Pc;
}

STATIC
inline
BOOLEAN
SupportedException (
  IN     EFI_EXCEPTION_TYPE  ExceptionType
  )
{
  switch (ExceptionType) {
    case E2K_EXC_ILLEGAL_OPCODE_NUM:
    case E2K_EXC_INSTR_PAGE_MISS_NUM:
    case E2K_EXC_INSTR_PAGE_PROT_NUM:
      return TRUE;
    default:
      return FALSE;
  }
}

VOID
EFIAPI
EmulatorSyncExceptionCallback (
  IN     EFI_EXCEPTION_TYPE  ExceptionType,
  IN OUT EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  UINT64       *PcSlot;
  ImageRecord  *Record;
  UINT64       ProgramCounter;

  PcSlot         = Cr0HiSlot (SystemContext);
  ProgramCounter = *PcSlot;
  Record         = ImageFindByAddress (ProgramCounter);

  if (SupportedException (ExceptionType) && (Record != NULL))
  {
    mRedirectProgramCounter = RecoverEntryPoint (ProgramCounter, Record);
    *PcSlot                 = (UINT64)EmulatorThunk;
    return;
  }

  /*
   * We can't handle this exception. Try to produce some meaningful
   * diagnostics regarding the emulated code this maps onto.
   */
  DEBUG ((
    DEBUG_ERROR,
    "Unhandled e2k exception %ld, PC 0x%lx\n",
    (INT64)ExceptionType,
    ProgramCounter
    ));

  if (Record != NULL) 
  {
    /*
     * An exception on the emulated image that we can't thunk: most
     * likely the x64 code bytes happened to decode as a *valid* e2k
     * instruction and executed before faulting differently.
     */
    DEBUG ((
      DEBUG_ERROR,
      "Exception occured due to executing %a code as e2k code.\n",
      Record->Cpu->Name
      ));
  }
  else if (CpuAddrIsCodeGen (ProgramCounter)) 
  {
    /*
     * It looks like we crashed in the JITed code.
     *
     * TBD: can we lookup/decode the TB info?
     */
    DEBUG ((DEBUG_ERROR, "Exception occurred in TBs\n"));
  }
  else if (IsDriverImagePointer ((VOID *)ProgramCounter)) 
  {
    DEBUG ((
      DEBUG_ERROR,
      "Exception occured at driver PC +0x%lx\n",
      ProgramCounter - (UINT64)gDriverImage->ImageBase
      ));
  }

  EmulatorDump ();
  CpuDeadLoop ();
}

STATIC UINTN  mExceptions[] = 
{
  E2K_EXC_ILLEGAL_OPCODE_NUM,
  E2K_EXC_INSTR_PAGE_MISS_NUM,
  E2K_EXC_INSTR_PAGE_PROT_NUM,
};

EFI_STATUS
ArchInit (
  VOID
  )
{
  INTN        Index;
  EFI_STATUS  Status;
  EFI_STATUS  Status2;

  Status = EFI_SUCCESS;
  for (Index = 0; Index < ARRAY_SIZE (mExceptions); Index++)
  {
    Status = gCpu->RegisterInterruptHandler (gCpu, mExceptions[Index], &EmulatorSyncExceptionCallback);
    if (EFI_ERROR (Status)) 
    {
      DEBUG ((DEBUG_ERROR, "RegisterInterruptHandler 0x%x: %r\n", mExceptions[Index], Status));
      break;
    }
  }

  if (EFI_ERROR (Status)) {
    while (--Index >= 0) {
      Status2 = gCpu->RegisterInterruptHandler (gCpu, mExceptions[Index], NULL);
      ASSERT (!EFI_ERROR (Status2));
    }
  }

  return Status;
}

VOID
ArchCleanup (
  VOID
  )
{
  INTN        Index;
  EFI_STATUS  Status;

  for (Index = 0; Index < ARRAY_SIZE (mExceptions); Index++) {
    Status = gCpu->RegisterInterruptHandler (gCpu, mExceptions[Index], NULL);
    ASSERT (!EFI_ERROR (Status));
  }
}
