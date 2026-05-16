#include <furi_hal_nfc_i.h>
#include <furi_hal_nfc_pn532.h>
#include <furi.h>

#define TAG "FuriHalNfcEvent"

FuriHalNfcEventInternal* furi_hal_nfc_event = NULL;

void furi_hal_nfc_event_init(void) {
    FURI_LOG_D(TAG, "Initializing NFC event system");
    furi_hal_nfc_event = malloc(sizeof(FuriHalNfcEventInternal));
    furi_check(furi_hal_nfc_event != NULL);
}

FuriHalNfcError furi_hal_nfc_event_start(void) {
    furi_check(furi_hal_nfc_event);

    furi_hal_nfc_event->thread = furi_thread_get_current_id();
    FURI_LOG_D(TAG, "Event system started for thread ID: %p", furi_hal_nfc_event->thread);
    furi_thread_flags_clear(FURI_HAL_NFC_EVENT_INTERNAL_ALL);

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_event_stop(void) {
    /* Safe to call even if event system was never initialised */
    if(!furi_hal_nfc_event) return FuriHalNfcErrorNone;
    FURI_LOG_D(TAG, "Event system stopped for thread ID: %p", furi_hal_nfc_event->thread);

    furi_hal_nfc_event->thread = NULL;

    return FuriHalNfcErrorNone;
}

void furi_hal_nfc_event_set(FuriHalNfcEventInternalType event) {
    furi_check(furi_hal_nfc_event);
    if(event == FuriHalNfcEventInternalTypeAbort) {
        FURI_LOG_D(TAG, "Abort SET: pn532=%d", furi_hal_nfc_pn532_is_active() ? 1 : 0);
    }
    if(furi_hal_nfc_event->thread) {
        furi_thread_flags_set(furi_hal_nfc_event->thread, event);
    }
}

FuriHalNfcError furi_hal_nfc_abort(void) {
    FURI_LOG_D(TAG, "Abort requested from thread %p", furi_thread_get_current_id());
    if(!furi_hal_nfc_event) return FuriHalNfcErrorNone;
    if(!furi_hal_nfc_event->thread) return FuriHalNfcErrorNone;
    furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeAbort);
    return FuriHalNfcErrorNone;
}

FuriHalNfcEvent furi_hal_nfc_wait_event_common(uint32_t timeout_ms) {
    furi_check(furi_hal_nfc_event);
    furi_check(furi_hal_nfc_event->thread);

    if(furi_hal_nfc_pn532_is_active()) {
        return furi_hal_nfc_pn532_wait_event(timeout_ms);
    }

    FuriHalNfcEvent event = 0;
    uint32_t event_timeout = timeout_ms == FURI_HAL_NFC_EVENT_WAIT_FOREVER ? FuriWaitForever :
                                                                             timeout_ms;
    uint32_t event_flag =
        furi_thread_flags_wait(FURI_HAL_NFC_EVENT_INTERNAL_ALL, FuriFlagWaitAny, event_timeout);
    if(event_flag != (unsigned)FuriFlagErrorTimeout) {
        if(event_flag & FuriHalNfcEventInternalTypeTimerFwtExpired) {
            event |= FuriHalNfcEventTimerFwtExpired;
            furi_thread_flags_clear(FuriHalNfcEventInternalTypeTimerFwtExpired);
        }
        if(event_flag & FuriHalNfcEventInternalTypeTimerBlockTxExpired) {
            event |= FuriHalNfcEventTimerBlockTxExpired;
            furi_thread_flags_clear(FuriHalNfcEventInternalTypeTimerBlockTxExpired);
        }
        if(event_flag & FuriHalNfcEventInternalTypeAbort) {
            FURI_LOG_D(
                TAG,
                "Processing Abort Request event from thread %p",
                furi_thread_get_current_id());
            event |= FuriHalNfcEventAbortRequest;
            furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
        }
    } else {
        event = FuriHalNfcEventTimeout;
    }

    return event;
}

bool furi_hal_nfc_event_wait_for_specific_irq(
    const FuriHalSpiBusHandle* handle,
    uint32_t mask,
    uint32_t timeout_ms) {
    furi_check(furi_hal_nfc_event);
    furi_check(furi_hal_nfc_event->thread);

    if(furi_hal_nfc_pn532_is_active()) {
        return true;
    }

    UNUSED(handle);
    UNUSED(mask);
    UNUSED(timeout_ms);
    return true;
}
