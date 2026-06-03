#define LOG_CLASS "WebsocketAnswererSample"

#include <libwebsockets.h>
#include <signal.h>

#include "Samples.h"
#include "../src/source/PeerConnection/jsmn.h"

#define DEFAULT_WEBSOCKET_PORT              8080
#define MAX_WEBSOCKET_SIGNALING_MESSAGE_LEN (MAX_SESSION_DESCRIPTION_INIT_SDP_LEN + 1024)
#define MAX_SIGNALING_WRAPPER_TOKENS        64

typedef struct __OutboundMessage {
    PCHAR payload;
    SIZE_T payloadLen;
    struct __OutboundMessage* pNext;
} OutboundMessage, *POutboundMessage;

typedef struct {
    struct lws_context* pLwsContext;
    UINT32 port;
    CHAR stunServerUrl[MAX_ICE_CONFIG_URI_LEN + 1];
    struct __WebsocketAnswererSession* pActiveSession;
} WebsocketAnswererApp, *PWebsocketAnswererApp;

typedef struct __WebsocketAnswererSession {
    PWebsocketAnswererApp pApp;
    struct lws* pWsi;
    PRtcPeerConnection pPeerConnection;
    PRtcRtpTransceiver pVideoTransceiver;
    PRtcRtpTransceiver pAudioTransceiver;
    RtcSessionDescriptionInit answerSessionDescriptionInit;
    MUTEX sendLock;
    POutboundMessage pSendHead;
    POutboundMessage pSendTail;
    CHAR receiveBuffer[MAX_WEBSOCKET_SIGNALING_MESSAGE_LEN];
    UINT32 receiveOffset;
    BOOL remoteCanTrickleIce;
    BOOL answerSent;
    BOOL offerReceived;
    BOOL loggedFirstAudioFrame;
    BOOL loggedFirstVideoFrame;
} WebsocketAnswererSession, *PWebsocketAnswererSession;

static VOID websocketSigIntHandler(INT32);
static STATUS initializeAnswererPeerConnection(PWebsocketAnswererSession);
static STATUS freeAnswererPeerConnection(PWebsocketAnswererSession);
static STATUS enqueueWrappedSignalingMessage(PWebsocketAnswererSession, PCHAR, PCHAR, UINT32);
static STATUS handleWebsocketOffer(PWebsocketAnswererSession, PCHAR, UINT32);
static STATUS handleWebsocketIceCandidate(PWebsocketAnswererSession, PCHAR, UINT32);
static STATUS processWebsocketMessage(PWebsocketAnswererSession, PCHAR, UINT32);
static BOOL answererHasPendingMessages(PWebsocketAnswererSession);
static VOID answererFreePendingMessages(PWebsocketAnswererSession);

static volatile sig_atomic_t gSampleInterrupted = FALSE;

static VOID websocketSigIntHandler(INT32 sigNum)
{
    UNUSED_PARAM(sigNum);
    gSampleInterrupted = TRUE;
}

static STATUS createWrappedMessage(PCHAR pMessageType, PCHAR pPayload, UINT32 payloadLen, PCHAR* ppWrappedMessage, PUINT32 pWrappedMessageLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pWrappedMessage = NULL;
    UINT32 wrappedMessageLen = 0, messageTypeLen = 0;
    INT32 written = 0;

    CHK(ppWrappedMessage != NULL && pWrappedMessageLen != NULL && pMessageType != NULL && pPayload != NULL, STATUS_NULL_ARG);

    messageTypeLen = (UINT32) STRLEN(pMessageType);
    wrappedMessageLen = payloadLen + messageTypeLen + ARRAY_SIZE("{\"type\":\"\",\"data\":}");
    pWrappedMessage = (PCHAR) MEMCALLOC(wrappedMessageLen, SIZEOF(CHAR));
    CHK(pWrappedMessage != NULL, STATUS_NOT_ENOUGH_MEMORY);

    written = SNPRINTF(pWrappedMessage, wrappedMessageLen, "{\"type\":\"%s\",\"data\":%.*s}", pMessageType, payloadLen, pPayload);
    CHK(written > 0 && written < wrappedMessageLen, STATUS_BUFFER_TOO_SMALL);

    *ppWrappedMessage = pWrappedMessage;
    *pWrappedMessageLen = (UINT32) written;

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        SAFE_MEMFREE(pWrappedMessage);
    }

    return retStatus;
}

static STATUS enqueueOutboundMessage(PWebsocketAnswererSession pSession, PCHAR pMessage, UINT32 messageLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    POutboundMessage pOutboundMessage = NULL;

    CHK(pSession != NULL && pMessage != NULL && messageLen != 0, STATUS_NULL_ARG);

    pOutboundMessage = (POutboundMessage) MEMCALLOC(1, SIZEOF(OutboundMessage));
    CHK(pOutboundMessage != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pOutboundMessage->payload = pMessage;
    pOutboundMessage->payloadLen = messageLen;

    MUTEX_LOCK(pSession->sendLock);
    if (pSession->pSendTail == NULL) {
        pSession->pSendHead = pOutboundMessage;
        pSession->pSendTail = pOutboundMessage;
    } else {
        pSession->pSendTail->pNext = pOutboundMessage;
        pSession->pSendTail = pOutboundMessage;
    }
    MUTEX_UNLOCK(pSession->sendLock);

    if (pSession->pApp != NULL && pSession->pApp->pLwsContext != NULL) {
        lws_cancel_service(pSession->pApp->pLwsContext);
    }

CleanUp:

    if (STATUS_FAILED(retStatus) && pOutboundMessage != NULL) {
        SAFE_MEMFREE(pMessage);
        SAFE_MEMFREE(pOutboundMessage);
    }

    return retStatus;
}

static POutboundMessage dequeueOutboundMessage(PWebsocketAnswererSession pSession)
{
    POutboundMessage pOutboundMessage = NULL;

    if (pSession == NULL) {
        return NULL;
    }

    MUTEX_LOCK(pSession->sendLock);
    pOutboundMessage = pSession->pSendHead;
    if (pOutboundMessage != NULL) {
        pSession->pSendHead = pOutboundMessage->pNext;
        if (pSession->pSendHead == NULL) {
            pSession->pSendTail = NULL;
        }
        pOutboundMessage->pNext = NULL;
    }
    MUTEX_UNLOCK(pSession->sendLock);

    return pOutboundMessage;
}

static BOOL answererHasPendingMessages(PWebsocketAnswererSession pSession)
{
    BOOL hasPendingMessages = FALSE;

    if (pSession == NULL) {
        return FALSE;
    }

    MUTEX_LOCK(pSession->sendLock);
    hasPendingMessages = pSession->pSendHead != NULL;
    MUTEX_UNLOCK(pSession->sendLock);

    return hasPendingMessages;
}

static VOID answererFreePendingMessages(PWebsocketAnswererSession pSession)
{
    POutboundMessage pOutboundMessage = NULL;

    if (pSession == NULL || !IS_VALID_MUTEX_VALUE(pSession->sendLock)) {
        return;
    }

    while ((pOutboundMessage = dequeueOutboundMessage(pSession)) != NULL) {
        SAFE_MEMFREE(pOutboundMessage->payload);
        SAFE_MEMFREE(pOutboundMessage);
    }
}

static STATUS enqueueWrappedSignalingMessage(PWebsocketAnswererSession pSession, PCHAR pMessageType, PCHAR pPayload, UINT32 payloadLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pWrappedMessage = NULL;
    UINT32 wrappedMessageLen = 0;

    CHK_STATUS(createWrappedMessage(pMessageType, pPayload, payloadLen, &pWrappedMessage, &wrappedMessageLen));
    CHK_STATUS(enqueueOutboundMessage(pSession, pWrappedMessage, wrappedMessageLen));

CleanUp:

    return retStatus;
}

static STATUS enqueueAnswerMessage(PWebsocketAnswererSession pSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 answerJsonLen = MAX_SIGNALING_MESSAGE_LEN;
    CHAR answerJson[MAX_SIGNALING_MESSAGE_LEN];

    CHK(pSession != NULL, STATUS_NULL_ARG);

    MEMSET(answerJson, 0x00, SIZEOF(answerJson));
    CHK_STATUS(serializeSessionDescriptionInit(&pSession->answerSessionDescriptionInit, answerJson, &answerJsonLen));
    CHK_STATUS(enqueueWrappedSignalingMessage(pSession, (PCHAR) "sdp", answerJson, (UINT32) STRLEN(answerJson)));
    pSession->answerSent = TRUE;

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

static VOID onAnswererConnectionStateChange(UINT64 customData, RTC_PEER_CONNECTION_STATE newState)
{
    PWebsocketAnswererSession pSession = (PWebsocketAnswererSession) customData;
    UNUSED_PARAM(pSession);
    DLOGI("[Websocket Answerer] Peer connection state changed to %u", newState);
}

static VOID onAnswererVideoFrame(UINT64 customData, PFrame pFrame)
{
    PWebsocketAnswererSession pSession = (PWebsocketAnswererSession) customData;

    if (pSession != NULL && pFrame != NULL && !pSession->loggedFirstVideoFrame) {
        pSession->loggedFirstVideoFrame = TRUE;
        DLOGI("[Websocket Answerer] First video frame received (%u bytes)", pFrame->size);
    }
}

static VOID onAnswererAudioFrame(UINT64 customData, PFrame pFrame)
{
    PWebsocketAnswererSession pSession = (PWebsocketAnswererSession) customData;

    if (pSession != NULL && pFrame != NULL && !pSession->loggedFirstAudioFrame) {
        pSession->loggedFirstAudioFrame = TRUE;
        DLOGI("[Websocket Answerer] First audio frame received (%u bytes)", pFrame->size);
    }
}

static VOID onAnswererIceCandidate(UINT64 customData, PCHAR candidateJson)
{
    STATUS retStatus = STATUS_SUCCESS;
    PWebsocketAnswererSession pSession = (PWebsocketAnswererSession) customData;

    CHK(pSession != NULL, STATUS_NULL_ARG);

    if (candidateJson == NULL) {
        DLOGD("[Websocket Answerer] ICE candidate gathering finished");
        if (pSession->offerReceived && !pSession->remoteCanTrickleIce && !pSession->answerSent) {
            CHK_STATUS(createAnswer(pSession->pPeerConnection, &pSession->answerSessionDescriptionInit));
            CHK_STATUS(enqueueAnswerMessage(pSession));
        }
    } else if (pSession->remoteCanTrickleIce && pSession->answerSent) {
        CHK_STATUS(enqueueWrappedSignalingMessage(pSession, (PCHAR) "ice", candidateJson, (UINT32) STRLEN(candidateJson)));
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
}

static STATUS initializeAnswererPeerConnection(PWebsocketAnswererSession pSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcConfiguration configuration;
    RtcMediaStreamTrack videoTrack, audioTrack;
    RtcRtpTransceiverInit transceiverInit;

    CHK(pSession != NULL && pSession->pApp != NULL, STATUS_NULL_ARG);

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;

    if (!IS_EMPTY_STRING(pSession->pApp->stunServerUrl)) {
        STRNCPY(configuration.iceServers[0].urls, pSession->pApp->stunServerUrl, MAX_ICE_CONFIG_URI_LEN);
    }

    CHK_STATUS(createPeerConnection(&configuration, &pSession->pPeerConnection));
    CHK_STATUS(peerConnectionOnIceCandidate(pSession->pPeerConnection, (UINT64) pSession, onAnswererIceCandidate));
    CHK_STATUS(peerConnectionOnConnectionStateChange(pSession->pPeerConnection, (UINT64) pSession, onAnswererConnectionStateChange));

    CHK_STATUS(addSupportedCodec(pSession->pPeerConnection, RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE));
    CHK_STATUS(addSupportedCodec(pSession->pPeerConnection, RTC_CODEC_OPUS));

    MEMSET(&videoTrack, 0x00, SIZEOF(RtcMediaStreamTrack));
    MEMSET(&audioTrack, 0x00, SIZEOF(RtcMediaStreamTrack));
    MEMSET(&transceiverInit, 0x00, SIZEOF(RtcRtpTransceiverInit));

    transceiverInit.direction = RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY;

    videoTrack.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    videoTrack.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    STRCPY(videoTrack.streamId, "websocketVideoStream");
    STRCPY(videoTrack.trackId, "websocketVideoTrack");
    CHK_STATUS(addTransceiver(pSession->pPeerConnection, &videoTrack, &transceiverInit, &pSession->pVideoTransceiver));
    CHK_STATUS(transceiverOnFrame(pSession->pVideoTransceiver, (UINT64) pSession, onAnswererVideoFrame));

    audioTrack.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
    audioTrack.codec = RTC_CODEC_OPUS;
    STRCPY(audioTrack.streamId, "websocketAudioStream");
    STRCPY(audioTrack.trackId, "websocketAudioTrack");
    CHK_STATUS(addTransceiver(pSession->pPeerConnection, &audioTrack, &transceiverInit, &pSession->pAudioTransceiver));
    CHK_STATUS(transceiverOnFrame(pSession->pAudioTransceiver, (UINT64) pSession, onAnswererAudioFrame));

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        CHK_LOG_ERR(retStatus);
        freeAnswererPeerConnection(pSession);
    }

    return retStatus;
}

static STATUS freeAnswererPeerConnection(PWebsocketAnswererSession pSession)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pSession != NULL, STATUS_NULL_ARG);

    if (pSession->pPeerConnection != NULL) {
        closePeerConnection(pSession->pPeerConnection);
        CHK_STATUS(freePeerConnection(&pSession->pPeerConnection));
    }

    pSession->pPeerConnection = NULL;
    pSession->pVideoTransceiver = NULL;
    pSession->pAudioTransceiver = NULL;
    pSession->offerReceived = FALSE;
    pSession->answerSent = FALSE;
    pSession->remoteCanTrickleIce = FALSE;
    pSession->loggedFirstAudioFrame = FALSE;
    pSession->loggedFirstVideoFrame = FALSE;
    MEMSET(&pSession->answerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

static STATUS handleWebsocketOffer(PWebsocketAnswererSession pSession, PCHAR pOfferJson, UINT32 offerJsonLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit remoteDescription;
    NullableBool canTrickle;

    CHK(pSession != NULL && pOfferJson != NULL, STATUS_NULL_ARG);
    CHK(!pSession->offerReceived, STATUS_INVALID_OPERATION);

    MEMSET(&remoteDescription, 0x00, SIZEOF(RtcSessionDescriptionInit));
    MEMSET(&pSession->answerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    CHK_STATUS(deserializeSessionDescriptionInit(pOfferJson, offerJsonLen, &remoteDescription));
    CHK(remoteDescription.type == SDP_TYPE_OFFER, STATUS_INVALID_ARG);
    CHK_STATUS(setRemoteDescription(pSession->pPeerConnection, &remoteDescription));

    canTrickle = canTrickleIceCandidates(pSession->pPeerConnection);
    CHK(!NULLABLE_CHECK_EMPTY(canTrickle), STATUS_INTERNAL_ERROR);
    pSession->remoteCanTrickleIce = canTrickle.value;

    CHK_STATUS(setLocalDescription(pSession->pPeerConnection, &pSession->answerSessionDescriptionInit));
    pSession->offerReceived = TRUE;

    if (pSession->remoteCanTrickleIce) {
        CHK_STATUS(createAnswer(pSession->pPeerConnection, &pSession->answerSessionDescriptionInit));
        CHK_STATUS(enqueueAnswerMessage(pSession));
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

static STATUS handleWebsocketIceCandidate(PWebsocketAnswererSession pSession, PCHAR pIceJson, UINT32 iceJsonLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcIceCandidateInit iceCandidateInit;

    CHK(pSession != NULL && pIceJson != NULL, STATUS_NULL_ARG);
    CHK(pSession->offerReceived, STATUS_INVALID_OPERATION);

    MEMSET(&iceCandidateInit, 0x00, SIZEOF(RtcIceCandidateInit));
    CHK_STATUS(deserializeRtcIceCandidateInit(pIceJson, iceJsonLen, &iceCandidateInit));
    CHK_STATUS(addIceCandidate(pSession->pPeerConnection, iceCandidateInit.candidate));

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

static STATUS processWebsocketMessage(PWebsocketAnswererSession pSession, PCHAR pMessage, UINT32 messageLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    jsmn_parser parser;
    jsmntok_t tokens[MAX_SIGNALING_WRAPPER_TOKENS];
    INT32 tokenCount, i;
    PCHAR pMessageType = NULL;
    UINT32 messageTypeLen = 0, dataLen = 0;
    PCHAR pData = NULL;

    CHK(pSession != NULL && pMessage != NULL, STATUS_NULL_ARG);

    jsmn_init(&parser);
    tokenCount = jsmn_parse(&parser, pMessage, messageLen, tokens, ARRAY_SIZE(tokens));
    CHK(tokenCount > 1, STATUS_INVALID_ARG);
    CHK(tokens[0].type == JSMN_OBJECT, STATUS_INVALID_ARG);

    for (i = 1; i + 1 < tokenCount; i += 2) {
        if (tokens[i].type != JSMN_STRING) {
            continue;
        }

        if ((UINT32) (tokens[i].end - tokens[i].start) == STRLEN("type") &&
            STRNCMP(pMessage + tokens[i].start, "type", STRLEN("type")) == 0) {
            pMessageType = pMessage + tokens[i + 1].start;
            messageTypeLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
        } else if ((UINT32) (tokens[i].end - tokens[i].start) == STRLEN("data") &&
                   STRNCMP(pMessage + tokens[i].start, "data", STRLEN("data")) == 0) {
            pData = pMessage + tokens[i + 1].start;
            dataLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
        }
    }

    CHK(pMessageType != NULL && pData != NULL, STATUS_INVALID_ARG);

    if (messageTypeLen == STRLEN("sdp") && STRNCMP(pMessageType, "sdp", messageTypeLen) == 0) {
        CHK_STATUS(handleWebsocketOffer(pSession, pData, dataLen));
    } else if (messageTypeLen == STRLEN("ice") && STRNCMP(pMessageType, "ice", messageTypeLen) == 0) {
        CHK_STATUS(handleWebsocketIceCandidate(pSession, pData, dataLen));
    } else {
        CHK(FALSE, STATUS_INVALID_ARG);
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

static INT32 websocketAnswererCallback(struct lws* wsi, enum lws_callback_reasons reason, PVOID user, PVOID in, SIZE_T len)
{
    STATUS retStatus = STATUS_SUCCESS;
    PWebsocketAnswererSession pSession = (PWebsocketAnswererSession) user;
    PWebsocketAnswererApp pApp = (PWebsocketAnswererApp) lws_context_user(lws_get_context(wsi));
    POutboundMessage pOutboundMessage = NULL;
    PBYTE pWriteBuffer = NULL;
    INT32 written = 0;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            if (pApp->pActiveSession != NULL) {
                DLOGW("[Websocket Answerer] Only one websocket peer is supported at a time");
                return -1;
            }

            MEMSET(pSession, 0x00, SIZEOF(WebsocketAnswererSession));
            pSession->pApp = pApp;
            pSession->pWsi = wsi;
            pSession->sendLock = MUTEX_CREATE(FALSE);
            if (!IS_VALID_MUTEX_VALUE(pSession->sendLock)) {
                return -1;
            }

            pApp->pActiveSession = pSession;
            retStatus = initializeAnswererPeerConnection(pSession);
            if (STATUS_FAILED(retStatus)) {
                return -1;
            }

            DLOGI("[Websocket Answerer] Browser websocket connected");
            break;

        case LWS_CALLBACK_RECEIVE:
            CHK(pSession != NULL, STATUS_NULL_ARG);
            CHK(pSession->receiveOffset + len < ARRAY_SIZE(pSession->receiveBuffer) - 1, STATUS_BUFFER_TOO_SMALL);

            MEMCPY(pSession->receiveBuffer + pSession->receiveOffset, in, len);
            pSession->receiveOffset += (UINT32) len;

            if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                pSession->receiveBuffer[pSession->receiveOffset] = '\0';
                retStatus = processWebsocketMessage(pSession, pSession->receiveBuffer, pSession->receiveOffset);
                pSession->receiveOffset = 0;
                if (STATUS_FAILED(retStatus)) {
                    return -1;
                }
            }
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            CHK(pSession != NULL, STATUS_NULL_ARG);

            pOutboundMessage = dequeueOutboundMessage(pSession);
            if (pOutboundMessage == NULL) {
                break;
            }

            pWriteBuffer = (PBYTE) MEMCALLOC(LWS_PRE + pOutboundMessage->payloadLen, SIZEOF(BYTE));
            CHK(pWriteBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);

            MEMCPY(pWriteBuffer + LWS_PRE, pOutboundMessage->payload, pOutboundMessage->payloadLen);
            written = lws_write(wsi, pWriteBuffer + LWS_PRE, pOutboundMessage->payloadLen, LWS_WRITE_TEXT);
            CHK(written == (INT32) pOutboundMessage->payloadLen, STATUS_INVALID_OPERATION);

            SAFE_MEMFREE(pWriteBuffer);
            SAFE_MEMFREE(pOutboundMessage->payload);
            SAFE_MEMFREE(pOutboundMessage);

            if (answererHasPendingMessages(pSession)) {
                lws_callback_on_writable(wsi);
            }
            break;

        case LWS_CALLBACK_CLOSED:
            if (pApp->pActiveSession == pSession) {
                pApp->pActiveSession = NULL;
            }

            if (pSession != NULL) {
                answererFreePendingMessages(pSession);
                freeAnswererPeerConnection(pSession);
                if (IS_VALID_MUTEX_VALUE(pSession->sendLock)) {
                    MUTEX_FREE(pSession->sendLock);
                    pSession->sendLock = INVALID_MUTEX_VALUE;
                }
                pSession->pWsi = NULL;
                pSession->receiveOffset = 0;
            }
            DLOGI("[Websocket Answerer] Browser websocket disconnected");
            break;

        default:
            break;
    }

    return 0;

CleanUp:

    SAFE_MEMFREE(pWriteBuffer);
    if (pOutboundMessage != NULL) {
        SAFE_MEMFREE(pOutboundMessage->payload);
        SAFE_MEMFREE(pOutboundMessage);
    }

    CHK_LOG_ERR(retStatus);
    return -1;
}

static struct lws_protocols gWebsocketAnswererProtocols[] = {
    {
        "kvs-webrtc-answerer",
        websocketAnswererCallback,
        SIZEOF(WebsocketAnswererSession),
        MAX_WEBSOCKET_SIGNALING_MESSAGE_LEN,
    },
    {NULL, NULL, 0, 0},
};

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 port = DEFAULT_WEBSOCKET_PORT;
    struct lws_context_creation_info creationInfo;
    WebsocketAnswererApp app;
    INT32 logLevel = LOG_LEVEL_WARN;
    PCHAR pLogLevel = NULL;

    MEMSET(&app, 0x00, SIZEOF(WebsocketAnswererApp));
    MEMSET(&creationInfo, 0x00, SIZEOF(creationInfo));

#ifndef _WIN32
    signal(SIGINT, websocketSigIntHandler);
#endif

    if (argc > 1) {
        if (STATUS_SUCCESS != STRTOUI32(argv[1], NULL, 10, &port) || port == 0) {
            port = DEFAULT_WEBSOCKET_PORT;
        }
    }

    if (argc > 2) {
        STRNCPY(app.stunServerUrl, argv[2], MAX_ICE_CONFIG_URI_LEN);
    }

    pLogLevel = GETENV(DEBUG_LOG_LEVEL_ENV_VAR);
    if (pLogLevel != NULL && STATUS_SUCCESS != STRTOUI32(pLogLevel, NULL, 10, (PUINT32) &logLevel)) {
        logLevel = LOG_LEVEL_WARN;
    }
    SET_LOGGER_LOG_LEVEL(logLevel);

    app.port = port;

    CHK_STATUS(initKvsWebRtc());

    creationInfo.port = app.port;
    creationInfo.protocols = gWebsocketAnswererProtocols;
    creationInfo.gid = -1;
    creationInfo.uid = -1;
    creationInfo.user = &app;

    app.pLwsContext = lws_create_context(&creationInfo);
    CHK(app.pLwsContext != NULL, STATUS_NULL_ARG);

    DLOGI("[Websocket Answerer] Listening on ws://0.0.0.0:%u", app.port);
    if (!IS_EMPTY_STRING(app.stunServerUrl)) {
        DLOGI("[Websocket Answerer] Using STUN server: %s", app.stunServerUrl);
    }

    while (!gSampleInterrupted) {
        lws_service(app.pLwsContext, 100);
        if (app.pActiveSession != NULL && app.pActiveSession->pWsi != NULL && answererHasPendingMessages(app.pActiveSession)) {
            lws_callback_on_writable(app.pActiveSession->pWsi);
        }
    }

CleanUp:

    if (app.pLwsContext != NULL) {
        lws_context_destroy(app.pLwsContext);
    }

    deinitKvsWebRtc();

    if (STATUS_FAILED(retStatus)) {
        DLOGE("[Websocket Answerer] Terminated with status 0x%08x", retStatus);
    }

    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}
