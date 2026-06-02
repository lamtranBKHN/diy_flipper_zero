#pragma once

typedef enum {
    // Reserve first 100 events for button types and indexes, starting from 0
    NfcCustomEventReserved = 100,

    // Mf classic dict attack events
    NfcCustomEventDictAttackComplete,
    NfcCustomEventDictAttackSkip,
    NfcCustomEventDictAttackDataUpdate,

    NfcCustomEventCardDetected,
    NfcCustomEventCardLost,

    NfcCustomEventViewExit,
    NfcCustomEventRetry,
    NfcCustomEventWorkerExit,
    NfcCustomEventWorkerUpdate,
    NfcCustomEventWrongCard,
    NfcCustomEventTimerExpired,
    NfcCustomEventByteInputDone,
    NfcCustomEventTextInputDone,
    NfcCustomEventDictAttackDone,

    NfcCustomEventRpcLoadFile,
    NfcCustomEventRpcExit,
    NfcCustomEventRpcSessionClose,

    NfcCustomEventPollerSuccess,
    NfcCustomEventPollerIncomplete,
    NfcCustomEventPollerReadFailed, // Deferred: read fail → dict attack (avoids stack overflow)
    NfcCustomEventPollerFailure,

    NfcCustomEventListenerUpdate,

    NfcCustomEventEmulationTimeExpired,
} NfcCustomEvent;
