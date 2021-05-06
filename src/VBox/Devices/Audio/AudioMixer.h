/* $Id$ */
/** @file
 * VBox audio - Mixing routines.
 *
 * The mixing routines are mainly used by the various audio device emulations
 * to achieve proper multiplexing from/to attached devices LUNs.
 */

/*
 * Copyright (C) 2014-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef VBOX_INCLUDED_SRC_Audio_AudioMixer_h
#define VBOX_INCLUDED_SRC_Audio_AudioMixer_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <iprt/cdefs.h>
#include <iprt/critsect.h>

#include <VBox/vmm/pdmaudioifs.h>
#include "AudioMixBuffer.h"
#include "AudioHlp.h"


/** Pointer to an audio mixer sink. */
typedef struct AUDMIXSINK *PAUDMIXSINK;


/**
 * Audio mixer instance.
 */
typedef struct AUDIOMIXER
{
    /** The mixer's name. */
    char                   *pszName;
    /** The mixer's critical section. */
    RTCRITSECT              CritSect;
    /** The master volume of this mixer. */
    PDMAUDIOVOLUME          VolMaster;
    /** List of audio mixer sinks. */
    RTLISTANCHOR            lstSinks;
    /** Number of used audio sinks. */
    uint8_t                 cSinks;
    /** Mixer flags. See AUDMIXER_FLAGS_XXX. */
    uint32_t                fFlags;
} AUDIOMIXER;
/** Pointer to an audio mixer instance. */
typedef AUDIOMIXER *PAUDIOMIXER;


/**
 * Audio mixer stream.
 */
typedef struct AUDMIXSTREAM
{
    /** List entry on AUDMIXSINK::lstStreams. */
    RTLISTNODE              Node;
    /** Name of this stream. */
    char                   *pszName;
    /** The statistics prefix. */
    char                   *pszStatPrefix;
    /** Sink this stream is attached to. */
    PAUDMIXSINK             pSink;
    /** Stream status of type AUDMIXSTREAM_STATUS_. */
    uint32_t                fStatus;
    /** Number of writable/readable frames the last time we checked. */
    uint32_t                cFramesLastAvail;
    /** Set if the stream has been found unreliable wrt. consuming/producing
     * samples, and that we shouldn't consider it when deciding how much to move
     * from the mixer buffer and to the drivers. */
    bool                    fUnreliable;
    /** Pointer to audio connector being used. */
    PPDMIAUDIOCONNECTOR     pConn;
    /** Pointer to PDM audio stream this mixer stream handles. */
    PPDMAUDIOSTREAM         pStream;
    /** Mixing buffer peeking state & config. */
    AUDIOMIXBUFPEEKSTATE    PeekState;
    /** Last read (recording) / written (playback) timestamp (in ns). */
    uint64_t                tsLastReadWrittenNs;
    /** The streams's critical section. */
    RTCRITSECT              CritSect;
} AUDMIXSTREAM;
/** Pointer to an audio mixer stream. */
typedef AUDMIXSTREAM *PAUDMIXSTREAM;

/** @name AUDMIXSTREAM_STATUS_XXX - mixer stream status.
 * (This is a destilled version of PDMAUDIOSTREAM_STS_XXX.)
 * @{ */
/** No status set. */
#define AUDMIXSTREAM_STATUS_NONE                UINT32_C(0)
/** The mixing stream is enabled (active). */
#define AUDMIXSTREAM_STATUS_ENABLED             RT_BIT_32(0)
/** The mixing stream can be read from.
 * Always set together with AUDMIXSTREAM_STATUS_ENABLED. */
#define AUDMIXSTREAM_STATUS_CAN_READ            RT_BIT_32(1)
/** The mixing stream can be written to.
 * Always set together with AUDMIXSTREAM_STATUS_ENABLED. */
#define AUDMIXSTREAM_STATUS_CAN_WRITE           RT_BIT_32(2)
/** @} */


/**
 * Audio mixer sink.
 */
typedef struct AUDMIXSINK
{
    /** List entry on AUDIOMIXER::lstSinks. */
    RTLISTNODE              Node;
    /** Pointer to mixer object this sink is bound to. */
    PAUDIOMIXER             pParent;
    /** Name of this sink. */
    char                   *pszName;
    /** The sink direction (either PDMAUDIODIR_IN or PDMAUDIODIR_OUT). */
    PDMAUDIODIR             enmDir;
    /** Scratch buffer for multiplexing / mixing. Might be NULL if not needed. */
    uint8_t                *pabScratchBuf;
    /** Size (in bytes) of pabScratchBuf. Might be 0 if not needed. */
    size_t                  cbScratchBuf;
    /** The sink's PCM format. */
    PDMAUDIOPCMPROPS        PCMProps;
    /** Sink status bits - AUDMIXSINK_STS_XXX. */
    uint32_t                fStatus;
    /** Number of streams assigned. */
    uint8_t                 cStreams;
    /** List of assigned streams.
     * @note All streams have the same PCM properties, so the mixer does not do
     *       any conversion.  bird: That is *NOT* true any more, the mixer has
     *       encoders/decoder states for each stream (well, input is still a todo).
     *
     * @todo Use something faster -- vector maybe?  bird: It won't be faster.  You
     *       will have a vector of stream pointers (because you cannot have a vector
     *       of full AUDMIXSTREAM structures since they'll move when the vector is
     *       reallocated and we need pointers to them to give out to devices), which
     *       is the same cost as going via Node.pNext/pPrev. */
    RTLISTANCHOR            lstStreams;
    /** The volume of this sink. The volume always will
     *  be combined with the mixer's master volume. */
    PDMAUDIOVOLUME          Volume;
    /** The volume of this sink, combined with the last set  master volume. */
    PDMAUDIOVOLUME          VolumeCombined;
    /** Timestamp since last update (in ms). */
    uint64_t                tsLastUpdatedMs;
    /** Last read (recording) / written (playback) timestamp (in ns). */
    uint64_t                tsLastReadWrittenNs;
    /** Union for input/output specifics. */
    union
    {
        struct
        {
            /** The current recording source. Can be NULL if not set. */
            PAUDMIXSTREAM  pStreamRecSource;
        } In;
        /*struct
        {
        } Out; */
    };
    struct
    {
        PAUDIOHLPFILE       pFile;
    } Dbg;
    /** This sink's mixing buffer, acting as
     * a parent buffer for all streams this sink owns. */
    AUDIOMIXBUF             MixBuf;
    /** The sink's critical section. */
    RTCRITSECT              CritSect;
} AUDMIXSINK;

/** @name AUDMIXSINK_STS_XXX - Sink status bits.
 * @{ */
/** No status specified. */
#define AUDMIXSINK_STS_NONE                  0
/** The sink is active and running. */
#define AUDMIXSINK_STS_RUNNING               RT_BIT(0)
/** The sink is in a pending disable state. */
#define AUDMIXSINK_STS_PENDING_DISABLE       RT_BIT(1)
/** Dirty flag.
 * - For output sinks this means that there is data in the sink which has not
 *   been played yet.
 * - For input sinks this means that there is data in the sink which has been
 *   recorded but not transferred to the destination yet. */
#define AUDMIXSINK_STS_DIRTY                 RT_BIT(2)
/** @} */


/**
 * Audio mixer operation.
 */
typedef enum AUDMIXOP
{
    /** Invalid operation, do not use. */
    AUDMIXOP_INVALID = 0,
    /** Copy data from A to B, overwriting data in B. */
    AUDMIXOP_COPY,
    /** Blend data from A with (existing) data in B. */
    AUDMIXOP_BLEND,
    /** The usual 32-bit hack. */
    AUDMIXOP_32BIT_HACK = 0x7fffffff
} AUDMIXOP;


/** @name AUDMIXER_FLAGS_XXX - For AudioMixerCreate().
 * @{ */
/** No mixer flags specified. */
#define AUDMIXER_FLAGS_NONE             0
/** Debug mode enabled.
 *  This writes .WAV file to the host, usually to the temporary directory. */
#define AUDMIXER_FLAGS_DEBUG            RT_BIT(0)
/** Validation mask. */
#define AUDMIXER_FLAGS_VALID_MASK       UINT32_C(0x00000001)
/** @} */


int AudioMixerCreate(const char *pszName, uint32_t fFlags, PAUDIOMIXER *ppMixer);
int AudioMixerCreateSink(PAUDIOMIXER pMixer, const char *pszName, PDMAUDIODIR enmDir, PPDMDEVINS pDevIns, PAUDMIXSINK *ppSink);
void AudioMixerDestroy(PAUDIOMIXER pMixer, PPDMDEVINS pDevIns);
void AudioMixerInvalidate(PAUDIOMIXER pMixer);
int AudioMixerSetMasterVolume(PAUDIOMIXER pMixer, PPDMAUDIOVOLUME pVol);
void AudioMixerDebug(PAUDIOMIXER pMixer, PCDBGFINFOHLP pHlp, const char *pszArgs);

int     AudioMixerSinkAddStream(PAUDMIXSINK pSink, PAUDMIXSTREAM pStream);
int     AudioMixerSinkCreateStream(PAUDMIXSINK pSink, PPDMIAUDIOCONNECTOR pConnector, PPDMAUDIOSTREAMCFG pCfg,
                                   PPDMDEVINS pDevIns, PAUDMIXSTREAM *ppStream);
int     AudioMixerSinkCtl(PAUDMIXSINK pSink, PDMAUDIOSTREAMCMD enmCmd);
void AudioMixerSinkDestroy(PAUDMIXSINK pSink, PPDMDEVINS pDevIns);
uint32_t AudioMixerSinkGetReadable(PAUDMIXSINK pSink);
uint32_t AudioMixerSinkGetWritable(PAUDMIXSINK pSink);
PDMAUDIODIR   AudioMixerSinkGetDir(PAUDMIXSINK pSink);
PAUDMIXSTREAM AudioMixerSinkGetRecordingSource(PAUDMIXSINK pSink);
uint32_t AudioMixerSinkGetStatus(PAUDMIXSINK pSink);
bool AudioMixerSinkIsActive(PAUDMIXSINK pSink);
int AudioMixerSinkRead(PAUDMIXSINK pSink, AUDMIXOP enmOp, void *pvBuf, uint32_t cbBuf, uint32_t *pcbRead);
void AudioMixerSinkRemoveStream(PAUDMIXSINK pSink, PAUDMIXSTREAM pStream);
void AudioMixerSinkRemoveAllStreams(PAUDMIXSINK pSink);
void AudioMixerSinkReset(PAUDMIXSINK pSink);
int AudioMixerSinkSetFormat(PAUDMIXSINK pSink, PCPDMAUDIOPCMPROPS pPCMProps);
int AudioMixerSinkSetRecordingSource(PAUDMIXSINK pSink, PAUDMIXSTREAM pStream);
int AudioMixerSinkSetVolume(PAUDMIXSINK pSink, PPDMAUDIOVOLUME pVol);
int AudioMixerSinkWrite(PAUDMIXSINK pSink, AUDMIXOP enmOp, const void *pvBuf, uint32_t cbBuf, uint32_t *pcbWritten);
int AudioMixerSinkUpdate(PAUDMIXSINK pSink);

void AudioMixerStreamDestroy(PAUDMIXSTREAM pStream, PPDMDEVINS pDevIns);

#endif /* !VBOX_INCLUDED_SRC_Audio_AudioMixer_h */

