#include "Samples.h"

extern PSampleConfiguration gSampleConfiguration;

#define LOCAL_MASTER_USE_DEFAULT_STUN_ENV "LOCAL_MASTER_USE_DEFAULT_STUN"
#define LOCAL_MASTER_ICE_SERVER_URI_ENV "LOCAL_MASTER_ICE_SERVER_URI"
#define LOCAL_MASTER_ICE_SERVER_USERNAME_ENV "LOCAL_MASTER_ICE_SERVER_USERNAME"
#define LOCAL_MASTER_ICE_SERVER_CREDENTIAL_ENV "LOCAL_MASTER_ICE_SERVER_CREDENTIAL"

STATUS sampleSendSdpAnswer(UINT64 customData, PCHAR peerClientId, PCHAR sdpAnswer)
{
    UNUSED_PARAM(customData);
    DLOGW("[KVS Master Local] sendSdpAnswer callback not implemented. PeerId=%s", peerClientId);
    DLOGD("[KVS Master Local] Answer payload: %s", sdpAnswer);
    return STATUS_SUCCESS;
}

STATUS sampleSendIceCandidate(UINT64 customData, PCHAR peerClientId, PCHAR iceCandidate)
{
    UNUSED_PARAM(customData);
    DLOGW("[KVS Master Local] sendIceCandidate callback not implemented. PeerId=%s", peerClientId);
    DLOGD("[KVS Master Local] ICE payload: %s", iceCandidate);
    return STATUS_SUCCESS;
}

STATUS localReadFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;
    CHK_ERR(pSize != NULL, STATUS_NULL_ARG, "[KVS Master Local] Invalid file size");
    size = *pSize;
    CHK_STATUS(readFile(frameFilePath, TRUE, pFrame, &size));

CleanUp:

    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }

    return retStatus;
}

PVOID localSendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT32 i;
    UINT64 startTime, elapsed;

    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));
    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master Local] Streaming session is NULL");

    frame.presentationTs = 0;
    startTime = GETTIME();

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        if (pSampleConfiguration->videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
            SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%04d.h264", fileIndex);
        } else if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
            SNPRINTF(filePath, MAX_PATH_LEN, "./h265SampleFrames/frame-%04d.h265", fileIndex);
        }

        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, filePath));
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pVideoFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY,
                    "[KVS Master Local] Failed to allocate video frame buffer");
            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;
        CHK_STATUS(localReadFrameFromDisk(frame.frameData, &frameSize, filePath));

        encoderStats.width = 640;
        encoderStats.height = 480;
        encoderStats.targetBitrate = 262000;
        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;
        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
            if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }
            encoderStats.encodeTimeMsec = 4;
            updateEncoderStats(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &encoderStats);
            if (status == STATUS_SRTP_NOT_READY_YET) {
                fileIndex = 0;
            } else if (status != STATUS_SUCCESS) {
                DLOGV("writeFrame() failed with 0x%08x", status);
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        elapsed = GETTIME() - startTime;
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION - elapsed % SAMPLE_VIDEO_FRAME_DURATION);
    }

CleanUp:

    DLOGI("[KVS Master Local] Closing video thread");
    CHK_LOG_ERR(retStatus);
    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID localSendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT32 i;
    STATUS status;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master Local] Streaming session is NULL");
    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        if (pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
            fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;
            SNPRINTF(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_AAC) {
            fileIndex = fileIndex % NUMBER_OF_AAC_FRAME_FILES + 1;
            SNPRINTF(filePath, MAX_PATH_LEN, "./aacSampleFrames/sample-%03d.aac", fileIndex);
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_ALAW) {
            fileIndex = fileIndex % NUMBER_OF_ALAW_FRAME_FILES + 1;
            SNPRINTF(filePath, MAX_PATH_LEN, "./alawSampleFrames/sample-%03d.pcm", fileIndex);
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_MULAW) {
            fileIndex = fileIndex % NUMBER_OF_MULAW_FRAME_FILES + 1;
            SNPRINTF(filePath, MAX_PATH_LEN, "./mulawSampleFrames/sample-%03d.pcm", fileIndex);
        }

        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, filePath));
        if (frameSize > pSampleConfiguration->audioBufferSize) {
            pSampleConfiguration->pAudioFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pAudioFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pAudioFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY,
                    "[KVS Master Local] Failed to allocate audio frame buffer");
            pSampleConfiguration->audioBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
        frame.size = frameSize;
        CHK_STATUS(localReadFrameFromDisk(frame.frameData, &frameSize, filePath));

        if (pSampleConfiguration->audioCodec == RTC_CODEC_AAC) {
            frame.presentationTs += SAMPLE_AUDIO_AAC_FRAME_DURATION;
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_ALAW) {
            frame.presentationTs += SAMPLE_AUDIO_ALAW_FRAME_DURATION;
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_MULAW) {
            frame.presentationTs += SAMPLE_AUDIO_MULAW_FRAME_DURATION;
        } else {
            frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;
        }

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
            if (status == STATUS_SRTP_NOT_READY_YET) {
                fileIndex = 0;
            } else if (status != STATUS_SUCCESS) {
                DLOGV("writeFrame() failed with 0x%08x", status);
            } else if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame) {
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        if (pSampleConfiguration->audioCodec == RTC_CODEC_AAC) {
            THREAD_SLEEP(SAMPLE_AUDIO_AAC_FRAME_DURATION);
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_ALAW) {
            THREAD_SLEEP(SAMPLE_AUDIO_ALAW_FRAME_DURATION);
        } else if (pSampleConfiguration->audioCodec == RTC_CODEC_MULAW) {
            THREAD_SLEEP(SAMPLE_AUDIO_MULAW_FRAME_DURATION);
        } else {
            THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
        }
    }

CleanUp:

    DLOGI("[KVS Master Local] Closing audio thread");
    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID localSampleReceiveAudioVideoFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;

    CHK_ERR(pSampleStreamingSession != NULL, STATUS_NULL_ARG, "[KVS Master Local] Streaming session is NULL");
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleAudioFrameHandler));

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 frameSize;
    PSampleConfiguration pSampleConfiguration = NULL;
    PCHAR pChannelName;
    PCHAR pUseDefaultStun;
    PCHAR pIceServerUri;
    PCHAR pIceServerUserName;
    PCHAR pIceServerCredential;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    UINT32 logLevel = setLogLevel();

    SET_INSTRUMENTED_ALLOCATORS();

#ifndef _WIN32
    signal(SIGINT, sigintHandler);
#endif

    pChannelName = argc > 1 ? argv[1] : (PCHAR) "KvsWebRtcLocalMaster";
    CHK_STATUS(createSampleConfigurationWithSignaling(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, FALSE, TRUE, logLevel,
                                                      &pSampleConfiguration));

    if (argc > 2) {
        if (!STRCMP(argv[2], AUDIO_CODEC_NAME_OPUS)) {
            audioCodec = RTC_CODEC_OPUS;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_AAC)) {
            audioCodec = RTC_CODEC_AAC;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_ALAW)) {
            audioCodec = RTC_CODEC_ALAW;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_MULAW)) {
            audioCodec = RTC_CODEC_MULAW;
        }
    }

    if (argc > 3) {
        if (!STRCMP(argv[3], VIDEO_CODEC_NAME_H265)) {
            videoCodec = RTC_CODEC_H265;
        } else {
            DLOGI("[KVS Master Local] Defaulting to H264 as the specified codec's sample frames may not be available");
        }
    }

    pSampleConfiguration->audioSource = localSendAudioPackets;
    pSampleConfiguration->videoSource = localSendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = localSampleReceiveAudioVideoFrame;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    pSampleConfiguration->sendSdpAnswer = sampleSendSdpAnswer;
    pSampleConfiguration->sendIceCandidate = sampleSendIceCandidate;

    if (pSampleConfiguration->videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
        pSampleConfiguration->videoRollingBufferDurationSec = 3;
        pSampleConfiguration->videoRollingBufferBitratebps = 1.4 * 1024 * 1024;
    } else if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
        pSampleConfiguration->videoRollingBufferDurationSec = 3;
        pSampleConfiguration->videoRollingBufferBitratebps = 462 * 1024;
    }

    if (pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
        pSampleConfiguration->audioRollingBufferDurationSec = 3;
        pSampleConfiguration->audioRollingBufferBitratebps = 512 * 1024;
    } else if (pSampleConfiguration->audioCodec == RTC_CODEC_AAC) {
        pSampleConfiguration->audioRollingBufferDurationSec = 3;
        pSampleConfiguration->audioRollingBufferBitratebps = 32 * 1024;
    } else if (pSampleConfiguration->audioCodec == RTC_CODEC_ALAW || pSampleConfiguration->audioCodec == RTC_CODEC_MULAW) {
        pSampleConfiguration->audioRollingBufferDurationSec = 3;
        pSampleConfiguration->audioRollingBufferBitratebps = 64 * 1024;
    }

    pUseDefaultStun = GETENV(LOCAL_MASTER_USE_DEFAULT_STUN_ENV);
    if (pUseDefaultStun != NULL && STRCMP(pUseDefaultStun, "0") == 0) {
        pSampleConfiguration->useDefaultStunServer = FALSE;
    }

    pIceServerUri = GETENV(LOCAL_MASTER_ICE_SERVER_URI_ENV);
    if (!IS_EMPTY_STRING(pIceServerUri)) {
        STRNCPY(pSampleConfiguration->customIceServers[0].urls, pIceServerUri, MAX_ICE_CONFIG_URI_LEN);
        pIceServerUserName = GETENV(LOCAL_MASTER_ICE_SERVER_USERNAME_ENV);
        pIceServerCredential = GETENV(LOCAL_MASTER_ICE_SERVER_CREDENTIAL_ENV);
        if (!IS_EMPTY_STRING(pIceServerUserName)) {
            STRNCPY(pSampleConfiguration->customIceServers[0].username, pIceServerUserName, MAX_ICE_CONFIG_USER_NAME_LEN);
        }
        if (!IS_EMPTY_STRING(pIceServerCredential)) {
            STRNCPY(pSampleConfiguration->customIceServers[0].credential, pIceServerCredential, MAX_ICE_CONFIG_CREDENTIAL_LEN);
        }
        pSampleConfiguration->customIceServerCount = 1;
    }

    if (videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-0001.h264"));
    } else if (videoCodec == RTC_CODEC_H265) {
        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, "./h265SampleFrames/frame-0001.h265"));
    }

    if (audioCodec == RTC_CODEC_OPUS) {
        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus"));
    } else if (audioCodec == RTC_CODEC_AAC) {
        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, "./aacSampleFrames/sample-001.aac"));
    } else if (audioCodec == RTC_CODEC_ALAW) {
        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, "./alawSampleFrames/sample-001.pcm"));
    } else if (audioCodec == RTC_CODEC_MULAW) {
        CHK_STATUS(localReadFrameFromDisk(NULL, &frameSize, "./mulawSampleFrames/sample-001.pcm"));
    }

    CHK_STATUS(initKvsWebRtc());
    gSampleConfiguration = pSampleConfiguration;

    DLOGI("[KVS Master Local] Ready. Feed remote offers through masterOnRemoteOffer() and ICE through masterOnRemoteIceCandidate().");

    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS Master Local] Terminated with status code 0x%08x", retStatus);
    }

    if (pSampleConfiguration != NULL) {
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
            THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
        }

        CHK_LOG_ERR(freeSampleConfiguration(&pSampleConfiguration));
    }

    RESET_INSTRUMENTED_ALLOCATORS();
    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}
