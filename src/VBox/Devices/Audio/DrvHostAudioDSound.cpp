/* $Id$ */
/** @file
 * Host audio driver - DirectSound (Windows).
 */

/*
 * Copyright (C) 2006-2020 Oracle Corporation
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
#define LOG_GROUP LOG_GROUP_DRV_HOST_AUDIO
#define INITGUID
#include <VBox/log.h>
#include <iprt/win/windows.h>
#include <dsound.h>
#include <Mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <iprt/alloc.h>
#include <iprt/system.h>
#include <iprt/uuid.h>
#include <iprt/utf16.h>

#include <VBox/vmm/pdmaudioinline.h>
#include <VBox/vmm/pdmaudiohostenuminline.h>

#include "VBoxDD.h"
#ifdef VBOX_WITH_AUDIO_MMNOTIFICATION_CLIENT
# include <new> /* For bad_alloc. */
# include "DrvHostAudioDSoundMMNotifClient.h"
#endif


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/*
 * Optional release logging, which a user can turn on with the
 * 'VBoxManage debugvm' command.
 * Debug logging still uses the common Log* macros from VBox.
 * Messages which always should go to the release log use LogRel.
 *
 * @deprecated Use LogRelMax, LogRel2 and LogRel3 directly.
 */
/** General code behavior. */
#define DSLOG(a)    do { LogRel2(a); } while(0)
/** Something which produce a lot of logging during playback/recording. */
#define DSLOGF(a)   do { LogRel3(a); } while(0)
/** Important messages like errors. Limited in the default release log to avoid log flood. */
#define DSLOGREL(a) \
    do {  \
        static int8_t s_cLogged = 0; \
        if (s_cLogged < 8) { \
            ++s_cLogged; \
            LogRel(a); \
        } else DSLOG(a); \
    } while (0)

/** Maximum number of attempts to restore the sound buffer before giving up. */
#define DRV_DSOUND_RESTORE_ATTEMPTS_MAX         3
#if 0 /** @todo r=bird: What are these for? Nobody is using them... */
/** Default input latency (in ms). */
#define DRV_DSOUND_DEFAULT_LATENCY_MS_IN        50
/** Default output latency (in ms). */
#define DRV_DSOUND_DEFAULT_LATENCY_MS_OUT       50
#endif


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/* Dynamically load dsound.dll. */
typedef HRESULT WINAPI FNDIRECTSOUNDENUMERATEW(LPDSENUMCALLBACKW pDSEnumCallback, PVOID pContext);
typedef FNDIRECTSOUNDENUMERATEW *PFNDIRECTSOUNDENUMERATEW;
typedef HRESULT WINAPI FNDIRECTSOUNDCAPTUREENUMERATEW(LPDSENUMCALLBACKW pDSEnumCallback, PVOID pContext);
typedef FNDIRECTSOUNDCAPTUREENUMERATEW *PFNDIRECTSOUNDCAPTUREENUMERATEW;
typedef HRESULT WINAPI FNDIRECTSOUNDCAPTURECREATE8(LPCGUID lpcGUID, LPDIRECTSOUNDCAPTURE8 *lplpDSC, LPUNKNOWN pUnkOuter);
typedef FNDIRECTSOUNDCAPTURECREATE8 *PFNDIRECTSOUNDCAPTURECREATE8;

#define VBOX_DSOUND_MAX_EVENTS 3

typedef enum DSOUNDEVENT
{
    DSOUNDEVENT_NOTIFY = 0,
    DSOUNDEVENT_INPUT,
    DSOUNDEVENT_OUTPUT,
} DSOUNDEVENT;

typedef struct DSOUNDHOSTCFG
{
    RTUUID          uuidPlay;
    LPCGUID         pGuidPlay;
    RTUUID          uuidCapture;
    LPCGUID         pGuidCapture;
} DSOUNDHOSTCFG, *PDSOUNDHOSTCFG;

typedef struct DSOUNDSTREAM
{
    /** The stream's acquired configuration. */
    PDMAUDIOSTREAMCFG   Cfg;
    /** Buffer alignment. */
    uint8_t             uAlign;
    /** Whether this stream is in an enable state on the DirectSound side. */
    bool                fEnabled;
    bool                afPadding[2];
    /** Size (in bytes) of the DirectSound buffer. */
    DWORD               cbBufSize;
    /** The stream's critical section for synchronizing access. */
    RTCRITSECT          CritSect;
    union
    {
        struct
        {
            /** The actual DirectSound Buffer (DSB) used for the capturing.
             *  This is a secondary buffer and is used as a streaming buffer. */
            LPDIRECTSOUNDCAPTUREBUFFER8 pDSCB;
            /** Current read offset (in bytes) within the DSB. */
            DWORD                       offReadPos;
            /** Number of buffer overruns happened. Used for logging. */
            uint8_t                     cOverruns;
        } In;
        struct
        {
            /** The actual DirectSound Buffer (DSB) used for playback.
             *  This is a secondary buffer and is used as a streaming buffer. */
            LPDIRECTSOUNDBUFFER8        pDSB;
            /** Current write offset (in bytes) within the DSB.
             * @note This is needed as the current write position as kept by direct sound
             *       will move ahead if we're too late. */
            DWORD                       offWritePos;
            /** Offset of last play cursor within the DSB when checked for pending. */
            DWORD                       offPlayCursorLastPending;
            /** Offset of last play cursor within the DSB when last played. */
            DWORD                       offPlayCursorLastPlayed;
            /** Total amount (in bytes) written to our internal ring buffer. */
            uint64_t                    cbWritten;
            /** Total amount (in bytes) played (to the DirectSound buffer). */
            uint64_t                    cbTransferred;
            /** Flag indicating whether playback was just (re)started. */
            bool                        fFirstTransfer;
            /** Flag indicating whether this stream is in draining mode, e.g. no new
             *  data is being written to it but DirectSound still needs to be able to
             *  play its remaining (buffered) data. */
            bool                        fDrain;
            /** How much (in bytes) the last transfer from the internal buffer
             *  to the DirectSound buffer was. */
            uint32_t                    cbLastTransferred;
            /** Timestamp (in ms) of the last transfer from the internal buffer
             *  to the DirectSound buffer. */
            uint64_t                    tsLastTransferredMs;
            /** Number of buffer underruns happened. Used for logging. */
            uint8_t                     cUnderruns;
        } Out;
    };
#ifdef LOG_ENABLED
    struct
    {
        uint64_t tsLastTransferredMs;
    } Dbg;
#endif
} DSOUNDSTREAM, *PDSOUNDSTREAM;

/**
 * DirectSound-specific device entry.
 */
typedef struct DSOUNDDEV
{
    PDMAUDIOHOSTDEV  Core;
    /** The GUID if handy. */
    GUID            Guid;
} DSOUNDDEV;
/** Pointer to a DirectSound device entry. */
typedef DSOUNDDEV *PDSOUNDDEV;

/**
 * Structure for holding a device enumeration context.
 */
typedef struct DSOUNDENUMCBCTX
{
    /** Enumeration flags. */
    uint32_t            fFlags;
    /** Pointer to device list to populate. */
    PPDMAUDIOHOSTENUM pDevEnm;
} DSOUNDENUMCBCTX, *PDSOUNDENUMCBCTX;

typedef struct DRVHOSTDSOUND
{
    /** Pointer to the driver instance structure. */
    PPDMDRVINS                  pDrvIns;
    /** Our audio host audio interface. */
    PDMIHOSTAUDIO               IHostAudio;
    /** Critical section to serialize access. */
    RTCRITSECT                  CritSect;
    /** DirectSound configuration options. */
    DSOUNDHOSTCFG               Cfg;
    /** List of devices of last enumeration. */
    PDMAUDIOHOSTENUM            DeviceEnum;
    /** Whether this backend supports any audio input.
     * @todo r=bird: This is not actually used for anything. */
    bool                        fEnabledIn;
    /** Whether this backend supports any audio output.
     * @todo r=bird: This is not actually used for anything. */
    bool                        fEnabledOut;
    /** The Direct Sound playback interface. */
    LPDIRECTSOUND8              pDS;
    /** The Direct Sound capturing interface. */
    LPDIRECTSOUNDCAPTURE8       pDSC;
#ifdef VBOX_WITH_AUDIO_MMNOTIFICATION_CLIENT
    DrvHostAudioDSoundMMNotifClient *m_pNotificationClient;
#endif
    /** Pointer to the input stream. */
    PDSOUNDSTREAM               pDSStrmIn;
    /** Pointer to the output stream. */
    PDSOUNDSTREAM               pDSStrmOut;
} DRVHOSTDSOUND, *PDRVHOSTDSOUND;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static HRESULT  directSoundPlayRestore(PDRVHOSTDSOUND pThis, LPDIRECTSOUNDBUFFER8 pDSB);
static HRESULT  directSoundPlayStart(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS);
static HRESULT  directSoundPlayStop(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, bool fFlush);
static HRESULT  directSoundCaptureStop(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, bool fFlush);

static int      dsoundDevicesEnumerate(PDRVHOSTDSOUND pThis, PDMAUDIOHOSTENUM pDevEnm, uint32_t fEnum);

static int      dsoundStreamEnable(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, bool fEnable);
static void     dsoundUpdateStatusInternal(PDRVHOSTDSOUND pThis);


static DWORD dsoundRingDistance(DWORD offEnd, DWORD offBegin, DWORD cSize)
{
    AssertReturn(offEnd <= cSize,   0);
    AssertReturn(offBegin <= cSize, 0);

    return offEnd >= offBegin ? offEnd - offBegin : cSize - offBegin + offEnd;
}


static char *dsoundGUIDToUtf8StrA(LPCGUID pGUID)
{
    if (pGUID)
    {
        LPOLESTR lpOLEStr;
        HRESULT hr = StringFromCLSID(*pGUID, &lpOLEStr);
        if (SUCCEEDED(hr))
        {
            char *pszGUID;
            int rc = RTUtf16ToUtf8(lpOLEStr, &pszGUID);
            CoTaskMemFree(lpOLEStr);

            return RT_SUCCESS(rc) ? pszGUID : NULL;
        }
    }

    return RTStrDup("{Default device}");
}


static HRESULT directSoundPlayRestore(PDRVHOSTDSOUND pThis, LPDIRECTSOUNDBUFFER8 pDSB)
{
    RT_NOREF(pThis);
    HRESULT hr = IDirectSoundBuffer8_Restore(pDSB);
    if (FAILED(hr))
        DSLOG(("DSound: Restoring playback buffer\n"));
    else
        DSLOGREL(("DSound: Restoring playback buffer failed with %Rhrc\n", hr));

    return hr;
}


static HRESULT directSoundPlayUnlock(PDRVHOSTDSOUND pThis, LPDIRECTSOUNDBUFFER8 pDSB,
                                     PVOID pv1, PVOID pv2,
                                     DWORD cb1, DWORD cb2)
{
    RT_NOREF(pThis);
    HRESULT hr = IDirectSoundBuffer8_Unlock(pDSB, pv1, cb1, pv2, cb2);
    if (FAILED(hr))
        DSLOGREL(("DSound: Unlocking playback buffer failed with %Rhrc\n", hr));
    return hr;
}


static HRESULT directSoundPlayLock(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS,
                                   DWORD dwOffset, DWORD dwBytes,
                                   PVOID *ppv1, PVOID *ppv2,
                                   DWORD *pcb1, DWORD *pcb2,
                                   DWORD dwFlags)
{
    AssertReturn(dwBytes, VERR_INVALID_PARAMETER);

    HRESULT hr = E_FAIL;
    AssertCompile(DRV_DSOUND_RESTORE_ATTEMPTS_MAX > 0);
    for (unsigned i = 0; i < DRV_DSOUND_RESTORE_ATTEMPTS_MAX; i++)
    {
        PVOID pv1, pv2;
        DWORD cb1, cb2;
        hr = IDirectSoundBuffer8_Lock(pStreamDS->Out.pDSB, dwOffset, dwBytes, &pv1, &cb1, &pv2, &cb2, dwFlags);
        if (SUCCEEDED(hr))
        {
            if (   (!pv1 || !(cb1 & pStreamDS->uAlign))
                && (!pv2 || !(cb2 & pStreamDS->uAlign)))
            {
                if (ppv1)
                    *ppv1 = pv1;
                if (ppv2)
                    *ppv2 = pv2;
                if (pcb1)
                    *pcb1 = cb1;
                if (pcb2)
                    *pcb2 = cb2;
                return S_OK;
            }
            DSLOGREL(("DSound: Locking playback buffer returned misaligned buffer: cb1=%#RX32, cb2=%#RX32 (alignment: %#RX32)\n",
                      *pcb1, *pcb2, pStreamDS->uAlign));
            directSoundPlayUnlock(pThis, pStreamDS->Out.pDSB, pv1, pv2, cb1, cb2);
            return E_FAIL;
        }

        if (hr != DSERR_BUFFERLOST)
            break;

        LogFlowFunc(("Locking failed due to lost buffer, restoring ...\n"));
        directSoundPlayRestore(pThis, pStreamDS->Out.pDSB);
    }

    DSLOGREL(("DSound: Locking playback buffer failed with %Rhrc (dwOff=%ld, dwBytes=%ld)\n", hr, dwOffset, dwBytes));
    return hr;
}


static HRESULT directSoundCaptureUnlock(LPDIRECTSOUNDCAPTUREBUFFER8 pDSCB,
                                        PVOID pv1, PVOID pv2,
                                        DWORD cb1, DWORD cb2)
{
    HRESULT hr = IDirectSoundCaptureBuffer8_Unlock(pDSCB, pv1, cb1, pv2, cb2);
    if (FAILED(hr))
        DSLOGREL(("DSound: Unlocking capture buffer failed with %Rhrc\n", hr));
    return hr;
}


static HRESULT directSoundCaptureLock(PDSOUNDSTREAM pStreamDS,
                                      DWORD dwOffset, DWORD dwBytes,
                                      PVOID *ppv1, PVOID *ppv2,
                                      DWORD *pcb1, DWORD *pcb2,
                                      DWORD dwFlags)
{
    PVOID pv1 = NULL;
    PVOID pv2 = NULL;
    DWORD cb1 = 0;
    DWORD cb2 = 0;

    HRESULT hr = IDirectSoundCaptureBuffer8_Lock(pStreamDS->In.pDSCB, dwOffset, dwBytes,
                                                 &pv1, &cb1, &pv2, &cb2, dwFlags);
    if (FAILED(hr))
    {
        DSLOGREL(("DSound: Locking capture buffer failed with %Rhrc\n", hr));
        return hr;
    }

    if (   (pv1 && (cb1 & pStreamDS->uAlign))
        || (pv2 && (cb2 & pStreamDS->uAlign)))
    {
        DSLOGREL(("DSound: Locking capture buffer returned misaligned buffer: cb1=%RI32, cb2=%RI32 (alignment: %RU32)\n",
                  cb1, cb2, pStreamDS->uAlign));
        directSoundCaptureUnlock(pStreamDS->In.pDSCB, pv1, pv2, cb1, cb2);
        return E_FAIL;
    }

    *ppv1 = pv1;
    *ppv2 = pv2;
    *pcb1 = cb1;
    *pcb2 = cb2;

    return S_OK;
}


/*
 * DirectSound playback
 */

/**
 * Creates a DirectSound playback instance.
 *
 * @return  HRESULT
 * @param   pGUID   GUID of device to create the playback interface for. NULL
 *                  for the default device.
 * @param   ppDS    Where to return the interface to the created instance.
 */
static HRESULT drvHostDSoundCreateDSPlaybackInstance(LPCGUID pGUID, LPDIRECTSOUND8 *ppDS)
{
    LogFlowFuncEnter();

    LPDIRECTSOUND8 pDS = NULL;
    HRESULT        hrc = CoCreateInstance(CLSID_DirectSound8, NULL, CLSCTX_ALL, IID_IDirectSound8, (void **)&pDS);
    if (SUCCEEDED(hrc))
    {
        hrc = IDirectSound8_Initialize(pDS, pGUID);
        if (SUCCEEDED(hrc))
        {
            HWND hWnd = GetDesktopWindow();
            hrc = IDirectSound8_SetCooperativeLevel(pDS, hWnd, DSSCL_PRIORITY);
            if (SUCCEEDED(hrc))
            {
                *ppDS = pDS;
                LogFlowFunc(("LEAVE S_OK\n"));
                return S_OK;
            }
            LogRelMax(64, ("DSound: Setting cooperative level for (hWnd=%p) failed: %Rhrc\n", hWnd, hrc));
        }
        else if (hrc == DSERR_NODRIVER) /* Usually means that no playback devices are attached. */
            LogRelMax(64, ("DSound: DirectSound playback is currently unavailable\n"));
        else
            LogRelMax(64, ("DSound: DirectSound playback initialization failed: %Rhrc\n", hrc));

        IDirectSound8_Release(pDS);
    }
    else
        LogRelMax(64, ("DSound: Creating playback instance failed: %Rhrc\n", hrc));

    LogFlowFunc(("LEAVE %Rhrc\n", hrc));
    return hrc;
}


#if 0 /* not used */
static HRESULT directSoundPlayGetStatus(PDRVHOSTDSOUND pThis, LPDIRECTSOUNDBUFFER8 pDSB, DWORD *pdwStatus)
{
    AssertPtrReturn(pThis, E_POINTER);
    AssertPtrReturn(pDSB,  E_POINTER);

    AssertPtrNull(pdwStatus);

    DWORD dwStatus = 0;
    HRESULT hr = E_FAIL;
    for (unsigned i = 0; i < DRV_DSOUND_RESTORE_ATTEMPTS_MAX; i++)
    {
        hr = IDirectSoundBuffer8_GetStatus(pDSB, &dwStatus);
        if (   hr == DSERR_BUFFERLOST
            || (   SUCCEEDED(hr)
                && (dwStatus & DSBSTATUS_BUFFERLOST)))
        {
            LogFlowFunc(("Getting status failed due to lost buffer, restoring ...\n"));
            directSoundPlayRestore(pThis, pDSB);
        }
        else
            break;
    }

    if (SUCCEEDED(hr))
    {
        if (pdwStatus)
            *pdwStatus = dwStatus;
    }
    else
        DSLOGREL(("DSound: Retrieving playback status failed with %Rhrc\n", hr));

    return hr;
}
#endif


/*
 * DirectSoundCapture
 */

#if 0 /* unused */
static LPCGUID dsoundCaptureSelectDevice(PDRVHOSTDSOUND pThis, PPDMAUDIOSTREAMCFG pCfg)
{
    AssertPtrReturn(pThis, NULL);
    AssertPtrReturn(pCfg,  NULL);

    int rc = VINF_SUCCESS;

    LPCGUID pGUID = pThis->Cfg.pGuidCapture;
    if (!pGUID)
    {
        PDSOUNDDEV pDev = NULL;
        switch (pCfg->u.enmSrc)
        {
            case PDMAUDIORECSRC_LINE:
                /*
                 * At the moment we're only supporting line-in in the HDA emulation,
                 * and line-in + mic-in in the AC'97 emulation both are expected
                 * to use the host's mic-in as well.
                 *
                 * So the fall through here is intentional for now.
                 */
            case PDMAUDIORECSRC_MIC:
                pDev = (PDSOUNDDEV)DrvAudioHlpDeviceEnumGetDefaultDevice(&pThis->DeviceEnum, PDMAUDIODIR_IN);
                break;

            default:
                AssertFailedStmt(rc = VERR_NOT_SUPPORTED);
                break;
        }

        if (   RT_SUCCESS(rc)
            && pDev)
        {
            DSLOG(("DSound: Guest source '%s' is using host recording device '%s'\n",
                   PDMAudioRecSrcGetName(pCfg->u.enmSrc), pDev->Core.szName));
            pGUID = &pDev->Guid;
        }
        if (RT_FAILURE(rc))
        {
            LogRel(("DSound: Selecting recording device failed with %Rrc\n", rc));
            return NULL;
        }
    }

    /* This always has to be in the release log. */
    char *pszGUID = dsoundGUIDToUtf8StrA(pGUID);
    LogRel(("DSound: Guest source '%s' is using host recording device with GUID '%s'\n",
            PDMAudioRecSrcGetName(pCfg->u.enmSrc), pszGUID ? pszGUID: "{?}"));
    RTStrFree(pszGUID);

    return pGUID;
}
#endif


/**
 * Creates a DirectSound capture instance.
 *
 * @returns HRESULT
 * @param   pGUID   GUID of device to create the capture interface for.  NULL
 *                  for default.
 * @param   ppDSC   Where to return the interface to the created instance.
 */
static HRESULT drvHostDSoundCreateDSCaptureInstance(LPCGUID pGUID, LPDIRECTSOUNDCAPTURE8 *ppDSC)
{
    LogFlowFuncEnter();

    LPDIRECTSOUNDCAPTURE8 pDSC = NULL;
    HRESULT hrc = CoCreateInstance(CLSID_DirectSoundCapture8, NULL, CLSCTX_ALL, IID_IDirectSoundCapture8, (void **)&pDSC);
    if (SUCCEEDED(hrc))
    {
        hrc = IDirectSoundCapture_Initialize(pDSC, pGUID);
        if (SUCCEEDED(hrc))
        {
            *ppDSC = pDSC;
            LogFlowFunc(("LEAVE S_OK\n"));
            return S_OK;
        }
        if (hrc == DSERR_NODRIVER) /* Usually means that no capture devices are attached. */
            LogRelMax(64, ("DSound: Capture device currently is unavailable\n"));
        else
            LogRelMax(64, ("DSound: Initializing capturing device failed: %Rhrc\n", hrc));
        IDirectSoundCapture_Release(pDSC);
    }
    else
        LogRelMax(64, ("DSound: Creating capture instance failed: %Rhrc\n", hrc));

    LogFlowFunc(("LEAVE %Rhrc\n", hrc));
    return hrc;
}


/**
 * Updates this host driver's internal status, according to the global, overall input/output
 * state and all connected (native) audio streams.
 *
 * @todo r=bird: This is a 'ing waste of 'ing time!  We're doing this everytime
 *       an 'ing stream is created and we doesn't 'ing use the information here
 *       for any darn thing!  Given the reported slowness of enumeration and
 *       issues with the 'ing code the only appropriate response is:
 *       AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARG!!!!!!!
 *
 * @param   pThis               Host audio driver instance.
 */
static void dsoundUpdateStatusInternal(PDRVHOSTDSOUND pThis)
{
#if 0 /** @todo r=bird: This isn't doing *ANYTHING* useful. So, I've just disabled it.  */
    AssertPtrReturnVoid(pThis);
    LogFlowFuncEnter();

    PDMAudioHostEnumDelete(&pThis->DeviceEnum);
    int rc = dsoundDevicesEnumerate(pThis, &pThis->DeviceEnum);
    if (RT_SUCCESS(rc))
    {
#if 0
        if (   pThis->fEnabledOut != RT_BOOL(cbCtx.cDevOut)
            || pThis->fEnabledIn  != RT_BOOL(cbCtx.cDevIn))
        {
            /** @todo Use a registered callback to the audio connector (e.g "OnConfigurationChanged") to
             *        let the connector know that something has changed within the host backend. */
        }
#endif
        pThis->fEnabledIn  = PDMAudioHostEnumCountMatching(&pThis->DeviceEnum, PDMAUDIODIR_IN)  != 0;
        pThis->fEnabledOut = PDMAudioHostEnumCountMatching(&pThis->DeviceEnum, PDMAUDIODIR_OUT) != 0;
    }

    LogFlowFuncLeaveRC(rc);
#else
    RT_NOREF(pThis);
#endif
}


/*********************************************************************************************************************************
*   PDMIHOSTAUDIO                                                                                                                *
*********************************************************************************************************************************/

/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnGetConfig}
 */
static DECLCALLBACK(int) drvHostDSoundHA_GetConfig(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDCFG pBackendCfg)
{
    RT_NOREF(pInterface);
    AssertPtrReturn(pInterface,  VERR_INVALID_POINTER);
    AssertPtrReturn(pBackendCfg, VERR_INVALID_POINTER);


    /*
     * Fill in the config structure.
     */
    RTStrCopy(pBackendCfg->szName, sizeof(pBackendCfg->szName), "DirectSound");
    pBackendCfg->cbStream       = sizeof(DSOUNDSTREAM);
    pBackendCfg->fFlags         = 0;
    pBackendCfg->cMaxStreamsIn  = UINT32_MAX;
    pBackendCfg->cMaxStreamsOut = UINT32_MAX;

    return VINF_SUCCESS;
}


/**
 * Callback for the playback device enumeration.
 *
 * @return  TRUE if continuing enumeration, FALSE if not.
 * @param   pGUID               Pointer to GUID of enumerated device. Can be NULL.
 * @param   pwszDescription     Pointer to (friendly) description of enumerated device.
 * @param   pwszModule          Pointer to module name of enumerated device.
 * @param   lpContext           Pointer to PDSOUNDENUMCBCTX context for storing the enumerated information.
 *
 * @note    Carbon copy of drvHostDSoundEnumOldStyleCaptureCallback with OUT direction.
 */
static BOOL CALLBACK drvHostDSoundEnumOldStylePlaybackCallback(LPGUID pGUID, LPCWSTR pwszDescription,
                                                               LPCWSTR pwszModule, PVOID lpContext)
{
    PDSOUNDENUMCBCTX pEnumCtx = (PDSOUNDENUMCBCTX)lpContext;
    AssertPtrReturn(pEnumCtx, FALSE);

    PPDMAUDIOHOSTENUM pDevEnm = pEnumCtx->pDevEnm;
    AssertPtrReturn(pDevEnm, FALSE);

    AssertPtrNullReturn(pGUID, FALSE); /* pGUID can be NULL for default device(s). */
    AssertPtrReturn(pwszDescription, FALSE);
    RT_NOREF(pwszModule); /* Do not care about pwszModule. */

    int rc;
    PDSOUNDDEV pDev = (PDSOUNDDEV)PDMAudioHostDevAlloc(sizeof(DSOUNDDEV));
    if (pDev)
    {
        pDev->Core.enmUsage = PDMAUDIODIR_OUT;
        pDev->Core.enmType  = PDMAUDIODEVICETYPE_BUILTIN;

        if (pGUID == NULL)
            pDev->Core.fFlags = PDMAUDIOHOSTDEV_F_DEFAULT;

        char *pszName;
        rc = RTUtf16ToUtf8(pwszDescription, &pszName);
        if (RT_SUCCESS(rc))
        {
            RTStrCopy(pDev->Core.szName, sizeof(pDev->Core.szName), pszName);
            RTStrFree(pszName);

            if (pGUID) /* pGUID == NULL means default device. */
                memcpy(&pDev->Guid, pGUID, sizeof(pDev->Guid));

            PDMAudioHostEnumAppend(pDevEnm, &pDev->Core);

            /* Note: Querying the actual device information will be done at some
             *       later point in time outside this enumeration callback to prevent
             *       DSound hangs. */
            return TRUE;
        }
        PDMAudioHostDevFree(&pDev->Core);
    }
    else
        rc = VERR_NO_MEMORY;

    LogRel(("DSound: Error enumeration playback device '%ls': rc=%Rrc\n", pwszDescription, rc));
    return FALSE; /* Abort enumeration. */
}


/**
 * Callback for the capture device enumeration.
 *
 * @return  TRUE if continuing enumeration, FALSE if not.
 * @param   pGUID               Pointer to GUID of enumerated device. Can be NULL.
 * @param   pwszDescription     Pointer to (friendly) description of enumerated device.
 * @param   pwszModule          Pointer to module name of enumerated device.
 * @param   lpContext           Pointer to PDSOUNDENUMCBCTX context for storing the enumerated information.
 *
 * @note    Carbon copy of drvHostDSoundEnumOldStylePlaybackCallback with IN direction.
 */
static BOOL CALLBACK drvHostDSoundEnumOldStyleCaptureCallback(LPGUID pGUID, LPCWSTR pwszDescription,
                                                              LPCWSTR pwszModule, PVOID lpContext)
{
    PDSOUNDENUMCBCTX pEnumCtx = (PDSOUNDENUMCBCTX )lpContext;
    AssertPtrReturn(pEnumCtx, FALSE);

    PPDMAUDIOHOSTENUM pDevEnm = pEnumCtx->pDevEnm;
    AssertPtrReturn(pDevEnm, FALSE);

    AssertPtrNullReturn(pGUID, FALSE); /* pGUID can be NULL for default device(s). */
    AssertPtrReturn(pwszDescription, FALSE);
    RT_NOREF(pwszModule); /* Do not care about pwszModule. */

    int rc;
    PDSOUNDDEV pDev = (PDSOUNDDEV)PDMAudioHostDevAlloc(sizeof(DSOUNDDEV));
    if (pDev)
    {
        pDev->Core.enmUsage = PDMAUDIODIR_IN;
        pDev->Core.enmType  = PDMAUDIODEVICETYPE_BUILTIN;

        char *pszName;
        rc = RTUtf16ToUtf8(pwszDescription, &pszName);
        if (RT_SUCCESS(rc))
        {
            RTStrCopy(pDev->Core.szName, sizeof(pDev->Core.szName), pszName);
            RTStrFree(pszName);

            if (pGUID) /* pGUID == NULL means default capture device. */
                memcpy(&pDev->Guid, pGUID, sizeof(pDev->Guid));

            PDMAudioHostEnumAppend(pDevEnm, &pDev->Core);

            /* Note: Querying the actual device information will be done at some
             *       later point in time outside this enumeration callback to prevent
             *       DSound hangs. */
            return TRUE;
        }
        PDMAudioHostDevFree(&pDev->Core);
    }
    else
        rc = VERR_NO_MEMORY;

    LogRel(("DSound: Error enumeration capture device '%ls', rc=%Rrc\n", pwszDescription, rc));
    return FALSE; /* Abort enumeration. */
}


/**
 * Queries information for a given (DirectSound) device.
 *
 * @returns VBox status code.
 * @param   pDev                Audio device to query information for.
 */
static int drvHostDSoundEnumOldStyleQueryDeviceInfo(PDSOUNDDEV pDev)
{
    AssertPtr(pDev);
    int rc;

    if (pDev->Core.enmUsage == PDMAUDIODIR_OUT)
    {
        LPDIRECTSOUND8 pDS;
        HRESULT hr = drvHostDSoundCreateDSPlaybackInstance(&pDev->Guid, &pDS);
        if (SUCCEEDED(hr))
        {
            DSCAPS DSCaps;
            RT_ZERO(DSCaps);
            DSCaps.dwSize = sizeof(DSCAPS);
            hr = IDirectSound_GetCaps(pDS, &DSCaps);
            if (SUCCEEDED(hr))
            {
                pDev->Core.cMaxOutputChannels = DSCaps.dwFlags & DSCAPS_PRIMARYSTEREO ? 2 : 1;

                DWORD dwSpeakerCfg;
                hr = IDirectSound_GetSpeakerConfig(pDS, &dwSpeakerCfg);
                if (SUCCEEDED(hr))
                {
                    unsigned uSpeakerCount = 0;
                    switch (DSSPEAKER_CONFIG(dwSpeakerCfg))
                    {
                        case DSSPEAKER_MONO:             uSpeakerCount = 1; break;
                        case DSSPEAKER_HEADPHONE:        uSpeakerCount = 2; break;
                        case DSSPEAKER_STEREO:           uSpeakerCount = 2; break;
                        case DSSPEAKER_QUAD:             uSpeakerCount = 4; break;
                        case DSSPEAKER_SURROUND:         uSpeakerCount = 4; break;
                        case DSSPEAKER_5POINT1:          uSpeakerCount = 6; break;
                        case DSSPEAKER_5POINT1_SURROUND: uSpeakerCount = 6; break;
                        case DSSPEAKER_7POINT1:          uSpeakerCount = 8; break;
                        case DSSPEAKER_7POINT1_SURROUND: uSpeakerCount = 8; break;
                        default:                                            break;
                    }

                    if (uSpeakerCount) /* Do we need to update the channel count? */
                        pDev->Core.cMaxOutputChannels = uSpeakerCount;

                    rc = VINF_SUCCESS;
                }
                else
                {
                    LogRel(("DSound: Error retrieving playback device speaker config, hr=%Rhrc\n", hr));
                    rc = VERR_ACCESS_DENIED; /** @todo Fudge! */
                }
            }
            else
            {
                LogRel(("DSound: Error retrieving playback device capabilities, hr=%Rhrc\n", hr));
                rc = VERR_ACCESS_DENIED; /** @todo Fudge! */
            }

            IDirectSound8_Release(pDS);
        }
        else
            rc = VERR_GENERAL_FAILURE;
    }
    else if (pDev->Core.enmUsage == PDMAUDIODIR_IN)
    {
        LPDIRECTSOUNDCAPTURE8 pDSC;
        HRESULT hr = drvHostDSoundCreateDSCaptureInstance(&pDev->Guid, &pDSC);
        if (SUCCEEDED(hr))
        {
            DSCCAPS DSCCaps;
            RT_ZERO(DSCCaps);
            DSCCaps.dwSize = sizeof(DSCCAPS);
            hr = IDirectSoundCapture_GetCaps(pDSC, &DSCCaps);
            if (SUCCEEDED(hr))
            {
                pDev->Core.cMaxInputChannels = DSCCaps.dwChannels;
                rc = VINF_SUCCESS;
            }
            else
            {
                LogRel(("DSound: Error retrieving capture device capabilities, hr=%Rhrc\n", hr));
                rc = VERR_ACCESS_DENIED; /** @todo Fudge! */
            }

            IDirectSoundCapture_Release(pDSC);
        }
        else
            rc = VERR_GENERAL_FAILURE;
    }
    else
        AssertFailedStmt(rc = VERR_NOT_SUPPORTED);

    return rc;
}


/**
 * Queries information for @a pDevice and adds an entry to the enumeration.
 *
 * @returns VBox status code.
 * @param   pDevEnm     The enumeration to add the device to.
 * @param   pDevice     The device.
 * @param   enmType     The type of device.
 * @param   fDefault    Whether it's the default device.
 */
static int drvHostDSoundEnumNewStyleAdd(PPDMAUDIOHOSTENUM pDevEnm, IMMDevice *pDevice, EDataFlow enmType, bool fDefault)
{
    int rc = VINF_SUCCESS; /* ignore most errors */

    /*
     * Gather the necessary properties.
     */
    IPropertyStore *pProperties = NULL;
    HRESULT hrc = pDevice->OpenPropertyStore(STGM_READ, &pProperties);
    if (SUCCEEDED(hrc))
    {
        /* Get the friendly name. */
        PROPVARIANT VarName;
        PropVariantInit(&VarName);
        hrc = pProperties->GetValue(PKEY_Device_FriendlyName, &VarName);
        if (SUCCEEDED(hrc))
        {
            /* Get the DirectSound GUID. */
            PROPVARIANT VarGUID;
            PropVariantInit(&VarGUID);
            hrc = pProperties->GetValue(PKEY_AudioEndpoint_GUID, &VarGUID);
            if (SUCCEEDED(hrc))
            {
                /* Get the device format. */
                PROPVARIANT VarFormat;
                PropVariantInit(&VarFormat);
                hrc = pProperties->GetValue(PKEY_AudioEngine_DeviceFormat, &VarFormat);
                if (SUCCEEDED(hrc))
                {
                    WAVEFORMATEX const * const pFormat = (WAVEFORMATEX const *)VarFormat.blob.pBlobData;
                    AssertPtr(pFormat);

                    /*
                     * Create a enumeration entry for it.
                     */
                    PDSOUNDDEV pDev = (PDSOUNDDEV)PDMAudioHostDevAlloc(sizeof(DSOUNDDEV));
                    if (pDev)
                    {
                        pDev->Core.enmUsage = enmType == eRender ? PDMAUDIODIR_OUT : PDMAUDIODIR_IN;
                        pDev->Core.enmType  = PDMAUDIODEVICETYPE_BUILTIN;
                        if (enmType == eRender)
                            pDev->Core.cMaxOutputChannels = pFormat->nChannels;
                        else
                            pDev->Core.cMaxInputChannels  = pFormat->nChannels;

                        RT_NOREF(fDefault);
                        //if (fDefault)
                            hrc = UuidFromStringW(VarGUID.pwszVal, &pDev->Guid);
                        if (SUCCEEDED(hrc))
                        {
                            char *pszName;
                            rc = RTUtf16ToUtf8(VarName.pwszVal, &pszName);
                            if (RT_SUCCESS(rc))
                            {
                                RTStrCopy(pDev->Core.szName, sizeof(pDev->Core.szName), pszName);
                                RTStrFree(pszName);

                                PDMAudioHostEnumAppend(pDevEnm, &pDev->Core);
                            }
                            else
                                PDMAudioHostDevFree(&pDev->Core);
                        }
                        else
                        {
                            LogFunc(("UuidFromStringW(%ls): %Rhrc\n", VarGUID.pwszVal, hrc));
                            PDMAudioHostDevFree(&pDev->Core);
                        }
                    }
                    else
                        rc = VERR_NO_MEMORY;
                    PropVariantClear(&VarFormat);
                }
                else
                    LogFunc(("Failed to get PKEY_AudioEngine_DeviceFormat: %Rhrc\n", hrc));
                PropVariantClear(&VarGUID);
            }
            else
                LogFunc(("Failed to get PKEY_AudioEndpoint_GUID: %Rhrc\n", hrc));
            PropVariantClear(&VarName);
        }
        else
            LogFunc(("Failed to get PKEY_Device_FriendlyName: %Rhrc\n", hrc));
        pProperties->Release();
    }
    else
        LogFunc(("OpenPropertyStore failed: %Rhrc\n", hrc));

    if (hrc == E_OUTOFMEMORY && RT_SUCCESS_NP(rc))
        rc = VERR_NO_MEMORY;
    return rc;
}


/**
 * Does a (Re-)enumeration of the host's playback + capturing devices.
 *
 * @return  VBox status code.
 * @param   pDevEnm             Where to store the enumerated devices.
 */
static int drvHostDSoundEnumerateDevices(PPDMAUDIOHOSTENUM pDevEnm)
{
    DSLOG(("DSound: Enumerating devices ...\n"));

    /*
     * Use the Vista+ API.
     */
    IMMDeviceEnumerator *pEnumerator;
    HRESULT hrc = CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void **)&pEnumerator);
    if (SUCCEEDED(hrc))
    {
        int rc = VINF_SUCCESS;
        for (unsigned idxPass = 0; idxPass < 2 && RT_SUCCESS(rc); idxPass++)
        {
            EDataFlow const enmType = idxPass == 0 ? EDataFlow::eRender : EDataFlow::eCapture;

            /* Get the default device first. */
            IMMDevice *pDefaultDevice = NULL;
            hrc = pEnumerator->GetDefaultAudioEndpoint(enmType, eMultimedia, &pDefaultDevice);
            if (SUCCEEDED(hrc))
                rc = drvHostDSoundEnumNewStyleAdd(pDevEnm, pDefaultDevice, enmType, true);
            else
                pDefaultDevice = NULL;

            /* Enumerate the devices. */
            IMMDeviceCollection *pCollection = NULL;
            hrc = pEnumerator->EnumAudioEndpoints(enmType, DEVICE_STATE_ACTIVE /*| DEVICE_STATE_UNPLUGGED?*/, &pCollection);
            if (SUCCEEDED(hrc) && pCollection != NULL)
            {
                UINT cDevices = 0;
                hrc = pCollection->GetCount(&cDevices);
                if (SUCCEEDED(hrc))
                {
                    for (UINT idxDevice = 0; idxDevice < cDevices && RT_SUCCESS(rc); idxDevice++)
                    {
                        IMMDevice *pDevice = NULL;
                        hrc = pCollection->Item(idxDevice, &pDevice);
                        if (SUCCEEDED(hrc) && pDevice)
                        {
                            if (pDevice != pDefaultDevice)
                                rc = drvHostDSoundEnumNewStyleAdd(pDevEnm, pDevice, enmType, false);
                            pDevice->Release();
                        }
                    }
                }
                pCollection->Release();
            }
            else
                LogRelMax(10, ("EnumAudioEndpoints(%s) failed: %Rhrc\n", idxPass == 0 ? "output" : "input", hrc));

            if (pDefaultDevice)
                pDefaultDevice->Release();
        }
        pEnumerator->Release();
        if (pDevEnm->cDevices > 0 || RT_FAILURE(rc))
        {
            DSLOG(("DSound: Enumerating devices done - %u device (%Rrc)\n", pDevEnm->cDevices, rc));
            return rc;
        }
    }

    /*
     * Fall back to dsound.
     */
    /* Resolve symbols once. */
    static PFNDIRECTSOUNDENUMERATEW        volatile s_pfnDirectSoundEnumerateW        = NULL;
    static PFNDIRECTSOUNDCAPTUREENUMERATEW volatile s_pfnDirectSoundCaptureEnumerateW = NULL;

    PFNDIRECTSOUNDENUMERATEW        pfnDirectSoundEnumerateW          = s_pfnDirectSoundEnumerateW;
    PFNDIRECTSOUNDCAPTUREENUMERATEW pfnDirectSoundCaptureEnumerateW   = s_pfnDirectSoundCaptureEnumerateW;
    if (!pfnDirectSoundEnumerateW || !pfnDirectSoundCaptureEnumerateW)
    {
        RTLDRMOD hModDSound = NIL_RTLDRMOD;
        int rc = RTLdrLoadSystem("dsound.dll", true /*fNoUnload*/, &hModDSound);
        if (RT_SUCCESS(rc))
        {
            rc = RTLdrGetSymbol(hModDSound, "DirectSoundEnumerateW", (void **)&pfnDirectSoundEnumerateW);
            if (RT_SUCCESS(rc))
                s_pfnDirectSoundEnumerateW = pfnDirectSoundEnumerateW;
            else
                LogRel(("DSound: Failed to get dsound.dll export DirectSoundEnumerateW: %Rrc\n", rc));

            rc = RTLdrGetSymbol(hModDSound, "DirectSoundCaptureEnumerateW", (void **)&pfnDirectSoundCaptureEnumerateW);
            if (RT_SUCCESS(rc))
                s_pfnDirectSoundCaptureEnumerateW = pfnDirectSoundCaptureEnumerateW;
            else
                LogRel(("DSound: Failed to get dsound.dll export DirectSoundCaptureEnumerateW: %Rrc\n", rc));
            RTLdrClose(hModDSound);
        }
        else
            LogRel(("DSound: Unable to load dsound.dll for enumerating devices: %Rrc\n", rc));
        if (!pfnDirectSoundEnumerateW && !pfnDirectSoundCaptureEnumerateW)
            return rc;
    }

    /* Common callback context for both playback and capture enumerations: */
    DSOUNDENUMCBCTX EnumCtx;
    EnumCtx.fFlags  = 0;
    EnumCtx.pDevEnm = pDevEnm;

    /* Enumerate playback devices. */
    if (pfnDirectSoundEnumerateW)
    {
        DSLOG(("DSound: Enumerating playback devices ...\n"));
        HRESULT hr = pfnDirectSoundEnumerateW(&drvHostDSoundEnumOldStylePlaybackCallback, &EnumCtx);
        if (FAILED(hr))
            LogRel(("DSound: Error enumerating host playback devices: %Rhrc\n", hr));
    }

    /* Enumerate capture devices. */
    if (pfnDirectSoundCaptureEnumerateW)
    {
        DSLOG(("DSound: Enumerating capture devices ...\n"));
        HRESULT hr = pfnDirectSoundCaptureEnumerateW(&drvHostDSoundEnumOldStyleCaptureCallback, &EnumCtx);
        if (FAILED(hr))
            LogRel(("DSound: Error enumerating host capture devices: %Rhrc\n", hr));
    }

    /*
     * Query Information for all enumerated devices.
     * Note! This is problematic to do from the enumeration callbacks.
     */
    PDSOUNDDEV pDev;
    RTListForEach(&pDevEnm->LstDevices, pDev, DSOUNDDEV, Core.ListEntry)
    {
        drvHostDSoundEnumOldStyleQueryDeviceInfo(pDev); /* ignore rc */
    }

    DSLOG(("DSound: Enumerating devices done\n"));

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnGetDevices}
 */
static DECLCALLBACK(int) drvHostDSoundHA_GetDevices(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHOSTENUM pDeviceEnum)
{
    RT_NOREF(pInterface);
    AssertPtrReturn(pDeviceEnum, VERR_INVALID_POINTER);

    PDMAudioHostEnumInit(pDeviceEnum);
    int rc = drvHostDSoundEnumerateDevices(pDeviceEnum);
    if (RT_FAILURE(rc))
        PDMAudioHostEnumDelete(pDeviceEnum);

    LogFlowFunc(("Returning %Rrc\n", rc));
    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnGetStatus}
 */
static DECLCALLBACK(PDMAUDIOBACKENDSTS) drvHostDSoundHA_GetStatus(PPDMIHOSTAUDIO pInterface, PDMAUDIODIR enmDir)
{
    RT_NOREF(pInterface, enmDir);
    return PDMAUDIOBACKENDSTS_RUNNING;
}


/**
 * Converts from PDM stream config to windows WAVEFORMATEX struct.
 *
 * @param   pCfg    The PDM audio stream config to convert from.
 * @param   pFmt    The windows structure to initialize.
 */
static void dsoundWaveFmtFromCfg(PCPDMAUDIOSTREAMCFG pCfg, PWAVEFORMATEX pFmt)
{
    RT_ZERO(*pFmt);
    pFmt->wFormatTag      = WAVE_FORMAT_PCM;
    pFmt->nChannels       = PDMAudioPropsChannels(&pCfg->Props);
    pFmt->wBitsPerSample  = PDMAudioPropsSampleBits(&pCfg->Props);
    pFmt->nSamplesPerSec  = PDMAudioPropsHz(&pCfg->Props);
    pFmt->nBlockAlign     = PDMAudioPropsFrameSize(&pCfg->Props);
    pFmt->nAvgBytesPerSec = PDMAudioPropsFramesToBytes(&pCfg->Props, PDMAudioPropsHz(&pCfg->Props));
    pFmt->cbSize          = 0; /* No extra data specified. */
}


/**
 * Resets the state of a DirectSound stream, clearing the buffer content.
 *
 * @param   pThis               Host audio driver instance.
 * @param   pStreamDS           Stream to reset state for.
 */
static void drvHostDSoundStreamReset(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    RT_NOREF(pThis);
    LogFunc(("Resetting %s\n", pStreamDS->Cfg.enmDir == PDMAUDIODIR_IN ? "capture" : "playback"));

    if (pStreamDS->Cfg.enmDir == PDMAUDIODIR_IN)
    {
        /*
         * Input streams.
         */
        LogFunc(("Resetting capture stream '%s'\n", pStreamDS->Cfg.szName));

        /* Reset the state: */
/** @todo r=bird: We set the read position to zero here, but shouldn't we query it
 * from the buffer instead given that there isn't any interface for repositioning
 * to the start of the buffer as with playback buffers? */
        pStreamDS->In.offReadPos = 0;
        pStreamDS->In.cOverruns  = 0;

        /* Clear the buffer content: */
        AssertPtr(pStreamDS->In.pDSCB);
        if (pStreamDS->In.pDSCB)
        {
            PVOID   pv1 = NULL;
            DWORD   cb1 = 0;
            PVOID   pv2 = NULL;
            DWORD   cb2 = 0;
            HRESULT hrc = IDirectSoundCaptureBuffer8_Lock(pStreamDS->In.pDSCB, 0, pStreamDS->cbBufSize,
                                                          &pv1, &cb1, &pv2, &cb2, 0 /*fFlags*/);
            if (SUCCEEDED(hrc))
            {
                PDMAudioPropsClearBuffer(&pStreamDS->Cfg.Props, pv1, cb1, PDMAUDIOPCMPROPS_B2F(&pStreamDS->Cfg.Props, cb1));
                if (pv2 && cb2)
                    PDMAudioPropsClearBuffer(&pStreamDS->Cfg.Props, pv2, cb2, PDMAUDIOPCMPROPS_B2F(&pStreamDS->Cfg.Props, cb2));
                hrc = IDirectSoundCaptureBuffer8_Unlock(pStreamDS->In.pDSCB, pv1, cb1, pv2, cb2);
                if (FAILED(hrc))
                    LogRelMaxFunc(64, ("DSound: Unlocking capture buffer '%s' after reset failed: %Rhrc\n",
                                       pStreamDS->Cfg.szName, hrc));
            }
            else
                LogRelMaxFunc(64, ("DSound: Locking capture buffer '%s' for reset failed: %Rhrc\n",
                                   pStreamDS->Cfg.szName, hrc));
        }
    }
    else
    {
        /*
         * Output streams.
         */
        Assert(pStreamDS->Cfg.enmDir == PDMAUDIODIR_OUT);
        LogFunc(("Resetting playback stream '%s'\n", pStreamDS->Cfg.szName));

        /* If draining was enagaged, make sure dsound has stopped playing: */
        if (pStreamDS->Out.fDrain && pStreamDS->Out.pDSB)
            pStreamDS->Out.pDSB->Stop();

        /* Reset the internal state: */
        pStreamDS->Out.fFirstTransfer           = true;
        pStreamDS->Out.fDrain                   = false;
        pStreamDS->Out.cUnderruns               = 0;
        pStreamDS->Out.cbLastTransferred        = 0;
        pStreamDS->Out.tsLastTransferredMs      = 0;
        pStreamDS->Out.cbTransferred            = 0;
        pStreamDS->Out.cbWritten                = 0;
        pStreamDS->Out.offWritePos              = 0;
        pStreamDS->Out.offPlayCursorLastPending = 0;
        pStreamDS->Out.offPlayCursorLastPlayed  = 0;

        /* Reset the buffer content and repositioning the buffer to the start of the buffer. */
        AssertPtr(pStreamDS->Out.pDSB);
        if (pStreamDS->Out.pDSB)
        {
            HRESULT hrc = IDirectSoundBuffer8_SetCurrentPosition(pStreamDS->Out.pDSB, 0);
            if (FAILED(hrc))
                LogRelMaxFunc(64, ("DSound: Failed to set buffer position for '%s': %Rhrc\n", pStreamDS->Cfg.szName, hrc));

            PVOID   pv1 = NULL;
            DWORD   cb1 = 0;
            PVOID   pv2 = NULL;
            DWORD   cb2 = 0;
            hrc = IDirectSoundBuffer8_Lock(pStreamDS->Out.pDSB, 0, pStreamDS->cbBufSize, &pv1, &cb1, &pv2, &cb2, 0 /*fFlags*/);
            if (hrc == DSERR_BUFFERLOST)
            {
                directSoundPlayRestore(pThis, pStreamDS->Out.pDSB);
                hrc = IDirectSoundBuffer8_Lock(pStreamDS->Out.pDSB, 0, pStreamDS->cbBufSize, &pv1, &cb1, &pv2, &cb2, 0 /*fFlags*/);
            }
            if (SUCCEEDED(hrc))
            {
                PDMAudioPropsClearBuffer(&pStreamDS->Cfg.Props, pv1, cb1, PDMAUDIOPCMPROPS_B2F(&pStreamDS->Cfg.Props, cb1));
                if (pv2 && cb2)
                    PDMAudioPropsClearBuffer(&pStreamDS->Cfg.Props, pv2, cb2, PDMAUDIOPCMPROPS_B2F(&pStreamDS->Cfg.Props, cb2));

                hrc = IDirectSoundBuffer8_Unlock(pStreamDS->Out.pDSB, pv1, cb1, pv2, cb2);
                if (FAILED(hrc))
                    LogRelMaxFunc(64, ("DSound: Unlocking playback buffer '%s' after reset failed: %Rhrc\n",
                                       pStreamDS->Cfg.szName, hrc));
            }
            else
                LogRelMaxFunc(64, ("DSound: Locking playback buffer '%s' for reset failed: %Rhrc\n", pStreamDS->Cfg.szName, hrc));
        }
    }

#ifdef LOG_ENABLED
    pStreamDS->Dbg.tsLastTransferredMs = 0;
#endif
}


/**
 * Worker for drvHostDSoundHA_StreamCreate that creates caputre stream.
 *
 * @returns Windows COM status code.
 * @param   pThis       The DSound instance data.
 * @param   pStreamDS   The stream instance data.
 * @param   pCfgReq     The requested stream config (input).
 * @param   pCfgAcq     Where to return the actual stream config.  This is a
 *                      copy of @a *pCfgReq when called.
 * @param   pWaveFmtX   On input the requested stream format.
 *                      Updated to the actual stream format on successful
 *                      return.
 */
static HRESULT drvHostDSoundStreamCreateCapture(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS,
                                                PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq, WAVEFORMATEX *pWaveFmtX)
{
    Assert(pStreamDS->In.pDSCB == NULL);
    HRESULT hrc;

    /*
     * Create, initialize and set up a IDirectSoundCapture instance the first time
     * we go thru here.
     */
    /** @todo bird: Or should we rather just throw this away after we've gotten the
     *        capture buffer?  Old code would just leak it... */
    if (pThis->pDSC == NULL)
    {
        hrc = drvHostDSoundCreateDSCaptureInstance(pThis->Cfg.pGuidCapture, &pThis->pDSC);
        if (FAILED(hrc))
            return hrc; /* The worker has complained to the release log already. */
    }

    /*
     * Create the capture buffer.
     */
    DSCBUFFERDESC BufferDesc =
    {
        /*.dwSize = */          sizeof(BufferDesc),
        /*.dwFlags = */         0,
        /*.dwBufferBytes =*/    PDMAudioPropsFramesToBytes(&pCfgReq->Props, pCfgReq->Backend.cFramesBufferSize),
        /*.dwReserved = */      0,
        /*.lpwfxFormat = */     pWaveFmtX,
        /*.dwFXCount = */       0,
        /*.lpDSCFXDesc = */     NULL
    };

    LogRel2(("DSound: Requested capture buffer is %#x B / %u B / %RU64 ms\n", BufferDesc.dwBufferBytes, BufferDesc.dwBufferBytes,
             PDMAudioPropsBytesToMilli(&pCfgReq->Props, BufferDesc.dwBufferBytes)));

    LPDIRECTSOUNDCAPTUREBUFFER pLegacyDSCB = NULL;
    hrc = IDirectSoundCapture_CreateCaptureBuffer(pThis->pDSC, &BufferDesc, &pLegacyDSCB, NULL);
    if (FAILED(hrc))
    {
        LogRelMax(64, ("DSound: Creating capture buffer for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));
        return hrc;
    }

    /* Get the IDirectSoundCaptureBuffer8 version of the interface. */
    hrc = IDirectSoundCaptureBuffer_QueryInterface(pLegacyDSCB, IID_IDirectSoundCaptureBuffer8, (void **)&pStreamDS->In.pDSCB);
    IDirectSoundCaptureBuffer_Release(pLegacyDSCB);
    if (FAILED(hrc))
    {
        LogRelMax(64, ("DSound: Querying IID_IDirectSoundCaptureBuffer8 for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));
        return hrc;
    }

    /*
     * Query the actual stream configuration.
     */
#if 0 /** @todo r=bird: WTF was this for? */
    DWORD offByteReadPos = 0;
    hrc = IDirectSoundCaptureBuffer8_GetCurrentPosition(pStreamDS->In.pDSCB, NULL, &offByteReadPos);
    if (FAILED(hrc))
    {
        offByteReadPos = 0;
        DSLOGREL(("DSound: Getting capture position failed with %Rhrc\n", hr));
    }
#endif
    RT_ZERO(*pWaveFmtX);
    hrc = IDirectSoundCaptureBuffer8_GetFormat(pStreamDS->In.pDSCB, pWaveFmtX, sizeof(*pWaveFmtX), NULL);
    if (SUCCEEDED(hrc))
    {
        /** @todo r=bird: We aren't converting/checking the pWaveFmtX content...   */

        DSCBCAPS BufferCaps = { /*.dwSize = */ sizeof(BufferCaps), 0, 0, 0 };
        hrc = IDirectSoundCaptureBuffer8_GetCaps(pStreamDS->In.pDSCB, &BufferCaps);
        if (SUCCEEDED(hrc))
        {
            LogRel2(("DSound: Acquired capture buffer capabilities for '%s':\n"
                     "DSound:   dwFlags       = %#RX32\n"
                     "DSound:   dwBufferBytes = %#RX32 B / %RU32 B / %RU64 ms\n"
                     "DSound:   dwReserved    = %#RX32\n",
                     pCfgReq->szName, BufferCaps.dwFlags, BufferCaps.dwBufferBytes, BufferCaps.dwBufferBytes,
                     PDMAudioPropsBytesToMilli(&pCfgReq->Props, BufferCaps.dwBufferBytes), BufferCaps.dwReserved ));

            /* Update buffer related stuff: */
            pStreamDS->In.offReadPos = 0; /** @todo shouldn't we use offBytReadPos here to "read at the initial capture position"? */
            pStreamDS->cbBufSize     = BufferCaps.dwBufferBytes;
            pCfgAcq->Backend.cFramesBufferSize = PDMAudioPropsBytesToFrames(&pCfgAcq->Props, BufferCaps.dwBufferBytes);

            /** @todo r=bird: WTF is this for? */
            pThis->pDSStrmIn = pStreamDS;

#if 0 /** @todo r=bird: uAlign isn't set anywhere, so this hasn't been checking anything for a while... */
            if (bc.dwBufferBytes & pStreamDS->uAlign)
                DSLOGREL(("DSound: Capture GetCaps returned misaligned buffer: size %RU32, alignment %RU32\n",
                          bc.dwBufferBytes, pStreamDS->uAlign + 1));
#endif
            LogFlow(("returns S_OK\n"));
            return S_OK;
        }
        LogRelMax(64, ("DSound: Getting capture buffer capabilities for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));
    }
    else
        LogRelMax(64, ("DSound: Getting capture format for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));

    IDirectSoundCaptureBuffer8_Release(pStreamDS->In.pDSCB);
    pStreamDS->In.pDSCB = NULL;
    LogFlowFunc(("returns %Rhrc\n", hrc));
    return hrc;
}


/**
 * Worker for drvHostDSoundHA_StreamCreate that creates playback stream.
 *
 * @returns Windows COM status code.
 * @param   pThis       The DSound instance data.
 * @param   pStreamDS   The stream instance data.
 * @param   pCfgReq     The requested stream config (input).
 * @param   pCfgAcq     Where to return the actual stream config.  This is a
 *                      copy of @a *pCfgReq when called.
 * @param   pWaveFmtX   On input the requested stream format.
 *                      Updated to the actual stream format on successful
 *                      return.
 */
static HRESULT drvHostDSoundStreamCreatePlayback(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS,
                                                 PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq, WAVEFORMATEX *pWaveFmtX)
{
    Assert(pStreamDS->Out.pDSB == NULL);
    HRESULT hrc;

    /*
     * Create, initialize and set up a DirectSound8 instance the first time
     * we go thru here.
     */
    /** @todo bird: Or should we rather just throw this away after we've gotten the
     *        sound buffer?  Old code would just leak it... */
    if (pThis->pDS == NULL)
    {
        hrc = drvHostDSoundCreateDSPlaybackInstance(pThis->Cfg.pGuidPlay, &pThis->pDS);
        if (FAILED(hrc))
            return hrc; /* The worker has complained to the release log already. */
    }

    /*
     * As we reuse our (secondary) buffer for playing out data as it comes in,
     * we're using this buffer as a so-called streaming buffer.
     *
     * See https://msdn.microsoft.com/en-us/library/windows/desktop/ee419014(v=vs.85).aspx
     *
     * However, as we do not want to use memory on the sound device directly
     * (as most modern audio hardware on the host doesn't have this anyway),
     * we're *not* going to use DSBCAPS_STATIC for that.
     *
     * Instead we're specifying DSBCAPS_LOCSOFTWARE, as this fits the bill
     * of copying own buffer data to our secondary's Direct Sound buffer.
     */
    DSBUFFERDESC BufferDesc =
    {
        /*.dwSize = */          sizeof(BufferDesc),
        /*.dwFlags = */         DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_LOCSOFTWARE,
        /*.dwBufferBytes = */   PDMAudioPropsFramesToBytes(&pCfgReq->Props, pCfgReq->Backend.cFramesBufferSize),
        /*.dwReserved = */      0,
        /*.lpwfxFormat = */     pWaveFmtX
        /*.guid3DAlgorithm =    {0, 0, 0, {0,0,0,0, 0,0,0,0}} */
    };
    LogRel2(("DSound: Requested playback buffer is %#x B / %u B / %RU64 ms\n", BufferDesc.dwBufferBytes, BufferDesc.dwBufferBytes,
             PDMAudioPropsBytesToMilli(&pCfgReq->Props, BufferDesc.dwBufferBytes)));

    LPDIRECTSOUNDBUFFER pLegacyDSB = NULL;
    hrc = IDirectSound8_CreateSoundBuffer(pThis->pDS, &BufferDesc, &pLegacyDSB, NULL);
    if (FAILED(hrc))
    {
        LogRelMax(64, ("DSound: Creating playback sound buffer for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));
        return hrc;
    }

    /* Get the IDirectSoundBuffer8 version of the interface. */
    hrc = IDirectSoundBuffer_QueryInterface(pLegacyDSB, IID_IDirectSoundBuffer8, (PVOID *)&pStreamDS->Out.pDSB);
    IDirectSoundBuffer_Release(pLegacyDSB);
    if (FAILED(hrc))
    {
        LogRelMax(64, ("DSound: Querying IID_IDirectSoundBuffer8 for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));
        return hrc;
    }

    /*
     * Query the actual stream parameters, they may differ from what we requested.
     */
    RT_ZERO(*pWaveFmtX);
    hrc = IDirectSoundBuffer8_GetFormat(pStreamDS->Out.pDSB, pWaveFmtX, sizeof(*pWaveFmtX), NULL);
    if (SUCCEEDED(hrc))
    {
        /** @todo r=bird: We aren't converting/checking the pWaveFmtX content...   */

        DSBCAPS BufferCaps = { /*.dwSize = */ sizeof(BufferCaps), 0, 0, 0, 0 };
        hrc = IDirectSoundBuffer8_GetCaps(pStreamDS->Out.pDSB, &BufferCaps);
        if (SUCCEEDED(hrc))
        {
            LogRel2(("DSound: Acquired playback buffer capabilities for '%s':\n"
                     "DSound:   dwFlags              = %#RX32\n"
                     "DSound:   dwBufferBytes        = %#RX32 B / %RU32 B / %RU64 ms\n"
                     "DSound:   dwUnlockTransferRate = %RU32 KB/s\n"
                     "DSound:   dwPlayCpuOverhead    = %RU32%%\n",
                     pCfgReq->szName, BufferCaps.dwFlags, BufferCaps.dwBufferBytes, BufferCaps.dwBufferBytes,
                     PDMAudioPropsBytesToMilli(&pCfgReq->Props, BufferCaps.dwBufferBytes),
                     BufferCaps.dwUnlockTransferRate, BufferCaps.dwPlayCpuOverhead));

            /* Update buffer related stuff: */
            pStreamDS->cbBufSize = BufferCaps.dwBufferBytes;
            pCfgAcq->Backend.cFramesBufferSize    = PDMAudioPropsBytesToFrames(&pCfgAcq->Props, BufferCaps.dwBufferBytes);
            pCfgAcq->Backend.cFramesPeriod        = pCfgAcq->Backend.cFramesBufferSize / 4; /* total fiction */
            pCfgAcq->Backend.cFramesPreBuffering  = pCfgReq->Backend.cFramesPreBuffering * pCfgAcq->Backend.cFramesBufferSize
                                                  / RT_MAX(pCfgReq->Backend.cFramesBufferSize, 1);

            /** @todo r=bird: WTF is this for? */
            pThis->pDSStrmOut = pStreamDS;

#if 0 /** @todo r=bird: uAlign isn't set anywhere, so this hasn't been checking anything for a while... */
            if (bc.dwBufferBytes & pStreamDS->uAlign)
                DSLOGREL(("DSound: Playback capabilities returned misaligned buffer: size %RU32, alignment %RU32\n",
                          bc.dwBufferBytes, pStreamDS->uAlign + 1));
#endif
            LogFlow(("returns S_OK\n"));
            return S_OK;
        }
        LogRelMax(64, ("DSound: Getting playback buffer capabilities for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));
    }
    else
        LogRelMax(64, ("DSound: Getting playback format for '%s' failed: %Rhrc\n", pCfgReq->szName, hrc));

    IDirectSoundBuffer8_Release(pStreamDS->Out.pDSB);
    pStreamDS->Out.pDSB = NULL;
    LogFlowFunc(("returns %Rhrc\n", hrc));
    return hrc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamCreate}
 */
static DECLCALLBACK(int) drvHostDSoundHA_StreamCreate(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream,
                                                      PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq)
{
    PDRVHOSTDSOUND pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio);
    PDSOUNDSTREAM  pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgReq, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfgAcq, VERR_INVALID_POINTER);
    AssertReturn(pCfgReq->enmDir == PDMAUDIODIR_IN || pCfgReq->enmDir == PDMAUDIODIR_OUT, VERR_INVALID_PARAMETER);
    Assert(PDMAudioStrmCfgEquals(pCfgReq, pCfgAcq));

    const char * const pszStreamType = pCfgReq->enmDir == PDMAUDIODIR_IN ? "capture" : "playback"; RT_NOREF(pszStreamType);
    LogFlowFunc(("enmSrc/Dst=%s '%s'\n",
                 pCfgReq->enmDir == PDMAUDIODIR_IN ? PDMAudioRecSrcGetName(pCfgReq->u.enmSrc)
                 : PDMAudioPlaybackDstGetName(pCfgReq->u.enmDst), pCfgReq->szName));

    /* For whatever reason: */
    dsoundUpdateStatusInternal(pThis);

    /*
     * DSound has different COM interfaces for working with input and output
     * streams, so we'll quickly part ways here after some common format
     * specification setup and logging.
     */
#if defined(RTLOG_REL_ENABLED) || defined(LOG_ENABLED)
    char szTmp[64];
#endif
    LogRel2(("DSound: Opening %s stream '%s' (%s)\n", pCfgReq->szName, pszStreamType,
             PDMAudioPropsToString(&pCfgReq->Props, szTmp, sizeof(szTmp))));

    WAVEFORMATEX WaveFmtX;
    dsoundWaveFmtFromCfg(pCfgReq, &WaveFmtX);
    LogRel2(("DSound: Requested %s format for '%s':\n"
             "DSound:   wFormatTag      = %RU16\n"
             "DSound:   nChannels       = %RU16\n"
             "DSound:   nSamplesPerSec  = %RU32\n"
             "DSound:   nAvgBytesPerSec = %RU32\n"
             "DSound:   nBlockAlign     = %RU16\n"
             "DSound:   wBitsPerSample  = %RU16\n"
             "DSound:   cbSize          = %RU16\n",
             pszStreamType, pCfgReq->szName, WaveFmtX.wFormatTag, WaveFmtX.nChannels, WaveFmtX.nSamplesPerSec,
             WaveFmtX.nAvgBytesPerSec, WaveFmtX.nBlockAlign, WaveFmtX.wBitsPerSample, WaveFmtX.cbSize));

    HRESULT hrc;
    if (pCfgReq->enmDir == PDMAUDIODIR_IN)
        hrc = drvHostDSoundStreamCreateCapture(pThis, pStreamDS, pCfgReq, pCfgAcq, &WaveFmtX);
    else
        hrc = drvHostDSoundStreamCreatePlayback(pThis, pStreamDS, pCfgReq, pCfgAcq, &WaveFmtX);
    int rc;
    if (SUCCEEDED(hrc))
    {
        LogRel2(("DSound: Acquired %s format for '%s':\n"
                 "DSound:   wFormatTag      = %RU16\n"
                 "DSound:   nChannels       = %RU16\n"
                 "DSound:   nSamplesPerSec  = %RU32\n"
                 "DSound:   nAvgBytesPerSec = %RU32\n"
                 "DSound:   nBlockAlign     = %RU16\n"
                 "DSound:   wBitsPerSample  = %RU16\n"
                 "DSound:   cbSize          = %RU16\n",
                 pszStreamType, pCfgReq->szName, WaveFmtX.wFormatTag, WaveFmtX.nChannels, WaveFmtX.nSamplesPerSec,
                 WaveFmtX.nAvgBytesPerSec, WaveFmtX.nBlockAlign, WaveFmtX.wBitsPerSample, WaveFmtX.cbSize));

        /*
         * Copy the acquired config and reset the stream (clears the buffer).
         */
        PDMAudioStrmCfgCopy(&pStreamDS->Cfg, pCfgAcq);
        drvHostDSoundStreamReset(pThis, pStreamDS);
        rc = VINF_SUCCESS;
    }
    else
        rc = VERR_AUDIO_STREAM_COULD_NOT_CREATE;

    LogFlowFunc(("returns %Rrc\n", rc));
    return rc;
}


static HRESULT directSoundPlayClose(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    AssertPtrReturn(pThis,     E_POINTER);
    AssertPtrReturn(pStreamDS, E_POINTER);

    LogFlowFuncEnter();

    HRESULT hr = directSoundPlayStop(pThis, pStreamDS, true /* fFlush */);
    if (SUCCEEDED(hr))
    {
        DSLOG(("DSound: Closing playback stream\n"));
        RTCritSectEnter(&pThis->CritSect);

        if (pStreamDS->Out.pDSB)
        {
            IDirectSoundBuffer8_Release(pStreamDS->Out.pDSB);
            pStreamDS->Out.pDSB = NULL;
        }

        pThis->pDSStrmOut = NULL;

        RTCritSectLeave(&pThis->CritSect);
    }

    if (FAILED(hr))
        DSLOGREL(("DSound: Stopping playback stream %p failed with %Rhrc\n", pStreamDS, hr));

    return hr;
}


static int dsoundDestroyStreamOut(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    LogFlowFuncEnter();

    HRESULT hr = directSoundPlayStop(pThis, pStreamDS, true /* fFlush */);
    if (SUCCEEDED(hr))
    {
        hr = directSoundPlayClose(pThis, pStreamDS);
        if (FAILED(hr))
            return VERR_GENERAL_FAILURE; /** @todo Fix. */
    }

    return VINF_SUCCESS;
}


static HRESULT directSoundCaptureClose(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    AssertPtrReturn(pThis,     E_POINTER);
    AssertPtrReturn(pStreamDS, E_POINTER);

    LogFlowFuncEnter();

    HRESULT hr = directSoundCaptureStop(pThis, pStreamDS, true /* fFlush */);
    if (FAILED(hr))
        return hr;

    if (   pStreamDS
        && pStreamDS->In.pDSCB)
    {
        DSLOG(("DSound: Closing capturing stream\n"));

        IDirectSoundCaptureBuffer8_Release(pStreamDS->In.pDSCB);
        pStreamDS->In.pDSCB = NULL;
    }

    LogFlowFunc(("Returning %Rhrc\n", hr));
    return hr;
}


static int dsoundDestroyStreamIn(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    LogFlowFuncEnter();

    directSoundCaptureClose(pThis, pStreamDS);

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamDestroy}
 */
static DECLCALLBACK(int) drvHostDSoundHA_StreamDestroy(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    PDRVHOSTDSOUND pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio);
    PDSOUNDSTREAM  pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, VERR_INVALID_POINTER);

    int rc;
    if (pStreamDS->Cfg.enmDir == PDMAUDIODIR_IN)
        rc = dsoundDestroyStreamIn(pThis, pStreamDS);
    else
        rc = dsoundDestroyStreamOut(pThis, pStreamDS);

    if (RT_SUCCESS(rc))
    {
        if (RTCritSectIsInitialized(&pStreamDS->CritSect))
            rc = RTCritSectDelete(&pStreamDS->CritSect);
    }

    return rc;
}


/**
 * Enables or disables a stream.
 *
 * @return  VBox status code.
 * @param   pThis               Host audio driver instance.
 * @param   pStreamDS           Stream to enable / disable.
 * @param   fEnable             Whether to enable or disable the stream.
 */
static int dsoundStreamEnable(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, bool fEnable)
{
    RT_NOREF(pThis);

    LogFunc(("%s %s\n",
             fEnable ? "Enabling" : "Disabling",
             pStreamDS->Cfg.enmDir == PDMAUDIODIR_IN ? "capture" : "playback"));

    if (fEnable)
        drvHostDSoundStreamReset(pThis, pStreamDS);

    pStreamDS->fEnabled = fEnable;

    return VINF_SUCCESS;
}


/**
 * Starts playing a DirectSound stream.
 *
 * @return  HRESULT
 * @param   pThis               Host audio driver instance.
 * @param   pStreamDS           Stream to start playing.
 */
static HRESULT directSoundPlayStart(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    HRESULT hr = S_OK;

    DWORD fFlags = DSCBSTART_LOOPING;

    for (unsigned i = 0; i < DRV_DSOUND_RESTORE_ATTEMPTS_MAX; i++)
    {
        DSLOG(("DSound: Starting playback\n"));
        hr = IDirectSoundBuffer8_Play(pStreamDS->Out.pDSB, 0, 0, fFlags);
        if (   SUCCEEDED(hr)
            || hr != DSERR_BUFFERLOST)
            break;
        LogFunc(("Restarting playback failed due to lost buffer, restoring ...\n"));
        directSoundPlayRestore(pThis, pStreamDS->Out.pDSB);
    }

    return hr;
}


static HRESULT directSoundPlayStop(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, bool fFlush)
{
    AssertPtrReturn(pThis,     E_POINTER);
    AssertPtrReturn(pStreamDS, E_POINTER);

    HRESULT hr = S_OK;

    if (pStreamDS->Out.pDSB)
    {
        DSLOG(("DSound: Stopping playback\n"));
        hr = IDirectSoundBuffer8_Stop(pStreamDS->Out.pDSB);
        if (FAILED(hr))
        {
            hr = directSoundPlayRestore(pThis, pStreamDS->Out.pDSB);
            if (FAILED(hr)) /** @todo shouldn't this be a SUCCEEDED? */
                hr = IDirectSoundBuffer8_Stop(pStreamDS->Out.pDSB);
        }
    }

    if (SUCCEEDED(hr))
    {
        if (fFlush)
            drvHostDSoundStreamReset(pThis, pStreamDS);
    }

    if (FAILED(hr))
        DSLOGREL(("DSound: %s playback failed with %Rhrc\n", fFlush ? "Stopping" : "Pausing", hr));

    return hr;
}


static int dsoundControlStreamOut(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    LogFlowFunc(("pStreamDS=%p, cmd=%d\n", pStreamDS, enmStreamCmd));

    HRESULT hr;
    int     rc = VINF_SUCCESS;
    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        {
            dsoundStreamEnable(pThis, pStreamDS, true /* fEnable */);
            break;
        }

        case PDMAUDIOSTREAMCMD_RESUME:
        {
            /* Only resume if the stream is enabled.
               Note! This used to always just resume the stream playback regardless of state,
                     and instead rely on DISABLE filling the buffer with silence. */
            if (pStreamDS->fEnabled)
            {
                hr = directSoundPlayStart(pThis, pStreamDS);
                if (FAILED(hr))
                    rc = VERR_NOT_SUPPORTED; /** @todo Fix this. */
            }
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        {
            dsoundStreamEnable(pThis, pStreamDS, false /* fEnable */);

            /* Don't stop draining buffers. They'll stop by themselves. */
            if (pStreamDS->Cfg.enmDir != PDMAUDIODIR_OUT || !pStreamDS->Out.fDrain)
            {
                hr = directSoundPlayStop(pThis, pStreamDS, true /* fFlush */);
                if (FAILED(hr))
                    rc = VERR_NOT_SUPPORTED;
            }
            break;
        }

        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            hr = directSoundPlayStop(pThis, pStreamDS, false /* fFlush */);
            if (FAILED(hr))
                rc = VERR_NOT_SUPPORTED;
            break;
        }

        case PDMAUDIOSTREAMCMD_DRAIN:
        {
            /* Make sure we transferred everything. */
            pStreamDS->fEnabled = true; /** @todo r=bird: ??? */

            /*
             * We've started the buffer in looping mode, try switch to non-looping...
             */
            if (pStreamDS->Out.pDSB)
            {
                Log2Func(("drain: Switching playback to non-looping mode...\n"));
                HRESULT hrc = IDirectSoundBuffer8_Stop(pStreamDS->Out.pDSB);
                if (SUCCEEDED(hrc))
                {
                    hrc = IDirectSoundBuffer8_Play(pStreamDS->Out.pDSB, 0, 0, 0);
                    if (SUCCEEDED(hrc))
                        pStreamDS->Out.fDrain = true;
                    else
                        Log2Func(("drain: IDirectSoundBuffer8_Play(,,,0) failed: %Rhrc\n", hrc));
                }
                else
                {
                    Log2Func(("drain: IDirectSoundBuffer8_Stop failed: %Rhrc\n", hrc));
                    hrc = directSoundPlayRestore(pThis, pStreamDS->Out.pDSB);
                    if (SUCCEEDED(hrc))
                    {
                        hrc = IDirectSoundBuffer8_Stop(pStreamDS->Out.pDSB);
                        Log2Func(("drain: IDirectSoundBuffer8_Stop failed: %Rhrc\n", hrc));
                    }
                }
            }
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}


static HRESULT directSoundCaptureStart(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS)
{
    AssertPtrReturn(pThis,     E_POINTER);
    AssertPtrReturn(pStreamDS, E_POINTER);

    HRESULT hr;
    if (pStreamDS->In.pDSCB)
    {
        DWORD dwStatus;
        hr = IDirectSoundCaptureBuffer8_GetStatus(pStreamDS->In.pDSCB, &dwStatus);
        if (FAILED(hr))
        {
            DSLOGREL(("DSound: Retrieving capture status failed with %Rhrc\n", hr));
        }
        else
        {
            if (dwStatus & DSCBSTATUS_CAPTURING)
            {
                DSLOG(("DSound: Already capturing\n"));
            }
            else
            {
                const DWORD fFlags = DSCBSTART_LOOPING;

                DSLOG(("DSound: Starting to capture\n"));
                hr = IDirectSoundCaptureBuffer8_Start(pStreamDS->In.pDSCB, fFlags);
                if (FAILED(hr))
                    DSLOGREL(("DSound: Starting to capture failed with %Rhrc\n", hr));
            }
        }
    }
    else
        hr = E_UNEXPECTED;

    LogFlowFunc(("Returning %Rhrc\n", hr));
    return hr;
}


static HRESULT directSoundCaptureStop(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, bool fFlush)
{
    AssertPtrReturn(pThis,     E_POINTER);
    AssertPtrReturn(pStreamDS, E_POINTER);

    RT_NOREF(pThis);

    HRESULT hr = S_OK;

    if (pStreamDS->In.pDSCB)
    {
        DSLOG(("DSound: Stopping capture\n"));
        hr = IDirectSoundCaptureBuffer_Stop(pStreamDS->In.pDSCB);
    }

    if (SUCCEEDED(hr))
    {
        if (fFlush)
             drvHostDSoundStreamReset(pThis, pStreamDS);
    }

    if (FAILED(hr))
        DSLOGREL(("DSound: Stopping capture buffer failed with %Rhrc\n", hr));

    return hr;
}


static int dsoundControlStreamIn(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    LogFlowFunc(("pStreamDS=%p, enmStreamCmd=%ld\n", pStreamDS, enmStreamCmd));

    int rc = VINF_SUCCESS;

    HRESULT hr;
    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
            dsoundStreamEnable(pThis, pStreamDS, true /* fEnable */);
            RT_FALL_THROUGH();
        case PDMAUDIOSTREAMCMD_RESUME:
        {
            /* Try to start capture. If it fails, then reopen and try again. */
            hr = directSoundCaptureStart(pThis, pStreamDS);
            if (FAILED(hr))
            {
                hr = directSoundCaptureClose(pThis, pStreamDS);
                if (SUCCEEDED(hr))
                {
/** @todo r=bird: This isn't working. CfgAcq isn't initialized! */
                    PDMAUDIOSTREAMCFG CfgAcq;
                    WAVEFORMATEX WaveFmtX;
                    dsoundWaveFmtFromCfg(&pStreamDS->Cfg, &WaveFmtX);
                    hr = drvHostDSoundStreamCreateCapture(pThis, pStreamDS, &pStreamDS->Cfg, &CfgAcq, &WaveFmtX);
                    if (SUCCEEDED(hr))
                    {
                        PDMAudioStrmCfgCopy(&pStreamDS->Cfg, &CfgAcq);

                        /** @todo What to do if the format has changed? */

                        hr = directSoundCaptureStart(pThis, pStreamDS);
                    }
                }
            }

            if (FAILED(hr))
                rc = VERR_NOT_SUPPORTED;
            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
            dsoundStreamEnable(pThis, pStreamDS, false /* fEnable */);
            RT_FALL_THROUGH();
        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            directSoundCaptureStop(pThis, pStreamDS,
                                   enmStreamCmd == PDMAUDIOSTREAMCMD_DISABLE /* fFlush */);

            /* Return success in any case, as stopping the capture can fail if
             * the capture buffer is not around anymore.
             *
             * This can happen if the host's capturing device has been changed suddenly. */
            rc = VINF_SUCCESS;
            break;
        }

        default:
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamControl}
 */
static DECLCALLBACK(int) drvHostDSoundHA_StreamControl(PPDMIHOSTAUDIO pInterface,
                                                       PPDMAUDIOBACKENDSTREAM pStream, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    PDRVHOSTDSOUND pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio);
    PDSOUNDSTREAM  pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, VERR_INVALID_POINTER);

    int rc;
    if (pStreamDS->Cfg.enmDir == PDMAUDIODIR_IN)
        rc = dsoundControlStreamIn(pThis,  pStreamDS, enmStreamCmd);
    else
        rc = dsoundControlStreamOut(pThis, pStreamDS, enmStreamCmd);

    return rc;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetReadable}
 */
static DECLCALLBACK(uint32_t) drvHostDSoundHA_StreamGetReadable(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    /*PDRVHOSTDSOUND  pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio); */ RT_NOREF(pInterface);
    PDSOUNDSTREAM   pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, 0);
    Assert(pStreamDS->Cfg.enmDir == PDMAUDIODIR_IN);

    if (pStreamDS->fEnabled)
    {
        /* This is the same calculation as for StreamGetPending. */
        AssertPtr(pStreamDS->In.pDSCB);
        DWORD   offCaptureCursor = 0;
        DWORD   offReadCursor    = 0;
        HRESULT hrc = IDirectSoundCaptureBuffer_GetCurrentPosition(pStreamDS->In.pDSCB, &offCaptureCursor, &offReadCursor);
        if (SUCCEEDED(hrc))
        {
            uint32_t cbPending = dsoundRingDistance(offCaptureCursor, offReadCursor, pStreamDS->cbBufSize);
            Log3Func(("cbPending=%RU32\n", cbPending));
            return cbPending;
        }
        AssertMsgFailed(("hrc=%Rhrc\n", hrc));
    }

    return 0;
}


/**
 * Retrieves the number of free bytes available for writing to a DirectSound output stream.
 *
 * @return  VBox status code. VERR_NOT_AVAILABLE if unable to determine or the
 *          buffer was not recoverable.
 * @param   pThis               Host audio driver instance.
 * @param   pStreamDS           DirectSound output stream to retrieve number for.
 * @param   pdwFree             Where to return the free amount on success.
 * @param   poffPlayCursor      Where to return the play cursor offset.
 */
static int dsoundGetFreeOut(PDRVHOSTDSOUND pThis, PDSOUNDSTREAM pStreamDS, DWORD *pdwFree, DWORD *poffPlayCursor)
{
    AssertPtrReturn(pThis,     VERR_INVALID_POINTER);
    AssertPtrReturn(pStreamDS, VERR_INVALID_POINTER);
    AssertPtrReturn(pdwFree,   VERR_INVALID_POINTER);
    AssertPtr(poffPlayCursor);

    Assert(pStreamDS->Cfg.enmDir == PDMAUDIODIR_OUT); /* Paranoia. */

    LPDIRECTSOUNDBUFFER8 pDSB = pStreamDS->Out.pDSB;
    if (!pDSB)
    {
        AssertPtr(pDSB);
        return VERR_INVALID_POINTER;
    }

    HRESULT hr = S_OK;

    /* Get the current play position which is used for calculating the free space in the buffer. */
    for (unsigned i = 0; i < DRV_DSOUND_RESTORE_ATTEMPTS_MAX; i++)
    {
        DWORD offPlayCursor  = 0;
        DWORD offWriteCursor = 0;
        hr = IDirectSoundBuffer8_GetCurrentPosition(pDSB, &offPlayCursor, &offWriteCursor);
        if (SUCCEEDED(hr))
        {
            int32_t cbDiff = offWriteCursor - offPlayCursor;
            if (cbDiff < 0)
                cbDiff += pStreamDS->cbBufSize;

            int32_t cbFree = offPlayCursor - pStreamDS->Out.offWritePos;
            if (cbFree < 0)
                cbFree += pStreamDS->cbBufSize;

            if (cbFree > (int32_t)pStreamDS->cbBufSize - cbDiff)
            {
                /** @todo count/log these. */
                pStreamDS->Out.offWritePos = offWriteCursor;
                cbFree = pStreamDS->cbBufSize - cbDiff;
            }

            /* When starting to use a DirectSound buffer, offPlayCursor and offWriteCursor
             * both point at position 0, so we won't be able to detect how many bytes
             * are writable that way.
             *
             * So use our per-stream written indicator to see if we just started a stream. */
            if (pStreamDS->Out.cbWritten == 0)
                cbFree = pStreamDS->cbBufSize;

            DSLOGREL(("DSound: offPlayCursor=%RU32, offWriteCursor=%RU32, offWritePos=%RU32 -> cbFree=%RI32\n",
                      offPlayCursor, offWriteCursor, pStreamDS->Out.offWritePos, cbFree));

            *pdwFree = cbFree;
            *poffPlayCursor = offPlayCursor;
            return VINF_SUCCESS;
        }

        if (hr != DSERR_BUFFERLOST) /** @todo MSDN doesn't state this error for GetCurrentPosition(). */
            break;

        LogFunc(("Getting playing position failed due to lost buffer, restoring ...\n"));

        directSoundPlayRestore(pThis, pDSB);
    }

    if (hr != DSERR_BUFFERLOST) /* Avoid log flooding if the error is still there. */
        DSLOGREL(("DSound: Getting current playback position failed with %Rhrc\n", hr));

    LogFunc(("Failed with %Rhrc\n", hr));

    *poffPlayCursor = pStreamDS->cbBufSize;
    return VERR_NOT_AVAILABLE;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetWritable}
 */
static DECLCALLBACK(uint32_t) drvHostDSoundHA_StreamGetWritable(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    PDRVHOSTDSOUND  pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio);
    PDSOUNDSTREAM   pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, 0);

    DWORD           cbFree    = 0;
    DWORD           offIgn    = 0;
    int rc = dsoundGetFreeOut(pThis, pStreamDS, &cbFree, &offIgn);
    AssertRCReturn(rc, 0);

    return cbFree;
}

#if 0 /* This isn't working as the write cursor is more a function of time than what we do.
         Previously we only reported the pre-buffering status anyway, so no harm. */
/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetPending}
 */
static DECLCALLBACK(uint32_t) drvHostDSoundHA_StreamGetPending(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    /*PDRVHOSTDSOUND  pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio); */ RT_NOREF(pInterface);
    PDSOUNDSTREAM   pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, 0);

    if (pStreamDS->Cfg.enmDir == PDMAUDIODIR_OUT)
    {
        /* This is a similar calculation as for StreamGetReadable, only for an output buffer. */
        AssertPtr(pStreamDS->In.pDSCB);
        DWORD   offPlayCursor  = 0;
        DWORD   offWriteCursor = 0;
        HRESULT hrc = IDirectSoundBuffer8_GetCurrentPosition(pStreamDS->Out.pDSB, &offPlayCursor, &offWriteCursor);
        if (SUCCEEDED(hrc))
        {
            uint32_t cbPending = dsoundRingDistance(offWriteCursor, offPlayCursor, pStreamDS->cbBufSize);
            Log3Func(("cbPending=%RU32\n", cbPending));
            return cbPending;
        }
        AssertMsgFailed(("hrc=%Rhrc\n", hrc));
    }
    /* else: For input streams we never have any pending data. */

    return 0;
}
#endif

/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamGetStatus}
 */
static DECLCALLBACK(PDMAUDIOSTREAMSTS) drvHostDSoundHA_StreamGetStatus(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream)
{
    RT_NOREF(pInterface);
    AssertPtrReturn(pStream, PDMAUDIOSTREAMSTS_FLAGS_NONE);

    PDSOUNDSTREAM pStreamDS = (PDSOUNDSTREAM)pStream;

    PDMAUDIOSTREAMSTS fStrmStatus = PDMAUDIOSTREAMSTS_FLAGS_INITIALIZED;

    if (pStreamDS->fEnabled)
        fStrmStatus |= PDMAUDIOSTREAMSTS_FLAGS_ENABLED;

    return fStrmStatus;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamPlay}
 */
static DECLCALLBACK(int) drvHostDSoundHA_StreamPlay(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream,
                                                    const void *pvBuf, uint32_t cbBuf, uint32_t *pcbWritten)
{
    PDRVHOSTDSOUND  pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio);
    PDSOUNDSTREAM   pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, 0);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    AssertReturn(cbBuf, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbWritten, VERR_INVALID_POINTER);

    if (pStreamDS->fEnabled)
        AssertReturn(pStreamDS->cbBufSize, VERR_INTERNAL_ERROR_2);
    else
    {
        Log2Func(("Stream disabled, skipping\n"));
        return VINF_SUCCESS;
    }

/** @todo Any condition under which we should call dsoundUpdateStatusInternal(pThis) here?
 * The old code thought it did so in case of failure, only it couldn't ever fails, so it never did. */

    /*
     * Transfer loop.
     */
    uint32_t cbWritten = 0;
    while (cbBuf > 0)
    {
        /*
         * Figure out how much we can possibly write.
         */
        DWORD offPlayCursor = 0;
        DWORD cbWritable    = 0;
        int rc = dsoundGetFreeOut(pThis, pStreamDS, &cbWritable, &offPlayCursor);
        AssertRCReturn(rc, rc);
        if (cbWritable < pStreamDS->Cfg.Props.cbFrame)
            break;

        uint32_t const cbToWrite = RT_MIN(cbWritable, cbBuf);
        Log3Func(("offPlay=%#x offWritePos=%#x -> cbWritable=%#x cbToWrite=%#x%s%s\n", offPlayCursor, pStreamDS->Out.offWritePos,
                  cbWritable, cbToWrite, pStreamDS->Out.fFirstTransfer ? " first" : "", pStreamDS->Out.fDrain ? " drain" : ""));

        /*
         * Lock that amount of buffer.
         */
        PVOID pv1 = NULL;
        DWORD cb1 = 0;
        PVOID pv2 = NULL;
        DWORD cb2 = 0;
        HRESULT hrc = directSoundPlayLock(pThis, pStreamDS, pStreamDS->Out.offWritePos, cbToWrite,
                                          &pv1, &pv2, &cb1, &cb2, 0 /*dwFlags*/);
        AssertMsgReturn(SUCCEEDED(hrc), ("%Rhrc\n", hrc), VERR_ACCESS_DENIED); /** @todo translate these status codes already! */
        //AssertMsg(cb1 + cb2 == cbToWrite, ("%#x + %#x vs %#x\n", cb1, cb2, cbToWrite));

        /*
         * Copy over the data.
         */
        memcpy(pv1, pvBuf, cb1);
        pvBuf      = (uint8_t *)pvBuf + cb1;
        cbBuf     -= cb1;
        cbWritten += cb1;

        if (pv2)
        {
            memcpy(pv2, pvBuf, cb2);
            pvBuf      = (uint8_t *)pvBuf + cb2;
            cbBuf     -= cb2;
            cbWritten += cb2;
        }

        /*
         * Unlock and update the write position.
         */
        directSoundPlayUnlock(pThis, pStreamDS->Out.pDSB, pv1, pv2, cb1, cb2); /** @todo r=bird: pThis + pDSB parameters here for Unlock, but only pThis for Lock. Why? */
        pStreamDS->Out.offWritePos = (pStreamDS->Out.offWritePos + cb1 + cb2) % pStreamDS->cbBufSize;

        /*
         * If this was the first chunk, kick off playing.
         */
        if (!pStreamDS->Out.fFirstTransfer)
        { /* likely */ }
        else
        {
            *pcbWritten = cbWritten;
            hrc = directSoundPlayStart(pThis, pStreamDS);
            AssertMsgReturn(SUCCEEDED(hrc), ("%Rhrc\n", hrc), VERR_ACCESS_DENIED); /** @todo translate these status codes already! */
            pStreamDS->Out.fFirstTransfer = false;
        }
    }

    /*
     * Done.
     */
    *pcbWritten = cbWritten;

    pStreamDS->Out.cbTransferred += cbWritten;
    if (cbWritten)
    {
        uint64_t const msPrev = pStreamDS->Out.tsLastTransferredMs;
        pStreamDS->Out.cbLastTransferred   = cbWritten;
        pStreamDS->Out.tsLastTransferredMs = RTTimeMilliTS();
        LogFlowFunc(("cbLastTransferred=%RU32, tsLastTransferredMs=%RU64 cMsDelta=%RU64\n",
                     cbWritten, pStreamDS->Out.tsLastTransferredMs, msPrev ? pStreamDS->Out.tsLastTransferredMs - msPrev : 0));
    }
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIHOSTAUDIO,pfnStreamCapture}
 */
static DECLCALLBACK(int) drvHostDSoundHA_StreamCapture(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDSTREAM pStream,
                                                       void *pvBuf, uint32_t cbBuf, uint32_t *pcbRead)
{
    /*PDRVHOSTDSOUND  pThis     = RT_FROM_MEMBER(pInterface, DRVHOSTDSOUND, IHostAudio);*/ RT_NOREF(pInterface);
    PDSOUNDSTREAM   pStreamDS = (PDSOUNDSTREAM)pStream;
    AssertPtrReturn(pStreamDS, 0);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    AssertReturn(cbBuf, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbRead, VERR_INVALID_POINTER);

#if 0 /** @todo r=bird: shouldn't we do the same check as for output streams? */
    if (pStreamDS->fEnabled)
        AssertReturn(pStreamDS->cbBufSize, VERR_INTERNAL_ERROR_2);
    else
    {
        Log2Func(("Stream disabled, skipping\n"));
        return VINF_SUCCESS;
    }
#endif

    /*
     * Read loop.
     */
    uint32_t cbRead = 0;
    while (cbBuf > 0)
    {
        /*
         * Figure out how much we can read.
         */
        DWORD   offCaptureCursor = 0;
        DWORD   offReadCursor    = 0;
        HRESULT hrc = IDirectSoundCaptureBuffer_GetCurrentPosition(pStreamDS->In.pDSCB, &offCaptureCursor, &offReadCursor);
        AssertMsgReturn(SUCCEEDED(hrc), ("%Rhrc\n", hrc), VERR_ACCESS_DENIED); /** @todo translate these status codes already! */
        //AssertMsg(offReadCursor == pStreamDS->In.offReadPos, ("%#x %#x\n", offReadCursor, pStreamDS->In.offReadPos));

        uint32_t const cbReadable = dsoundRingDistance(offCaptureCursor, pStreamDS->In.offReadPos, pStreamDS->cbBufSize);

        if (cbReadable >= pStreamDS->Cfg.Props.cbFrame)
        { /* likely */ }
        else
        {
            if (cbRead > 0)
            { /* likely */ }
            else if (pStreamDS->In.cOverruns < 32)
            {
                pStreamDS->In.cOverruns++;
                DSLOG(("DSound: Warning: Buffer full (size is %zu bytes), skipping to record data (overflow #%RU32)\n",
                       pStreamDS->cbBufSize, pStreamDS->In.cOverruns));
            }
            break;
        }

        uint32_t const cbToRead = RT_MIN(cbReadable, cbBuf);
        Log3Func(("offCapture=%#x offRead=%#x/%#x -> cbWritable=%#x cbToWrite=%#x\n",
                  offCaptureCursor, offReadCursor, pStreamDS->In.offReadPos, cbReadable, cbToRead));

        /*
         * Lock that amount of buffer.
         */
        PVOID pv1 = NULL;
        DWORD cb1 = 0;
        PVOID pv2 = NULL;
        DWORD cb2 = 0;
        hrc = directSoundCaptureLock(pStreamDS, pStreamDS->In.offReadPos, cbToRead, &pv1, &pv2, &cb1, &cb2, 0 /*dwFlags*/);
        AssertMsgReturn(SUCCEEDED(hrc), ("%Rhrc\n", hrc), VERR_ACCESS_DENIED); /** @todo translate these status codes already! */
        AssertMsg(cb1 + cb2 == cbToRead, ("%#x + %#x vs %#x\n", cb1, cb2, cbToRead));

        /*
         * Copy over the data.
         */
        memcpy(pvBuf, pv1, cb1);
        pvBuf   = (uint8_t *)pvBuf + cb1;
        cbBuf  -= cb1;
        cbRead += cb1;

        if (pv2)
        {
            memcpy(pvBuf, pv2, cb2);
            pvBuf   = (uint8_t *)pvBuf + cb2;
            cbBuf  -= cb2;
            cbRead += cb2;
        }

        /*
         * Unlock and update the write position.
         */
        directSoundCaptureUnlock(pStreamDS->In.pDSCB, pv1, pv2, cb1, cb2); /** @todo r=bird: pDSB parameter here for Unlock, but pStreamDS for Lock. Why? */
        pStreamDS->In.offReadPos = (pStreamDS->In.offReadPos + cb1 + cb2) % pStreamDS->cbBufSize;
    }

    /*
     * Done.
     */
    if (pcbRead)
        *pcbRead = cbRead;

#ifdef VBOX_AUDIO_DEBUG_DUMP_PCM_DATA
    if (cbRead)
    {
        RTFILE hFile;
        int rc2 = RTFileOpen(&hFile, VBOX_AUDIO_DEBUG_DUMP_PCM_DATA_PATH "dsoundCapture.pcm",
                             RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
        if (RT_SUCCESS(rc2))
        {
            RTFileWrite(hFile, (uint8_t *)pvBuf - cbRead, cbRead, NULL);
            RTFileClose(hFile);
        }
    }
#endif
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   PDMDRVINS::IBase Interface                                                                                                   *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvHostDSoundQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS     pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVHOSTDSOUND pThis   = PDMINS_2_DATA(pDrvIns, PDRVHOSTDSOUND);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIHOSTAUDIO, &pThis->IHostAudio);
    return NULL;
}


/*********************************************************************************************************************************
*   PDMDRVREG Interface                                                                                                          *
*********************************************************************************************************************************/

/**
 * @callback_method_impl{FNPDMDRVDESTRUCT, pfnDestruct}
 */
static DECLCALLBACK(void) drvHostDSoundDestruct(PPDMDRVINS pDrvIns)
{
    PDRVHOSTDSOUND pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTDSOUND);
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);

    LogFlowFuncEnter();

#ifdef VBOX_WITH_AUDIO_MMNOTIFICATION_CLIENT
    if (pThis->m_pNotificationClient)
    {
        pThis->m_pNotificationClient->Unregister();
        pThis->m_pNotificationClient->Release();

        pThis->m_pNotificationClient = NULL;
    }
#endif

    PDMAudioHostEnumDelete(&pThis->DeviceEnum);

    if (pThis->pDrvIns)
        CoUninitialize();

    int rc2 = RTCritSectDelete(&pThis->CritSect);
    AssertRC(rc2);

    LogFlowFuncLeave();
}


static LPCGUID dsoundConfigQueryGUID(PCFGMNODE pCfg, const char *pszName, RTUUID *pUuid)
{
    LPCGUID pGuid = NULL;

    char *pszGuid = NULL;
    int rc = CFGMR3QueryStringAlloc(pCfg, pszName, &pszGuid);
    if (RT_SUCCESS(rc))
    {
        rc = RTUuidFromStr(pUuid, pszGuid);
        if (RT_SUCCESS(rc))
            pGuid = (LPCGUID)&pUuid;
        else
            DSLOGREL(("DSound: Error parsing device GUID for device '%s': %Rrc\n", pszName, rc));

        RTStrFree(pszGuid);
    }

    return pGuid;
}


static void dsoundConfigInit(PDRVHOSTDSOUND pThis, PCFGMNODE pCfg)
{
    pThis->Cfg.pGuidPlay    = dsoundConfigQueryGUID(pCfg, "DeviceGuidOut", &pThis->Cfg.uuidPlay);
    pThis->Cfg.pGuidCapture = dsoundConfigQueryGUID(pCfg, "DeviceGuidIn",  &pThis->Cfg.uuidCapture);

    DSLOG(("DSound: Configuration: DeviceGuidOut {%RTuuid}, DeviceGuidIn {%RTuuid}\n",
           &pThis->Cfg.uuidPlay,
           &pThis->Cfg.uuidCapture));
}


/**
 * @callback_method_impl{FNPDMDRVCONSTRUCT,
 *      Construct a DirectSound Audio driver instance.}
 */
static DECLCALLBACK(int) drvHostDSoundConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVHOSTDSOUND pThis = PDMINS_2_DATA(pDrvIns, PDRVHOSTDSOUND);
    RT_NOREF(fFlags);
    LogRel(("Audio: Initializing DirectSound audio driver\n"));

    /*
     * Init basic data members and interfaces.
     */
    pThis->pDrvIns                   = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface = drvHostDSoundQueryInterface;
    /* IHostAudio */
    pThis->IHostAudio.pfnGetConfig          = drvHostDSoundHA_GetConfig;
    pThis->IHostAudio.pfnGetDevices         = drvHostDSoundHA_GetDevices;
    pThis->IHostAudio.pfnGetStatus          = drvHostDSoundHA_GetStatus;
    pThis->IHostAudio.pfnStreamCreate       = drvHostDSoundHA_StreamCreate;
    pThis->IHostAudio.pfnStreamDestroy      = drvHostDSoundHA_StreamDestroy;
    pThis->IHostAudio.pfnStreamControl      = drvHostDSoundHA_StreamControl;
    pThis->IHostAudio.pfnStreamGetReadable  = drvHostDSoundHA_StreamGetReadable;
    pThis->IHostAudio.pfnStreamGetWritable  = drvHostDSoundHA_StreamGetWritable;
    pThis->IHostAudio.pfnStreamGetPending   = NULL;
    pThis->IHostAudio.pfnStreamGetStatus    = drvHostDSoundHA_StreamGetStatus;
    pThis->IHostAudio.pfnStreamPlay         = drvHostDSoundHA_StreamPlay;
    pThis->IHostAudio.pfnStreamCapture      = drvHostDSoundHA_StreamCapture;

    /*
     * Init the static parts.
     */
    PDMAudioHostEnumInit(&pThis->DeviceEnum);

    pThis->fEnabledIn  = false;
    pThis->fEnabledOut = false;

    /*
     * Verify that IDirectSound is available.
     */
    HRESULT hrc = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hrc))
    {
        LPDIRECTSOUND pDirectSound = NULL;
        hrc = CoCreateInstance(CLSID_DirectSound, NULL, CLSCTX_ALL, IID_IDirectSound, (void **)&pDirectSound);
        if (SUCCEEDED(hrc))
            IDirectSound_Release(pDirectSound);
        else
        {
            LogRel(("DSound: DirectSound not available: %Rhrc\n", hrc));
            return VERR_AUDIO_BACKEND_INIT_FAILED;
        }
    }
    else
    {
        LogRel(("DSound: CoInitializeEx(,COINIT_MULTITHREADED) failed: %Rhrc\n", hrc));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

#ifdef VBOX_WITH_AUDIO_MMNOTIFICATION_CLIENT
    /*
     * Set up WASAPI device change notifications (Vista+).
     */
    if (RTSystemGetNtVersion() >= RTSYSTEM_MAKE_NT_VERSION(6, 0, 0))
    {
        /* Get the notification interface (from DrvAudio). */
# ifdef VBOX_WITH_AUDIO_CALLBACKS
        PPDMIAUDIONOTIFYFROMHOST pIAudioNotifyFromHost = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIAUDIONOTIFYFROMHOST);
        Assert(pIAudioNotifyFromHost);
# else
        PPDMIAUDIONOTIFYFROMHOST pIAudioNotifyFromHost = NULL;
# endif
        try
        {
            pThis->m_pNotificationClient = new DrvHostAudioDSoundMMNotifClient(pIAudioNotifyFromHost);
        }
        catch (std::bad_alloc &)
        {
            return VERR_NO_MEMORY;
        }
        hrc = pThis->m_pNotificationClient->Initialize();
        if (SUCCEEDED(hrc))
        {
            hrc = pThis->m_pNotificationClient->Register();
            if (SUCCEEDED(hrc))
                LogRel2(("DSound: Notification client is enabled (ver %#RX64)\n", RTSystemGetNtVersion()));
            else
            {
                LogRel(("DSound: Notification client registration failed: %Rhrc\n", hrc));
                return VERR_AUDIO_BACKEND_INIT_FAILED;
            }
        }
        else
        {
            LogRel(("DSound: Notification client initialization failed: %Rhrc\n", hrc));
            return VERR_AUDIO_BACKEND_INIT_FAILED;
        }
    }
    else
        LogRel2(("DSound: Notification client is disabled (ver %#RX64)\n", RTSystemGetNtVersion()));
#endif

    /*
     * Initialize configuration values and critical section.
     */
    dsoundConfigInit(pThis, pCfg);
    return RTCritSectInit(&pThis->CritSect);
}


/**
 * PDM driver registration.
 */
const PDMDRVREG g_DrvHostDSound =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "DSoundAudio",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "DirectSound Audio host driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVHOSTDSOUND),
    /* pfnConstruct */
    drvHostDSoundConstruct,
    /* pfnDestruct */
    drvHostDSoundDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};
