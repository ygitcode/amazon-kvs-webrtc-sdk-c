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

    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioVideoFrame;
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
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-0001.h264"));
    } else if (videoCodec == RTC_CODEC_H265) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h265SampleFrames/frame-0001.h265"));
    }

    if (audioCodec == RTC_CODEC_OPUS) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus"));
    } else if (audioCodec == RTC_CODEC_AAC) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./aacSampleFrames/sample-001.aac"));
    } else if (audioCodec == RTC_CODEC_ALAW) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./alawSampleFrames/sample-001.pcm"));
    } else if (audioCodec == RTC_CODEC_MULAW) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./mulawSampleFrames/sample-001.pcm"));
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
