/* $Id$ */
/** @file
 * DBGPlugInWindows - Debugger and Guest OS Digger Plugin For Windows NT.
 */

/*
 * Copyright (C) 2009-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DBGF /// @todo add new log group.
#include "DBGPlugIns.h"
#include <VBox/vmm/dbgf.h>
#include <VBox/vmm/cpumctx.h>
#include <VBox/vmm/mm.h>
#include <VBox/err.h>
#include <VBox/param.h>
#include <iprt/ctype.h>
#include <iprt/ldr.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/utf16.h>
#include <iprt/formats/pecoff.h>
#include <iprt/formats/mz.h>
#include <iprt/nt/nt-structures.h>


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** @name Internal WinNT structures
 * @{ */
/**
 * PsLoadedModuleList entry for 32-bit NT aka LDR_DATA_TABLE_ENTRY.
 * Tested with XP.
 */
typedef struct NTMTE32
{
    struct
    {
        uint32_t    Flink;
        uint32_t    Blink;
    }               InLoadOrderLinks,
                    InMemoryOrderModuleList,
                    InInitializationOrderModuleList;
    uint32_t        DllBase;
    uint32_t        EntryPoint;
    /** @note This field is not a size in NT 3.1. It's NULL for images loaded by the
     *        boot loader, for other images it looks like some kind of pointer.  */
    uint32_t        SizeOfImage;
    struct
    {
        uint16_t    Length;
        uint16_t    MaximumLength;
        uint32_t    Buffer;
    }               FullDllName,
                    BaseDllName;
    uint32_t        Flags;
    uint16_t        LoadCount;
    uint16_t        TlsIndex;
    /* ... there is more ... */
} NTMTE32;
typedef NTMTE32 *PNTMTE32;

/**
 * PsLoadedModuleList entry for 64-bit NT aka LDR_DATA_TABLE_ENTRY.
 */
typedef struct NTMTE64
{
    struct
    {
        uint64_t    Flink;
        uint64_t    Blink;
    }               InLoadOrderLinks,                  /**< 0x00 */
                    InMemoryOrderModuleList,           /**< 0x10 */
                    InInitializationOrderModuleList;   /**< 0x20 */
    uint64_t        DllBase;                           /**< 0x30 */
    uint64_t        EntryPoint;                        /**< 0x38 */
    uint32_t        SizeOfImage;                       /**< 0x40 */
    uint32_t        Alignment;                         /**< 0x44 */
    struct
    {
        uint16_t    Length;                            /**< 0x48,0x58 */
        uint16_t    MaximumLength;                     /**< 0x4a,0x5a */
        uint32_t    Alignment;                         /**< 0x4c,0x5c */
        uint64_t    Buffer;                            /**< 0x50,0x60 */
    }               FullDllName,                       /**< 0x48 */
                    BaseDllName;                       /**< 0x58 */
    uint32_t        Flags;                             /**< 0x68 */
    uint16_t        LoadCount;                         /**< 0x6c */
    uint16_t        TlsIndex;                          /**< 0x6e */
    /* ... there is more ... */
} NTMTE64;
typedef NTMTE64 *PNTMTE64;

/** MTE union. */
typedef union NTMTE
{
    NTMTE32         vX_32;
    NTMTE64         vX_64;
} NTMTE;
typedef NTMTE *PNTMTE;


/**
 * The essential bits of the KUSER_SHARED_DATA structure.
 */
typedef struct NTKUSERSHAREDDATA
{
    uint32_t        TickCountLowDeprecated;
    uint32_t        TickCountMultiplier;
    struct
    {
        uint32_t    LowPart;
        int32_t     High1Time;
        int32_t     High2Time;

    }               InterruptTime,
                    SystemTime,
                    TimeZoneBias;
    uint16_t        ImageNumberLow;
    uint16_t        ImageNumberHigh;
    RTUTF16         NtSystemRoot[260];
    uint32_t        MaxStackTraceDepth;
    uint32_t        CryptoExponent;
    uint32_t        TimeZoneId;
    uint32_t        LargePageMinimum;
    uint32_t        Reserved2[6];
    uint32_t        NtBuildNumber;
    uint32_t        NtProductType;
    uint8_t         ProductTypeIsValid;
    uint8_t         abPadding[3];
    uint32_t        NtMajorVersion;
    uint32_t        NtMinorVersion;
    /* uint8_t         ProcessorFeatures[64];
    ...
    */
} NTKUSERSHAREDDATA;
typedef NTKUSERSHAREDDATA *PNTKUSERSHAREDDATA;

/** KI_USER_SHARED_DATA for i386 */
#define NTKUSERSHAREDDATA_WINNT32   UINT32_C(0xffdf0000)
/** KI_USER_SHARED_DATA for AMD64 */
#define NTKUSERSHAREDDATA_WINNT64   UINT64_C(0xfffff78000000000)

/** NTKUSERSHAREDDATA::NtProductType */
typedef enum NTPRODUCTTYPE
{
    kNtProductType_Invalid = 0,
    kNtProductType_WinNt = 1,
    kNtProductType_LanManNt,
    kNtProductType_Server
} NTPRODUCTTYPE;


/** NT image header union. */
typedef union NTHDRSU
{
    IMAGE_NT_HEADERS32  vX_32;
    IMAGE_NT_HEADERS64  vX_64;
} NTHDRS;
/** Pointer to NT image header union. */
typedef NTHDRS *PNTHDRS;
/** Pointer to const NT image header union. */
typedef NTHDRS const *PCNTHDRS;

/** @} */



typedef enum DBGDIGGERWINNTVER
{
    DBGDIGGERWINNTVER_UNKNOWN,
    DBGDIGGERWINNTVER_3_1,
    DBGDIGGERWINNTVER_3_5,
    DBGDIGGERWINNTVER_4_0,
    DBGDIGGERWINNTVER_5_0,
    DBGDIGGERWINNTVER_5_1,
    DBGDIGGERWINNTVER_6_0
} DBGDIGGERWINNTVER;

/**
 * WinNT guest OS digger instance data.
 */
typedef struct DBGDIGGERWINNT
{
    /** Whether the information is valid or not.
     * (For fending off illegal interface method calls.) */
    bool                fValid;
    /** 32-bit (true) or 64-bit (false) */
    bool                f32Bit;
    /** Set if NT 3.1 was detected.
     * This implies both Misc.VirtualSize and NTMTE32::SizeOfImage are zero. */
    bool                fNt31;

    /** The NT version. */
    DBGDIGGERWINNTVER   enmVer;
    /** NTKUSERSHAREDDATA::NtProductType */
    NTPRODUCTTYPE       NtProductType;
    /** NTKUSERSHAREDDATA::NtMajorVersion */
    uint32_t            NtMajorVersion;
    /** NTKUSERSHAREDDATA::NtMinorVersion */
    uint32_t            NtMinorVersion;
    /** NTKUSERSHAREDDATA::NtBuildNumber */
    uint32_t            NtBuildNumber;

    /** The address of the ntoskrnl.exe image. */
    DBGFADDRESS         KernelAddr;
    /** The address of the ntoskrnl.exe module table entry. */
    DBGFADDRESS         KernelMteAddr;
    /** The address of PsLoadedModuleList. */
    DBGFADDRESS         PsLoadedModuleListAddr;

    /** Array of detected KPCR addresses for each vCPU. */
    PDBGFADDRESS        paKpcrAddr;
    /** Array of detected KPCRB addresses for each vCPU. */
    PDBGFADDRESS        paKpcrbAddr;

    /** The Windows NT specifics interface. */
    DBGFOSIWINNT        IWinNt;
} DBGDIGGERWINNT;
/** Pointer to the linux guest OS digger instance data. */
typedef DBGDIGGERWINNT *PDBGDIGGERWINNT;


/**
 * The WinNT digger's loader reader instance data.
 */
typedef struct DBGDIGGERWINNTRDR
{
    /** The VM handle (referenced). */
    PUVM                pUVM;
    /** The image base. */
    DBGFADDRESS         ImageAddr;
    /** The image size. */
    uint32_t            cbImage;
    /** The file offset of the SizeOfImage field in the optional header if it
     *  needs patching, otherwise set to UINT32_MAX. */
    uint32_t            offSizeOfImage;
    /** The correct image size. */
    uint32_t            cbCorrectImageSize;
    /** Number of entries in the aMappings table. */
    uint32_t            cMappings;
    /** Mapping hint. */
    uint32_t            iHint;
    /** Mapping file offset to memory offsets, ordered by file offset. */
    struct
    {
        /** The file offset. */
        uint32_t        offFile;
        /** The size of this mapping. */
        uint32_t        cbMem;
        /** The offset to the memory from the start of the image.  */
        uint32_t        offMem;
    } aMappings[1];
} DBGDIGGERWINNTRDR;
/** Pointer a WinNT loader reader instance data. */
typedef DBGDIGGERWINNTRDR *PDBGDIGGERWINNTRDR;


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** Validates a 32-bit Windows NT kernel address */
#define WINNT32_VALID_ADDRESS(Addr)         ((Addr) >         UINT32_C(0x80000000) && (Addr) <         UINT32_C(0xfffff000))
/** Validates a 64-bit Windows NT kernel address */
#define WINNT64_VALID_ADDRESS(Addr)         ((Addr) > UINT64_C(0xffff800000000000) && (Addr) < UINT64_C(0xfffffffffffff000))
/** Validates a kernel address. */
#define WINNT_VALID_ADDRESS(pThis, Addr)    ((pThis)->f32Bit ? WINNT32_VALID_ADDRESS(Addr) : WINNT64_VALID_ADDRESS(Addr))
/** Versioned and bitness wrapper. */
#define WINNT_UNION(pThis, pUnion, Member)  ((pThis)->f32Bit ? (pUnion)->vX_32. Member : (pUnion)->vX_64. Member )

/** The length (in chars) of the kernel file name (no path). */
#define WINNT_KERNEL_BASE_NAME_LEN          12

/** WindowsNT on little endian ASCII systems. */
#define DIG_WINNT_MOD_TAG                   UINT64_C(0x54696e646f774e54)


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static DECLCALLBACK(int)  dbgDiggerWinNtInit(PUVM pUVM, void *pvData);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** Kernel names. */
static const RTUTF16 g_wszKernelNames[][WINNT_KERNEL_BASE_NAME_LEN + 1] =
{
    { 'n', 't', 'o', 's', 'k', 'r', 'n', 'l', '.', 'e', 'x', 'e' }
};



/**
 * Tries to resolve the KPCR and KPCRB addresses for each vCPU.
 *
 * @returns nothing.
 * @param   pThis           The instance data.
 * @param   pUVM            The user mode VM handle.
 */
static void dbgDiggerWinNtResolveKpcr(PDBGDIGGERWINNT pThis, PUVM pUVM)
{
    if (pThis->f32Bit)
    { /** @todo */ }
    else
    {
        /*
         * Getting at the KPCR and KPCRB is explained here:
         *     https://www.geoffchappell.com/studies/windows/km/ntoskrnl/structs/kprcb/amd64.htm
         * Together with the available offsets from:
         *     https://github.com/tpn/winsdk-10/blob/master/Include/10.0.16299.0/shared/ksamd64.inc#L883
         * we can verify that the found addresses are valid by cross checking that the GDTR and self reference
         * match what we expect.
         */
        VMCPUID cCpus = DBGFR3CpuGetCount(pUVM);
        pThis->paKpcrAddr = (PDBGFADDRESS)RTMemAllocZ(cCpus * 2 * sizeof(DBGFADDRESS));
        if (RT_LIKELY(pThis->paKpcrAddr))
        {
            pThis->paKpcrbAddr = &pThis->paKpcrAddr[cCpus];

            /* Work each CPU, unexpected values in each CPU make the whole thing fail to play safe. */
            int rc = VINF_SUCCESS;
            for (VMCPUID idCpu = 0; (idCpu < cCpus) && RT_SUCCESS(rc); idCpu++)
            {
                PDBGFADDRESS pKpcrAddr = &pThis->paKpcrAddr[idCpu];
                PDBGFADDRESS pKpcrbAddr = &pThis->paKpcrbAddr[idCpu];

                /* Read GS base which points to the base of the KPCR for each CPU. */
                RTGCUINTPTR GCPtrTmp = 0;
                rc = DBGFR3RegCpuQueryU64(pUVM, idCpu, DBGFREG_GS_BASE, &GCPtrTmp);
                if (RT_SUCCESS(rc))
                {
                    LogFlow(("DigWinNt/KPCR[%u]: GS Base %RGv\n", idCpu, GCPtrTmp));
                    DBGFR3AddrFromFlat(pUVM, pKpcrAddr, GCPtrTmp);

                    rc = DBGFR3RegCpuQueryU64(pUVM, idCpu, DBGFREG_GDTR_BASE, &GCPtrTmp);
                    if (RT_SUCCESS(rc))
                    {
                        /*
                         * Read the start of the KPCR (@todo Probably move this to a global header)
                         * and verify its content.
                         */
                        struct
                        {
                            RTGCUINTPTR GCPtrGdt;
                            RTGCUINTPTR GCPtrTss;
                            RTGCUINTPTR GCPtrUserRsp;
                            RTGCUINTPTR GCPtrSelf;
                            RTGCUINTPTR GCPtrCurrentPrcb;
                        } Kpcr;

                        rc = DBGFR3MemRead(pUVM, idCpu, pKpcrAddr, &Kpcr, sizeof(Kpcr));
                        if (RT_SUCCESS(rc))
                        {
                            if (   Kpcr.GCPtrGdt == GCPtrTmp
                                && Kpcr.GCPtrSelf == pKpcrAddr->FlatPtr
                                /** @todo && TSS */ )
                            {
                                DBGFR3AddrFromFlat(pUVM, pKpcrbAddr, Kpcr.GCPtrCurrentPrcb);
                                LogRel(("DigWinNt/KPCR[%u]: KPCR=%RGv KPCRB=%RGv\n", idCpu, pKpcrAddr->FlatPtr, pKpcrbAddr->FlatPtr));
                            }
                            else
                                LogRel(("DigWinNt/KPCR[%u]: KPCR validation error GDT=(%RGv vs %RGv) KPCRB=(%RGv vs %RGv)\n", idCpu,
                                        Kpcr.GCPtrGdt, GCPtrTmp, Kpcr.GCPtrSelf, pKpcrAddr->FlatPtr));
                        }
                        else
                            LogRel(("DigWinNt/KPCR[%u]: Reading KPCR start at %RGv failed with %Rrc\n", idCpu, pKpcrAddr->FlatPtr, rc));
                    }
                    else
                        LogRel(("DigWinNt/KPCR[%u]: Getting GDT base register failed with %Rrc\n", idCpu, rc));
                }
                else
                    LogRel(("DigWinNt/KPCR[%u]: Getting GS base register failed with %Rrc\n", idCpu, rc));
            }

            if (RT_FAILURE(rc))
            {
                LogRel(("DigWinNt/KPCR: Failed to detmine KPCR and KPCRB rc=%Rrc\n", rc));
                RTMemFree(pThis->paKpcrAddr);
                pThis->paKpcrAddr  = NULL;
                pThis->paKpcrbAddr = NULL;
            }
        }
        else
            LogRel(("DigWinNt/KPCR: Failed to allocate %u entries for the KPCR/KPCRB addresses\n", cCpus * 2));
    }
}


/**
 * Process a PE image found in guest memory.
 *
 * @param   pThis           The instance data.
 * @param   pUVM            The user mode VM handle.
 * @param   pszName         The module name.
 * @param   pszFilename     The image filename.
 * @param   pImageAddr      The image address.
 * @param   cbImage         The size of the image.
 */
static void dbgDiggerWinNtProcessImage(PDBGDIGGERWINNT pThis, PUVM pUVM, const char *pszName, const char *pszFilename,
                                       PCDBGFADDRESS pImageAddr, uint32_t cbImage)
{
    LogFlow(("DigWinNt: %RGp %#x %s\n", pImageAddr->FlatPtr, cbImage, pszName));

    /*
     * Do some basic validation first.
     */
    if (   (cbImage < sizeof(IMAGE_NT_HEADERS64) && !pThis->fNt31)
        || cbImage >= _1M * 256)
    {
        Log(("DigWinNt: %s: Bad image size: %#x\n", pszName, cbImage));
        return;
    }

    /*
     * Use the common in-memory module reader to create a debug module.
     */
    RTERRINFOSTATIC ErrInfo;
    RTDBGMOD        hDbgMod = NIL_RTDBGMOD;
    int rc = DBGFR3ModInMem(pUVM, pImageAddr, pThis->fNt31 ? DBGFMODINMEM_F_PE_NT31 : 0, pszName, pszFilename,
                            pThis->f32Bit ? RTLDRARCH_X86_32 : RTLDRARCH_AMD64, cbImage,
                            &hDbgMod, RTErrInfoInitStatic(&ErrInfo));
    if (RT_SUCCESS(rc))
    {
        /*
         * Tag the module.
         */
        rc = RTDbgModSetTag(hDbgMod, DIG_WINNT_MOD_TAG);
        AssertRC(rc);

        /*
         * Link the module.
         */
        RTDBGAS hAs = DBGFR3AsResolveAndRetain(pUVM, DBGF_AS_KERNEL);
        if (hAs != NIL_RTDBGAS)
            rc = RTDbgAsModuleLink(hAs, hDbgMod, pImageAddr->FlatPtr, RTDBGASLINK_FLAGS_REPLACE /*fFlags*/);
        else
            rc = VERR_INTERNAL_ERROR;
        RTDbgModRelease(hDbgMod);
        RTDbgAsRelease(hAs);
    }
    else if (RTErrInfoIsSet(&ErrInfo.Core))
        Log(("DigWinNt: %s: DBGFR3ModInMem failed: %Rrc - %s\n", pszName, rc, ErrInfo.Core.pszMsg));
    else
        Log(("DigWinNt: %s: DBGFR3ModInMem failed: %Rrc\n", pszName, rc));
}


/**
 * Generate a debugger compatible module name from a filename.
 *
 * @returns Pointer to module name (doesn't need to be pszName).
 * @param   pszFilename         The source filename.
 * @param   pszName             Buffer to put the module name in.
 * @param   cbName              Buffer size.
 */
static const char *dbgDiggerWintNtFilenameToModuleName(const char *pszFilename, char *pszName, size_t cbName)
{
    /* Skip to the filename part of the filename. :-) */
    pszFilename = RTPathFilenameEx(pszFilename, RTPATH_STR_F_STYLE_DOS);

    /* We try use 'nt' for the kernel. */
    if (   RTStrICmpAscii(pszFilename, "ntoskrnl.exe") == 0
        || RTStrICmpAscii(pszFilename, "ntkrnlmp.exe") == 0)
        return "nt";


    /* Drop the extension if .dll or .sys. */
    size_t cchFilename = strlen(pszFilename);
    if (   cchFilename > 4
        && pszFilename[cchFilename - 4] == '.')
    {
        if (   RTStrICmpAscii(&pszFilename[cchFilename - 4], ".sys") == 0
            || RTStrICmpAscii(&pszFilename[cchFilename - 4], ".dll") == 0)
            cchFilename -= 4;
    }

    /* Copy and do replacements. */
    if (cchFilename >= cbName)
        cchFilename = cbName - 1;
    size_t off;
    for (off = 0; off < cchFilename; off++)
    {
        char ch = pszFilename[off];
        if (!RT_C_IS_ALNUM(ch))
            ch = '_';
        pszName[off] = ch;
    }
    pszName[off] = '\0';
    return pszName;
}


/**
 * @interface_method_impl{DBGFOSIWINNT,pfnQueryVersion}
 */
static DECLCALLBACK(int) dbgDiggerWinNtIWinNt_QueryVersion(struct DBGFOSIWINNT *pThis, PUVM pUVM,
                                                           uint32_t *puVersMajor, uint32_t *puVersMinor,
                                                           uint32_t *puBuildNumber, bool *pf32Bit)
{
    RT_NOREF(pUVM);
    PDBGDIGGERWINNT pData = RT_FROM_MEMBER(pThis, DBGDIGGERWINNT, IWinNt);

    if (puVersMajor)
        *puVersMajor = pData->NtMajorVersion;
    if (puVersMinor)
        *puVersMinor = pData->NtMinorVersion;
    if (puBuildNumber)
        *puBuildNumber = pData->NtBuildNumber;
    if (pf32Bit)
        *pf32Bit = pData->f32Bit;
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{DBGFOSIWINNT,pfnQueryKernelPtrs}
 */
static DECLCALLBACK(int) dbgDiggerWinNtIWinNt_QueryKernelPtrs(struct DBGFOSIWINNT *pThis, PUVM pUVM,
                                                              PRTGCUINTPTR pGCPtrKernBase, PRTGCUINTPTR pGCPtrPsLoadedModuleList)
{
    RT_NOREF(pUVM);
    PDBGDIGGERWINNT pData = RT_FROM_MEMBER(pThis, DBGDIGGERWINNT, IWinNt);

    *pGCPtrKernBase           = pData->KernelAddr.FlatPtr;
    *pGCPtrPsLoadedModuleList = pData->PsLoadedModuleListAddr.FlatPtr;
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{DBGFOSIWINNT,pfnQueryKpcrForVCpu}
 */
static DECLCALLBACK(int) dbgDiggerWinNtIWinNt_QueryKpcrForVCpu(struct DBGFOSIWINNT *pThis, PUVM pUVM, VMCPUID idCpu,
                                                               PRTGCUINTPTR pKpcr, PRTGCUINTPTR pKpcrb)
{
    PDBGDIGGERWINNT pData = RT_FROM_MEMBER(pThis, DBGDIGGERWINNT, IWinNt);

    if (!pData->paKpcrAddr)
        return VERR_NOT_SUPPORTED;

    AssertReturn(idCpu < DBGFR3CpuGetCount(pUVM), VERR_INVALID_PARAMETER);

    if (pKpcr)
        *pKpcr = pData->paKpcrAddr[idCpu].FlatPtr;
    if (pKpcrb)
        *pKpcrb = pData->paKpcrbAddr[idCpu].FlatPtr;
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{DBGFOSIWINNT,pfnQueryCurThrdForVCpu}
 */
static DECLCALLBACK(int) dbgDiggerWinNtIWinNt_QueryCurThrdForVCpu(struct DBGFOSIWINNT *pThis, PUVM pUVM, VMCPUID idCpu,
                                                                  PRTGCUINTPTR pCurThrd)
{
    PDBGDIGGERWINNT pData = RT_FROM_MEMBER(pThis, DBGDIGGERWINNT, IWinNt);

    if (!pData->paKpcrAddr)
        return VERR_NOT_SUPPORTED;

    AssertReturn(idCpu < DBGFR3CpuGetCount(pUVM), VERR_INVALID_PARAMETER);

    DBGFADDRESS AddrCurThrdPtr = pData->paKpcrbAddr[idCpu];
    DBGFR3AddrAdd(&AddrCurThrdPtr, 0x08); /** @todo Make this prettier. */
    return DBGFR3MemRead(pUVM, idCpu, &AddrCurThrdPtr, pCurThrd, sizeof(*pCurThrd));
}


/**
 * @copydoc DBGFOSREG::pfnStackUnwindAssist
 */
static DECLCALLBACK(int) dbgDiggerWinNtStackUnwindAssist(PUVM pUVM, void *pvData, VMCPUID idCpu, PDBGFSTACKFRAME pFrame,
                                                         PRTDBGUNWINDSTATE pState, PCCPUMCTX pInitialCtx, RTDBGAS hAs,
                                                         uint64_t *puScratch)
{
    Assert(pInitialCtx);

    /*
     * We want to locate trap frames here.  The trap frame structure contains
     * the 64-bit IRET frame, so given unwind information it's easy to identify
     * using the return type and frame address.
     */
    if (pFrame->fFlags & DBGFSTACKFRAME_FLAGS_64BIT)
    {
        /*
         * Is this a trap frame?  If so, try read the trap frame.
         */
        if (   pFrame->enmReturnType == RTDBGRETURNTYPE_IRET64
            && !(pFrame->AddrFrame.FlatPtr & 0x7)
            && WINNT64_VALID_ADDRESS(pFrame->AddrFrame.FlatPtr) )
        {
            KTRAP_FRAME_AMD64 TrapFrame;
            RT_ZERO(TrapFrame);
            uint64_t const uTrapFrameAddr = pFrame->AddrFrame.FlatPtr
                                          - RT_UOFFSETOF(KTRAP_FRAME_AMD64, ErrCdOrXcptFrameOrS);
            int rc = pState->pfnReadStack(pState, uTrapFrameAddr, sizeof(TrapFrame), &TrapFrame);
            if (RT_SUCCESS(rc))
            {
                /* Valid?  Not too much else we can check here (EFlags isn't
                   reliable in manually construct frames). */
                if (TrapFrame.ExceptionActive <= 2)
                {
                    pFrame->fFlags |= DBGFSTACKFRAME_FLAGS_TRAP_FRAME;

                    /*
                     * Add sure 'register' information from the frame to the frame.
                     *
                     * To avoid code duplication, we do this in two steps in a loop.
                     * The first iteration only figures out how many registers we're
                     * going to save and allocates room for them.  The second iteration
                     * does the actual adding.
                     */
                    uint32_t      cRegs      = pFrame->cSureRegs;
                    PDBGFREGVALEX paSureRegs = NULL;
#define ADD_REG_NAMED(a_Type, a_ValMemb, a_Value, a_pszName) do { \
                            if (paSureRegs) \
                            { \
                                paSureRegs[iReg].pszName         = a_pszName;\
                                paSureRegs[iReg].enmReg          = DBGFREG_END; \
                                paSureRegs[iReg].enmType         = a_Type; \
                                paSureRegs[iReg].Value.a_ValMemb = (a_Value); \
                            } \
                            iReg++; \
                        } while (0)
#define MAYBE_ADD_GREG(a_Value, a_enmReg, a_idxReg) do { \
                            if (!(pState->u.x86.Loaded.s.fRegs & RT_BIT(a_idxReg))) \
                            { \
                                if (paSureRegs) \
                                { \
                                    pState->u.x86.Loaded.s.fRegs    |= RT_BIT(a_idxReg); \
                                    pState->u.x86.auRegs[a_idxReg]   = (a_Value); \
                                    paSureRegs[iReg].Value.u64       = (a_Value); \
                                    paSureRegs[iReg].enmReg          = a_enmReg; \
                                    paSureRegs[iReg].enmType         = DBGFREGVALTYPE_U64; \
                                    paSureRegs[iReg].pszName         = NULL; \
                                } \
                                iReg++; \
                            } \
                        } while (0)
                    for (unsigned iLoop = 0; iLoop < 2; iLoop++)
                    {
                        uint32_t iReg = pFrame->cSureRegs;
                        ADD_REG_NAMED(DBGFREGVALTYPE_U64, u64, uTrapFrameAddr, "TrapFrame");
                        ADD_REG_NAMED(DBGFREGVALTYPE_U8,   u8, TrapFrame.ExceptionActive, "ExceptionActive");
                        if (TrapFrame.ExceptionActive == 0)
                        {
                            ADD_REG_NAMED(DBGFREGVALTYPE_U8,   u8, TrapFrame.PreviousIrql, "PrevIrql");
                            ADD_REG_NAMED(DBGFREGVALTYPE_U8,   u8, (uint8_t)TrapFrame.ErrCdOrXcptFrameOrS, "IntNo");
                        }
                        else if (   TrapFrame.ExceptionActive == 1
                                 && TrapFrame.FaultIndicator == ((TrapFrame.ErrCdOrXcptFrameOrS >> 1) & 0x9))
                            ADD_REG_NAMED(DBGFREGVALTYPE_U64, u64, TrapFrame.FaultAddrOrCtxRecOrTS, "cr2-probably");
                        if (TrapFrame.SegCs & X86_SEL_RPL)
                            ADD_REG_NAMED(DBGFREGVALTYPE_U8,   u8, 1, "UserMode");
                        else
                            ADD_REG_NAMED(DBGFREGVALTYPE_U8,   u8, 1, "KernelMode");
                        if (TrapFrame.ExceptionActive <= 1)
                        {
                            MAYBE_ADD_GREG(TrapFrame.Rax, DBGFREG_RAX, X86_GREG_xAX);
                            MAYBE_ADD_GREG(TrapFrame.Rcx, DBGFREG_RCX, X86_GREG_xCX);
                            MAYBE_ADD_GREG(TrapFrame.Rdx, DBGFREG_RDX, X86_GREG_xDX);
                            MAYBE_ADD_GREG(TrapFrame.R8,  DBGFREG_R8,  X86_GREG_x8);
                            MAYBE_ADD_GREG(TrapFrame.R9,  DBGFREG_R9,  X86_GREG_x9);
                            MAYBE_ADD_GREG(TrapFrame.R10, DBGFREG_R10, X86_GREG_x10);
                            MAYBE_ADD_GREG(TrapFrame.R11, DBGFREG_R11, X86_GREG_x11);
                        }
                        else if (TrapFrame.ExceptionActive == 2)
                        {
                            MAYBE_ADD_GREG(TrapFrame.Rbx, DBGFREG_RBX, X86_GREG_xBX);
                            MAYBE_ADD_GREG(TrapFrame.Rsi, DBGFREG_RSI, X86_GREG_xSI);
                            MAYBE_ADD_GREG(TrapFrame.Rdi, DBGFREG_RDI, X86_GREG_xDI);
                        }
                        // MAYBE_ADD_GREG(TrapFrame.Rbp, DBGFREG_RBP, X86_GREG_xBP); - KiInterrupt[Sub]Dispatch* may leave this invalid.

                        /* Done? */
                        if (iLoop > 0)
                        {
                            Assert(cRegs == iReg);
                            break;
                        }

                        /* Resize the array, zeroing the extension. */
                        if (pFrame->cSureRegs)
                            paSureRegs = (PDBGFREGVALEX)MMR3HeapRealloc(pFrame->paSureRegs, iReg * sizeof(paSureRegs[0]));
                        else
                            paSureRegs = (PDBGFREGVALEX)MMR3HeapAllocU(pUVM, MM_TAG_DBGF_STACK, iReg * sizeof(paSureRegs[0]));
                        AssertReturn(paSureRegs, VERR_NO_MEMORY);

                        pFrame->paSureRegs = paSureRegs;
                        RT_BZERO(&paSureRegs[pFrame->cSureRegs], (iReg - pFrame->cSureRegs) * sizeof(paSureRegs[0]));
                        cRegs = iReg;
                    }
#undef ADD_REG_NAMED
#undef MAYBE_ADD_GREG

                    /* Commit the register update. */
                    pFrame->cSureRegs = cRegs;
                }
            }
        }
    }

    RT_NOREF(pUVM, pvData, idCpu, hAs, pInitialCtx, puScratch);
    return VINF_SUCCESS;
}


/**
 * @copydoc DBGFOSREG::pfnQueryInterface
 */
static DECLCALLBACK(void *) dbgDiggerWinNtQueryInterface(PUVM pUVM, void *pvData, DBGFOSINTERFACE enmIf)
{
    RT_NOREF(pUVM);
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;

    switch (enmIf)
    {
        case DBGFOSINTERFACE_WINNT:
            return &pThis->IWinNt;
        default:
            return NULL;
    }
}


/**
 * @copydoc DBGFOSREG::pfnQueryVersion
 */
static DECLCALLBACK(int)  dbgDiggerWinNtQueryVersion(PUVM pUVM, void *pvData, char *pszVersion, size_t cchVersion)
{
    RT_NOREF1(pUVM);
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;
    Assert(pThis->fValid);
    const char *pszNtProductType;
    switch (pThis->NtProductType)
    {
        case kNtProductType_WinNt:      pszNtProductType = "-WinNT";        break;
        case kNtProductType_LanManNt:   pszNtProductType = "-LanManNT";     break;
        case kNtProductType_Server:     pszNtProductType = "-Server";       break;
        default:                        pszNtProductType = "";              break;
    }
    RTStrPrintf(pszVersion, cchVersion, "%u.%u-%s%s (BuildNumber %u)", pThis->NtMajorVersion, pThis->NtMinorVersion,
                pThis->f32Bit ? "x86" : "AMD64", pszNtProductType, pThis->NtBuildNumber);
    return VINF_SUCCESS;
}


/**
 * @copydoc DBGFOSREG::pfnTerm
 */
static DECLCALLBACK(void)  dbgDiggerWinNtTerm(PUVM pUVM, void *pvData)
{
    RT_NOREF1(pUVM);
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;
    Assert(pThis->fValid);

    /*
     * As long as we're using our private LDR reader implementation,
     * we must unlink and ditch the modules we created.
     */
    RTDBGAS hDbgAs = DBGFR3AsResolveAndRetain(pUVM, DBGF_AS_KERNEL);
    if (hDbgAs != NIL_RTDBGAS)
    {
        uint32_t iMod = RTDbgAsModuleCount(hDbgAs);
        while (iMod-- > 0)
        {
            RTDBGMOD hMod = RTDbgAsModuleByIndex(hDbgAs, iMod);
            if (hMod != NIL_RTDBGMOD)
            {
                if (RTDbgModGetTag(hMod) == DIG_WINNT_MOD_TAG)
                {
                    int rc = RTDbgAsModuleUnlink(hDbgAs, hMod);
                    AssertRC(rc);
                }
                RTDbgModRelease(hMod);
            }
        }
        RTDbgAsRelease(hDbgAs);
    }

    if (pThis->paKpcrAddr)
        RTMemFree(pThis->paKpcrAddr);
    /* pThis->paKpcrbAddr comes from the same allocation as pThis->paKpcrAddr. */

    pThis->paKpcrAddr  = NULL;
    pThis->paKpcrbAddr = NULL;

    pThis->fValid = false;
}


/**
 * @copydoc DBGFOSREG::pfnRefresh
 */
static DECLCALLBACK(int)  dbgDiggerWinNtRefresh(PUVM pUVM, void *pvData)
{
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;
    NOREF(pThis);
    Assert(pThis->fValid);

    /*
     * For now we'll flush and reload everything.
     */
    dbgDiggerWinNtTerm(pUVM, pvData);

    return dbgDiggerWinNtInit(pUVM, pvData);
}


/**
 * @copydoc DBGFOSREG::pfnInit
 */
static DECLCALLBACK(int)  dbgDiggerWinNtInit(PUVM pUVM, void *pvData)
{
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;
    Assert(!pThis->fValid);

    union
    {
        uint8_t             au8[0x2000];
        RTUTF16             wsz[0x2000/2];
        NTKUSERSHAREDDATA   UserSharedData;
    }               u;
    DBGFADDRESS     Addr;
    int             rc;

    /*
     * Figure the NT version.
     */
    DBGFR3AddrFromFlat(pUVM, &Addr, pThis->f32Bit ? NTKUSERSHAREDDATA_WINNT32 : NTKUSERSHAREDDATA_WINNT64);
    rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &Addr, &u, PAGE_SIZE);
    if (RT_SUCCESS(rc))
    {
        pThis->NtProductType  = u.UserSharedData.ProductTypeIsValid && u.UserSharedData.NtProductType <= kNtProductType_Server
                              ? (NTPRODUCTTYPE)u.UserSharedData.NtProductType
                              : kNtProductType_Invalid;
        pThis->NtMajorVersion = u.UserSharedData.NtMajorVersion;
        pThis->NtMinorVersion = u.UserSharedData.NtMinorVersion;
        pThis->NtBuildNumber  = u.UserSharedData.NtBuildNumber;
    }
    else if (pThis->fNt31)
    {
        pThis->NtProductType  = kNtProductType_WinNt;
        pThis->NtMajorVersion = 3;
        pThis->NtMinorVersion = 1;
        pThis->NtBuildNumber  = 0;
    }
    else
    {
        Log(("DigWinNt: Error reading KUSER_SHARED_DATA: %Rrc\n", rc));
        return rc;
    }

    /*
     * Dig out the module chain.
     */
    DBGFADDRESS AddrPrev = pThis->PsLoadedModuleListAddr;
    Addr                 = pThis->KernelMteAddr;
    do
    {
        /* Read the validate the MTE. */
        NTMTE Mte;
        rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &Addr, &Mte, pThis->f32Bit ? sizeof(Mte.vX_32) : sizeof(Mte.vX_64));
        if (RT_FAILURE(rc))
            break;
        if (WINNT_UNION(pThis, &Mte, InLoadOrderLinks.Blink) != AddrPrev.FlatPtr)
        {
            Log(("DigWinNt: Bad Mte At %RGv - backpointer\n", Addr.FlatPtr));
            break;
        }
        if (!WINNT_VALID_ADDRESS(pThis, WINNT_UNION(pThis, &Mte, InLoadOrderLinks.Flink)) )
        {
            Log(("DigWinNt: Bad Mte at %RGv - forward pointer\n", Addr.FlatPtr));
            break;
        }
        if (!WINNT_VALID_ADDRESS(pThis, WINNT_UNION(pThis, &Mte, BaseDllName.Buffer)))
        {
            Log(("DigWinNt: Bad Mte at %RGv - BaseDllName=%llx\n", Addr.FlatPtr, WINNT_UNION(pThis, &Mte, BaseDllName.Buffer)));
            break;
        }
        if (!WINNT_VALID_ADDRESS(pThis, WINNT_UNION(pThis, &Mte, FullDllName.Buffer)))
        {
            Log(("DigWinNt: Bad Mte at %RGv - FullDllName=%llx\n", Addr.FlatPtr, WINNT_UNION(pThis, &Mte, FullDllName.Buffer)));
            break;
        }
        if (!WINNT_VALID_ADDRESS(pThis, WINNT_UNION(pThis, &Mte, DllBase)))
        {
            Log(("DigWinNt: Bad Mte at %RGv - DllBase=%llx\n", Addr.FlatPtr, WINNT_UNION(pThis, &Mte, DllBase) ));
            break;
        }

        uint32_t const cbImageMte = !pThis->fNt31 ? WINNT_UNION(pThis, &Mte, SizeOfImage) : 0;
        if (   !pThis->fNt31
            && (   cbImageMte > _256M
                || WINNT_UNION(pThis, &Mte, EntryPoint) - WINNT_UNION(pThis, &Mte, DllBase) > cbImageMte) )
        {
            Log(("DigWinNt: Bad Mte at %RGv - EntryPoint=%llx SizeOfImage=%x DllBase=%llx\n",
                 Addr.FlatPtr, WINNT_UNION(pThis, &Mte, EntryPoint), cbImageMte, WINNT_UNION(pThis, &Mte, DllBase)));
            break;
        }

        /* Read the full name. */
        DBGFADDRESS AddrName;
        DBGFR3AddrFromFlat(pUVM, &AddrName, WINNT_UNION(pThis, &Mte, FullDllName.Buffer));
        uint16_t    cbName = WINNT_UNION(pThis, &Mte, FullDllName.Length);
        if (cbName < sizeof(u))
            rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &AddrName, &u, cbName);
        else
            rc = VERR_OUT_OF_RANGE;
        if (RT_FAILURE(rc))
        {
            DBGFR3AddrFromFlat(pUVM, &AddrName, WINNT_UNION(pThis, &Mte, BaseDllName.Buffer));
            cbName = WINNT_UNION(pThis, &Mte, BaseDllName.Length);
            if (cbName < sizeof(u))
                rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &AddrName, &u, cbName);
            else
                rc = VERR_OUT_OF_RANGE;
        }
        if (RT_SUCCESS(rc))
        {
            u.wsz[cbName / 2] = '\0';

            char *pszFilename;
            rc = RTUtf16ToUtf8(u.wsz, &pszFilename);
            if (RT_SUCCESS(rc))
            {
                char        szModName[128];
                const char *pszModName = dbgDiggerWintNtFilenameToModuleName(pszFilename, szModName, sizeof(szModName));

                /* Read the start of the PE image and pass it along to a worker. */
                DBGFADDRESS ImageAddr;
                DBGFR3AddrFromFlat(pUVM, &ImageAddr, WINNT_UNION(pThis, &Mte, DllBase));
                dbgDiggerWinNtProcessImage(pThis, pUVM, pszModName, pszFilename, &ImageAddr, cbImageMte);
                RTStrFree(pszFilename);
            }
        }

        /* next */
        AddrPrev = Addr;
        DBGFR3AddrFromFlat(pUVM, &Addr, WINNT_UNION(pThis, &Mte, InLoadOrderLinks.Flink));
    } while (   Addr.FlatPtr != pThis->KernelMteAddr.FlatPtr
             && Addr.FlatPtr != pThis->PsLoadedModuleListAddr.FlatPtr);

    /* Try resolving the KPCR and KPCRB addresses for each vCPU. */
    dbgDiggerWinNtResolveKpcr(pThis, pUVM);

    pThis->fValid = true;
    return VINF_SUCCESS;
}


/**
 * @copydoc DBGFOSREG::pfnProbe
 */
static DECLCALLBACK(bool)  dbgDiggerWinNtProbe(PUVM pUVM, void *pvData)
{
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;
    DBGFADDRESS     Addr;
    union
    {
        uint8_t             au8[8192];
        uint16_t            au16[8192/2];
        uint32_t            au32[8192/4];
        IMAGE_DOS_HEADER    MzHdr;
        RTUTF16             wsz[8192/2];
        X86DESCGATE         a32Gates[X86_XCPT_PF + 1];
        X86DESC64GATE       a64Gates[X86_XCPT_PF + 1];
    } u;

    union
    {
        NTMTE32 v32;
        NTMTE64 v64;
    } uMte, uMte2, uMte3;

    /*
     * NT only runs in protected or long mode.
     */
    CPUMMODE const enmMode = DBGFR3CpuGetMode(pUVM, 0 /*idCpu*/);
    if (enmMode != CPUMMODE_PROTECTED && enmMode != CPUMMODE_LONG)
        return false;
    bool const      f64Bit = enmMode == CPUMMODE_LONG;
    uint64_t const  uStart = f64Bit ? UINT64_C(0xffff080000000000) : UINT32_C(0x80001000);
    uint64_t const  uEnd   = f64Bit ? UINT64_C(0xffffffffffff0000) : UINT32_C(0xffff0000);

    /*
     * To approximately locate the kernel we examine the IDTR handlers.
     *
     * The exception/trap/fault handlers are all in NT kernel image, we pick
     * KiPageFault here.
     */
    uint64_t uIdtrBase = 0;
    uint16_t uIdtrLimit = 0;
    int rc = DBGFR3RegCpuQueryXdtr(pUVM, 0, DBGFREG_IDTR, &uIdtrBase, &uIdtrLimit);
    AssertRCReturn(rc, false);

    const uint16_t cbMinIdtr = (X86_XCPT_PF + 1) * (f64Bit ? sizeof(X86DESC64GATE) : sizeof(X86DESCGATE));
    if (uIdtrLimit < cbMinIdtr)
        return false;

    rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, DBGFR3AddrFromFlat(pUVM, &Addr, uIdtrBase), &u, cbMinIdtr);
    if (RT_FAILURE(rc))
        return false;

    uint64_t uKrnlStart  = uStart;
    uint64_t uKrnlEnd    = uEnd;
    if (f64Bit)
    {
        uint64_t uHandler = u.a64Gates[X86_XCPT_PF].u16OffsetLow
                          | ((uint32_t)u.a64Gates[X86_XCPT_PF].u16OffsetHigh << 16)
                          | ((uint64_t)u.a64Gates[X86_XCPT_PF].u32OffsetTop  << 32);
        if (uHandler < uStart || uHandler > uEnd)
            return false;
        uKrnlStart = (uHandler & ~(uint64_t)_4M) - _512M;
        uKrnlEnd   = (uHandler + (uint64_t)_4M) & ~(uint64_t)_4M;
    }
    else
    {
        uint32_t uHandler = RT_MAKE_U32(u.a32Gates[X86_XCPT_PF].u16OffsetLow, u.a32Gates[X86_XCPT_PF].u16OffsetHigh);
        if (uHandler < uStart || uHandler > uEnd)
            return false;
        uKrnlStart = (uHandler & ~(uint64_t)_4M) - _64M;
        uKrnlEnd   = (uHandler + (uint64_t)_4M) & ~(uint64_t)_4M;
    }

    /*
     * Look for the PAGELK section name that seems to be a part of all kernels.
     * Then try find the module table entry for it.  Since it's the first entry
     * in the PsLoadedModuleList we can easily validate the list head and report
     * success.
     *
     * Note! We ASSUME the section name is 8 byte aligned.
     */
    DBGFADDRESS KernelAddr;
    for (DBGFR3AddrFromFlat(pUVM, &KernelAddr, uKrnlStart);
         KernelAddr.FlatPtr < uKrnlEnd;
         KernelAddr.FlatPtr += PAGE_SIZE)
    {
        bool fNt31 = false;
        DBGFADDRESS const RetryAddress = KernelAddr;
        rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &KernelAddr, uEnd - KernelAddr.FlatPtr,
                           8, "PAGELK\0", sizeof("PAGELK\0"), &KernelAddr);
        if (   rc == VERR_DBGF_MEM_NOT_FOUND
            && enmMode != CPUMMODE_LONG)
        {
            /* NT3.1 didn't have a PAGELK section, so look for _TEXT instead.  The
               following VirtualSize is zero, so check for that too. */
            rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &RetryAddress, uEnd - RetryAddress.FlatPtr,
                               8, "_TEXT\0\0\0\0\0\0", sizeof("_TEXT\0\0\0\0\0\0"), &KernelAddr);
            fNt31 = true;
        }
        if (RT_FAILURE(rc))
            break;
        DBGFR3AddrSub(&KernelAddr, KernelAddr.FlatPtr & PAGE_OFFSET_MASK);

        /* MZ + PE header. */
        rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &KernelAddr, &u, sizeof(u));
        if (    RT_SUCCESS(rc)
            &&  u.MzHdr.e_magic == IMAGE_DOS_SIGNATURE
            &&  !(u.MzHdr.e_lfanew & 0x7)
            &&  u.MzHdr.e_lfanew >= 0x080
            &&  u.MzHdr.e_lfanew <= 0x400) /* W8 is at 0x288*/
        {
            if (enmMode != CPUMMODE_LONG)
            {
                IMAGE_NT_HEADERS32 const *pHdrs = (IMAGE_NT_HEADERS32 const *)&u.au8[u.MzHdr.e_lfanew];
                if (    pHdrs->Signature                            == IMAGE_NT_SIGNATURE
                    &&  pHdrs->FileHeader.Machine                   == IMAGE_FILE_MACHINE_I386
                    &&  pHdrs->FileHeader.SizeOfOptionalHeader      == sizeof(pHdrs->OptionalHeader)
                    &&  pHdrs->FileHeader.NumberOfSections          >= 10 /* the kernel has lots */
                    &&  (pHdrs->FileHeader.Characteristics & (IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL)) == IMAGE_FILE_EXECUTABLE_IMAGE
                    &&  pHdrs->OptionalHeader.Magic                 == IMAGE_NT_OPTIONAL_HDR32_MAGIC
                    &&  pHdrs->OptionalHeader.NumberOfRvaAndSizes   == IMAGE_NUMBEROF_DIRECTORY_ENTRIES
                    )
                {
                    /* Find the MTE. */
                    RT_ZERO(uMte);
                    uMte.v32.DllBase     = KernelAddr.FlatPtr;
                    uMte.v32.EntryPoint  = KernelAddr.FlatPtr + pHdrs->OptionalHeader.AddressOfEntryPoint;
                    uMte.v32.SizeOfImage = !fNt31 ? pHdrs->OptionalHeader.SizeOfImage : 0; /* NT 3.1 didn't set the size. */
                    DBGFADDRESS HitAddr;
                    rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &KernelAddr, uEnd - KernelAddr.FlatPtr,
                                       4 /*align*/, &uMte.v32.DllBase, 3 * sizeof(uint32_t), &HitAddr);
                    while (RT_SUCCESS(rc))
                    {
                        /* check the name. */
                        DBGFADDRESS MteAddr = HitAddr;
                        rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, DBGFR3AddrSub(&MteAddr, RT_OFFSETOF(NTMTE32, DllBase)),
                                           &uMte2.v32, sizeof(uMte2.v32));
                        if (    RT_SUCCESS(rc)
                            &&  uMte2.v32.DllBase     == uMte.v32.DllBase
                            &&  uMte2.v32.EntryPoint  == uMte.v32.EntryPoint
                            &&  uMte2.v32.SizeOfImage == uMte.v32.SizeOfImage
                            &&  WINNT32_VALID_ADDRESS(uMte2.v32.InLoadOrderLinks.Flink)
                            &&  WINNT32_VALID_ADDRESS(uMte2.v32.BaseDllName.Buffer)
                            &&  WINNT32_VALID_ADDRESS(uMte2.v32.FullDllName.Buffer)
                            &&  uMte2.v32.BaseDllName.Length <= 128
                            &&  uMte2.v32.FullDllName.Length <= 260
                           )
                        {
                            rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, DBGFR3AddrFromFlat(pUVM, &Addr, uMte2.v32.BaseDllName.Buffer),
                                               u.wsz, uMte2.v32.BaseDllName.Length);
                            u.wsz[uMte2.v32.BaseDllName.Length / 2] = '\0';
                            if (    RT_SUCCESS(rc)
                                &&  (   !RTUtf16ICmp(u.wsz, g_wszKernelNames[0])
                                  /* || !RTUtf16ICmp(u.wsz, g_wszKernelNames[1]) */
                                    )
                               )
                            {
                                rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/,
                                                   DBGFR3AddrFromFlat(pUVM, &Addr, uMte2.v32.InLoadOrderLinks.Blink),
                                                   &uMte3.v32, RT_SIZEOFMEMB(NTMTE32, InLoadOrderLinks));
                                if (   RT_SUCCESS(rc)
                                    && uMte3.v32.InLoadOrderLinks.Flink == MteAddr.FlatPtr
                                    && WINNT32_VALID_ADDRESS(uMte3.v32.InLoadOrderLinks.Blink) )
                                {
                                    Log(("DigWinNt: MteAddr=%RGv KernelAddr=%RGv SizeOfImage=%x &PsLoadedModuleList=%RGv (32-bit)\n",
                                         MteAddr.FlatPtr, KernelAddr.FlatPtr, uMte2.v32.SizeOfImage, Addr.FlatPtr));
                                    pThis->KernelAddr               = KernelAddr;
                                    pThis->KernelMteAddr            = MteAddr;
                                    pThis->PsLoadedModuleListAddr   = Addr;
                                    pThis->f32Bit                   = true;
                                    pThis->fNt31                    = fNt31;
                                    return true;
                                }
                            }
                            else if (RT_SUCCESS(rc))
                            {
                                Log2(("DigWinNt: Wrong module: MteAddr=%RGv ImageAddr=%RGv SizeOfImage=%#x '%ls'\n",
                                      MteAddr.FlatPtr, KernelAddr.FlatPtr, uMte2.v32.SizeOfImage, u.wsz));
                                break; /* Not NT kernel */
                            }
                        }

                        /* next */
                        DBGFR3AddrAdd(&HitAddr, 4);
                        if (HitAddr.FlatPtr < uEnd)
                            rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &HitAddr, uEnd - HitAddr.FlatPtr,
                                               4 /*align*/, &uMte.v32.DllBase, 3 * sizeof(uint32_t), &HitAddr);
                        else
                            rc = VERR_DBGF_MEM_NOT_FOUND;
                    }
                }
            }
            else
            {
                IMAGE_NT_HEADERS64 const *pHdrs = (IMAGE_NT_HEADERS64 const *)&u.au8[u.MzHdr.e_lfanew];
                if (    pHdrs->Signature                            == IMAGE_NT_SIGNATURE
                    &&  pHdrs->FileHeader.Machine                   == IMAGE_FILE_MACHINE_AMD64
                    &&  pHdrs->FileHeader.SizeOfOptionalHeader      == sizeof(pHdrs->OptionalHeader)
                    &&  pHdrs->FileHeader.NumberOfSections          >= 10 /* the kernel has lots */
                    &&      (pHdrs->FileHeader.Characteristics & (IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL))
                         == IMAGE_FILE_EXECUTABLE_IMAGE
                    &&  pHdrs->OptionalHeader.Magic                 == IMAGE_NT_OPTIONAL_HDR64_MAGIC
                    &&  pHdrs->OptionalHeader.NumberOfRvaAndSizes   == IMAGE_NUMBEROF_DIRECTORY_ENTRIES
                    )
                {
                    /* Find the MTE. */
                    RT_ZERO(uMte.v64);
                    uMte.v64.DllBase     = KernelAddr.FlatPtr;
                    uMte.v64.EntryPoint  = KernelAddr.FlatPtr + pHdrs->OptionalHeader.AddressOfEntryPoint;
                    uMte.v64.SizeOfImage = pHdrs->OptionalHeader.SizeOfImage;
                    DBGFADDRESS ScanAddr;
                    DBGFADDRESS HitAddr;
                    rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, DBGFR3AddrFromFlat(pUVM, &ScanAddr, uStart),
                                       uEnd - uStart, 8 /*align*/, &uMte.v64.DllBase, 5 * sizeof(uint32_t), &HitAddr);
                    while (RT_SUCCESS(rc))
                    {
                        /* Read the start of the MTE and check some basic members. */
                        DBGFADDRESS MteAddr = HitAddr;
                        rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, DBGFR3AddrSub(&MteAddr, RT_OFFSETOF(NTMTE64, DllBase)),
                                           &uMte2.v64, sizeof(uMte2.v64));
                        if (    RT_SUCCESS(rc)
                            &&  uMte2.v64.DllBase     == uMte.v64.DllBase
                            &&  uMte2.v64.EntryPoint  == uMte.v64.EntryPoint
                            &&  uMte2.v64.SizeOfImage == uMte.v64.SizeOfImage
                            &&  WINNT64_VALID_ADDRESS(uMte2.v64.InLoadOrderLinks.Flink)
                            &&  WINNT64_VALID_ADDRESS(uMte2.v64.BaseDllName.Buffer)
                            &&  WINNT64_VALID_ADDRESS(uMte2.v64.FullDllName.Buffer)
                            &&  uMte2.v64.BaseDllName.Length <= 128
                            &&  uMte2.v64.FullDllName.Length <= 260
                            )
                        {
                            /* Try read the base name and compare with known NT kernel names. */
                            rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, DBGFR3AddrFromFlat(pUVM, &Addr, uMte2.v64.BaseDllName.Buffer),
                                               u.wsz, uMte2.v64.BaseDllName.Length);
                            u.wsz[uMte2.v64.BaseDllName.Length / 2] = '\0';
                            if (    RT_SUCCESS(rc)
                                &&  (   !RTUtf16ICmp(u.wsz, g_wszKernelNames[0])
                                  /* || !RTUtf16ICmp(u.wsz, g_wszKernelNames[1]) */
                                    )
                               )
                            {
                                /* Read the link entry of the previous entry in the list and check that its
                                   forward pointer points at the MTE we've found. */
                                rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/,
                                                   DBGFR3AddrFromFlat(pUVM, &Addr, uMte2.v64.InLoadOrderLinks.Blink),
                                                   &uMte3.v64, RT_SIZEOFMEMB(NTMTE64, InLoadOrderLinks));
                                if (   RT_SUCCESS(rc)
                                    && uMte3.v64.InLoadOrderLinks.Flink == MteAddr.FlatPtr
                                    && WINNT64_VALID_ADDRESS(uMte3.v64.InLoadOrderLinks.Blink) )
                                {
                                    Log(("DigWinNt: MteAddr=%RGv KernelAddr=%RGv SizeOfImage=%x &PsLoadedModuleList=%RGv (32-bit)\n",
                                         MteAddr.FlatPtr, KernelAddr.FlatPtr, uMte2.v64.SizeOfImage, Addr.FlatPtr));
                                    pThis->KernelAddr               = KernelAddr;
                                    pThis->KernelMteAddr            = MteAddr;
                                    pThis->PsLoadedModuleListAddr   = Addr;
                                    pThis->f32Bit                   = false;
                                    pThis->fNt31                    = false;
                                    return true;
                                }
                            }
                            else if (RT_SUCCESS(rc))
                            {
                                Log2(("DigWinNt: Wrong module: MteAddr=%RGv ImageAddr=%RGv SizeOfImage=%#x '%ls'\n",
                                      MteAddr.FlatPtr, KernelAddr.FlatPtr, uMte2.v64.SizeOfImage, u.wsz));
                                break; /* Not NT kernel */
                            }
                        }

                        /* next */
                        DBGFR3AddrAdd(&HitAddr, 8);
                        if (HitAddr.FlatPtr < uEnd)
                            rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &HitAddr, uEnd - HitAddr.FlatPtr,
                                               8 /*align*/, &uMte.v64.DllBase, 3 * sizeof(uint32_t), &HitAddr);
                        else
                            rc = VERR_DBGF_MEM_NOT_FOUND;
                    }
                }
            }
        }
    }
    return false;
}


/**
 * @copydoc DBGFOSREG::pfnDestruct
 */
static DECLCALLBACK(void)  dbgDiggerWinNtDestruct(PUVM pUVM, void *pvData)
{
    RT_NOREF2(pUVM, pvData);
}


/**
 * @copydoc DBGFOSREG::pfnConstruct
 */
static DECLCALLBACK(int)  dbgDiggerWinNtConstruct(PUVM pUVM, void *pvData)
{
    RT_NOREF1(pUVM);
    PDBGDIGGERWINNT pThis = (PDBGDIGGERWINNT)pvData;
    pThis->fValid      = false;
    pThis->f32Bit      = false;
    pThis->enmVer      = DBGDIGGERWINNTVER_UNKNOWN;

    pThis->IWinNt.u32Magic               = DBGFOSIWINNT_MAGIC;
    pThis->IWinNt.pfnQueryVersion        = dbgDiggerWinNtIWinNt_QueryVersion;
    pThis->IWinNt.pfnQueryKernelPtrs     = dbgDiggerWinNtIWinNt_QueryKernelPtrs;
    pThis->IWinNt.pfnQueryKpcrForVCpu    = dbgDiggerWinNtIWinNt_QueryKpcrForVCpu;
    pThis->IWinNt.pfnQueryCurThrdForVCpu = dbgDiggerWinNtIWinNt_QueryCurThrdForVCpu;
    pThis->IWinNt.u32EndMagic            = DBGFOSIWINNT_MAGIC;

    return VINF_SUCCESS;
}


const DBGFOSREG g_DBGDiggerWinNt =
{
    /* .u32Magic = */               DBGFOSREG_MAGIC,
    /* .fFlags = */                 0,
    /* .cbData = */                 sizeof(DBGDIGGERWINNT),
    /* .szName = */                 "WinNT",
    /* .pfnConstruct = */           dbgDiggerWinNtConstruct,
    /* .pfnDestruct = */            dbgDiggerWinNtDestruct,
    /* .pfnProbe = */               dbgDiggerWinNtProbe,
    /* .pfnInit = */                dbgDiggerWinNtInit,
    /* .pfnRefresh = */             dbgDiggerWinNtRefresh,
    /* .pfnTerm = */                dbgDiggerWinNtTerm,
    /* .pfnQueryVersion = */        dbgDiggerWinNtQueryVersion,
    /* .pfnQueryInterface = */      dbgDiggerWinNtQueryInterface,
    /* .pfnStackUnwindAssist = */   dbgDiggerWinNtStackUnwindAssist,
    /* .u32EndMagic = */            DBGFOSREG_MAGIC
};

