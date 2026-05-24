#ifndef FW_CFG_unit_tests

#include "nfc.h"

#include <furi_hal_nfc.h>
#include <furi/furi.h>
#include <furi_hal.h>

#define TAG "Nfc"

#define NFC_MAX_BUFFER_SIZE (288)

#define NFC_FELICA_LISTENER_RESPONSE_TIME_A_FC (512 * 64)
#define NFC_FELICA_LISTENER_RESPONSE_TIME_B_FC (256 * 64)

typedef enum {
    NfcStateIdle,
    NfcStateRunning,
} NfcState;

typedef enum {
    NfcPollerStateStart,
    NfcPollerStateReady,
    NfcPollerStateReset,
    NfcPollerStateStop,

    NfcPollerStateNum,
} NfcPollerState;

typedef enum {
    NfcCommStateIdle,
    NfcCommStateWaitBlockTxTimer,
    NfcCommStateReadyTx,
    NfcCommStateWaitTxEnd,
    NfcCommStateWaitRxStart,
    NfcCommStateWaitRxEnd,
    NfcCommStateFailed,
} NfcCommState;

typedef enum {
    NfcConfigurationStateIdle,
    NfcConfigurationStateDone,
} NfcConfigurationState;

struct Nfc {
    NfcState state;
    NfcPollerState poller_state;
    NfcCommState comm_state;
    NfcConfigurationState config_state;
    NfcMode mode;

    uint32_t fdt_listen_fc;
    uint32_t mask_rx_time_fc;
    uint32_t fdt_poll_fc;
    uint32_t fdt_poll_poll_us;
    uint32_t guard_time_us;
    NfcEventCallback callback;
    void* context;

    uint8_t tx_buffer[NFC_MAX_BUFFER_SIZE];
    size_t tx_bits;
    uint8_t rx_buffer[NFC_MAX_BUFFER_SIZE];
    size_t rx_bits;

    FuriThread* worker_thread;
    FuriSemaphore* poller_ready_sem;
};

typedef bool (*NfcWorkerPollerStateHandler)(Nfc* instance);

static const FuriHalNfcTech nfc_tech_table[NfcModeNum][NfcTechNum] = {
    [NfcModePoller] =
        {
            [NfcTechIso14443a] = FuriHalNfcTechIso14443a,
            [NfcTechIso14443b] = FuriHalNfcTechIso14443b,
            [NfcTechIso15693] = FuriHalNfcTechIso15693,
            [NfcTechFelica] = FuriHalNfcTechFelica,
        },
    [NfcModeListener] =
        {
            [NfcTechIso14443a] = FuriHalNfcTechIso14443a,
            [NfcTechIso14443b] = FuriHalNfcTechInvalid,
            [NfcTechIso15693] = FuriHalNfcTechIso15693,
            [NfcTechFelica] = FuriHalNfcTechFelica,
        },
};

static NfcError nfc_process_hal_error(FuriHalNfcError error) {
    NfcError ret = NfcErrorNone;

    switch(error) {
    case FuriHalNfcErrorNone:
        ret = NfcErrorNone;
        break;
    case FuriHalNfcErrorIncompleteFrame:
        ret = NfcErrorIncompleteFrame;
        break;
    case FuriHalNfcErrorDataFormat:
        ret = NfcErrorDataFormat;
        break;
    case FuriHalNfcErrorCommunicationTimeout:
        ret = NfcErrorTimeout;
        break;
    case FuriHalNfcErrorBufferOverflow:
        ret = NfcErrorBufferOverflow;
        break;

    default:
        ret = NfcErrorInternal;
    }

    return ret;
}

static int32_t nfc_worker_listener(void* context) {
    furi_assert(context);

    Nfc* instance = context;
    furi_assert(instance->callback);
    furi_assert(instance->config_state == NfcConfigurationStateDone);

    instance->state = NfcStateRunning;

    furi_hal_nfc_event_start();

    NfcEventData event_data = {};
    event_data.buffer = bit_buffer_alloc(NFC_MAX_BUFFER_SIZE);
    NfcEvent nfc_event = {.data = event_data};
    NfcCommand command = NfcCommandContinue;

    while(true) {
        FuriHalNfcEvent event = furi_hal_nfc_listener_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        if(event & FuriHalNfcEventAbortRequest) {
            nfc_event.type = NfcEventTypeUserAbort;
            instance->callback(nfc_event, instance->context);
            break;
        }
        if(event & FuriHalNfcEventFieldOn) {
            nfc_event.type = NfcEventTypeFieldOn;
            instance->callback(nfc_event, instance->context);
        }
        if(event & FuriHalNfcEventFieldOff) {
            nfc_event.type = NfcEventTypeFieldOff;
            instance->callback(nfc_event, instance->context);
            furi_hal_nfc_listener_idle();
        }
        if(event & FuriHalNfcEventListenerActive) {
            nfc_event.type = NfcEventTypeListenerActivated;
            instance->callback(nfc_event, instance->context);
        }
        if(event & FuriHalNfcEventRxEnd) {
            furi_hal_nfc_timer_block_tx_start(instance->fdt_listen_fc);

            nfc_event.type = NfcEventTypeRxEnd;
            furi_hal_nfc_listener_rx(
                instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
            bit_buffer_copy_bits(event_data.buffer, instance->rx_buffer, instance->rx_bits);
            command = instance->callback(nfc_event, instance->context);
            if(command == NfcCommandStop) {
                break;
            } else if(command == NfcCommandReset) {
                furi_hal_nfc_listener_enable_rx();
            } else if(command == NfcCommandSleep) {
                furi_hal_nfc_listener_idle();
            }
        }
    }

    furi_hal_nfc_reset_mode();
    instance->config_state = NfcConfigurationStateIdle;

    bit_buffer_free(event_data.buffer);
    furi_hal_nfc_low_power_mode_start();
    return 0;
}

/* Forward declaration: defined below nfc_worker_poller_start_handler but called from it */
bool nfc_worker_poller_stop_handler(Nfc* instance);

bool nfc_worker_poller_start_handler(Nfc* instance) {
    furi_hal_nfc_poller_field_on();
    if(instance->guard_time_us) {
        furi_hal_nfc_timer_block_tx_start_us(instance->guard_time_us);
        FuriHalNfcEvent event = furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        if(event & FuriHalNfcEventAbortRequest) {
            instance->poller_state = NfcPollerStateStop;
            /* Release the semaphore so nfc_poller_detect() doesn't block
             * for 3 seconds waiting for the ready handler to release it.
             * The ready handler will NOT fire because we're exiting early. */
            furi_semaphore_release(instance->poller_ready_sem);
            return nfc_worker_poller_stop_handler(instance);
        }
        furi_assert(event & FuriHalNfcEventTimerBlockTxExpired);
    }
    instance->poller_state = NfcPollerStateReady;

    return false;
}

bool nfc_worker_poller_ready_handler(Nfc* instance) {
    NfcCommand command = NfcCommandContinue;

    NfcEvent event = {.type = NfcEventTypePollerReady};
    command = instance->callback(event, instance->context);
    if(command == NfcCommandReset) {
        instance->poller_state = NfcPollerStateReset;
    } else if(command == NfcCommandStop) {
        instance->poller_state = NfcPollerStateStop;
    }

    furi_semaphore_release(instance->poller_ready_sem);

    return false;
}

bool nfc_worker_poller_reset_handler(Nfc* instance) {
    furi_hal_nfc_low_power_mode_start();
    furi_delay_ms(10);
    furi_hal_nfc_low_power_mode_stop();
    instance->poller_state = NfcPollerStateStart;

    return false;
}

bool nfc_worker_poller_stop_handler(Nfc* instance) {
    furi_hal_nfc_reset_mode();
    instance->config_state = NfcConfigurationStateIdle;

    furi_hal_nfc_low_power_mode_start();
    // Wait after field is off some time to reset tag power
    furi_delay_ms(10);
    instance->poller_state = NfcPollerStateStart;

    return true;
}

static const NfcWorkerPollerStateHandler nfc_worker_poller_state_handlers[NfcPollerStateNum] = {
    [NfcPollerStateStart] = nfc_worker_poller_start_handler,
    [NfcPollerStateReady] = nfc_worker_poller_ready_handler,
    [NfcPollerStateReset] = nfc_worker_poller_reset_handler,
    [NfcPollerStateStop] = nfc_worker_poller_stop_handler,
};

static int32_t nfc_worker_poller(void* context) {
    furi_assert(context);

    Nfc* instance = context;
    furi_assert(instance->callback);
    instance->state = NfcStateRunning;
    instance->poller_state = NfcPollerStateStart;

    furi_hal_nfc_event_start();

    bool exit = false;
    while(!exit) {
        exit = nfc_worker_poller_state_handlers[instance->poller_state](instance);
    }

    furi_hal_nfc_event_stop();
    return 0;
}

Nfc* nfc_alloc(void) {
    furi_hal_nfc_acquire();

    Nfc* instance = malloc(sizeof(Nfc));
    instance->state = NfcStateIdle;
    instance->comm_state = NfcCommStateIdle;
    instance->config_state = NfcConfigurationStateIdle;

    instance->worker_thread = furi_thread_alloc();
    furi_thread_set_name(instance->worker_thread, "NfcWorker");
    furi_thread_set_context(instance->worker_thread, instance);
    furi_thread_set_priority(instance->worker_thread, FuriThreadPriorityHighest);
    furi_thread_set_stack_size(instance->worker_thread, 12 * 1024);
    instance->poller_ready_sem = furi_semaphore_alloc(1, 0);
    furi_hal_nfc_release();

    return instance;
}

void nfc_free(Nfc* instance) {
    furi_check(instance);
    // nfc_stop() should be called before nfc_free() to ensure the worker
    // thread is joined.  Defensively check and clean up if not — freeing
    // a running FuriThread is undefined behaviour.
    if(instance->state == NfcStateRunning) {
        FURI_LOG_W(TAG, "nfc_free: worker still running, forcing stop");
        furi_hal_nfc_abort();
        furi_thread_join(instance->worker_thread);
    }

    // The NFC HAL mutex is acquired and released entirely within nfc_alloc().
    // nfc_free() does not interact with the HAL — it only frees the Nfc struct
    // and its members.
    furi_thread_free(instance->worker_thread);
    furi_semaphore_free(instance->poller_ready_sem);
    free(instance);
}

void nfc_config(Nfc* instance, NfcMode mode, NfcTech tech) {
    furi_check(instance);
    furi_check(mode < NfcModeNum);
    furi_check(tech < NfcTechNum);
    furi_check(instance->config_state == NfcConfigurationStateIdle);

    FuriHalNfcTech hal_tech = nfc_tech_table[mode][tech];
    if(hal_tech == FuriHalNfcTechInvalid) {
        /* On the DIY board (PN532) some mode/tech combos are unsupported but
         * are reachable via auto-detection paths (e.g. listener mode for
         * non-A protocols).  A panic here would kill the app on bad cards;
         * log + return cleanly so the caller can recover. */
        FURI_LOG_E(
            TAG, "nfc_config: unsupported mode=%d tech=%d on this backend", mode, tech);
        instance->config_state = NfcConfigurationStateIdle;
        return;
    }
    FuriHalNfcMode hal_mode = (mode == NfcModePoller) ? FuriHalNfcModePoller :
                                                        FuriHalNfcModeListener;
    furi_hal_nfc_low_power_mode_stop();
    FuriHalNfcError error = furi_hal_nfc_set_mode(hal_mode, hal_tech);
    if(error != FuriHalNfcErrorNone) {
        furi_hal_nfc_low_power_mode_start();
        instance->config_state = NfcConfigurationStateIdle;
        return;
    }

    instance->mode = mode;
    instance->config_state = NfcConfigurationStateDone;
}

void nfc_set_fdt_poll_fc(Nfc* instance, uint32_t fdt_poll_fc) {
    furi_check(instance);
    instance->fdt_poll_fc = fdt_poll_fc;
}

void nfc_set_fdt_listen_fc(Nfc* instance, uint32_t fdt_listen_fc) {
    furi_check(instance);
    instance->fdt_listen_fc = fdt_listen_fc;
}

void nfc_set_fdt_poll_poll_us(Nfc* instance, uint32_t fdt_poll_poll_us) {
    furi_check(instance);
    instance->fdt_poll_poll_us = fdt_poll_poll_us;
}

void nfc_set_guard_time_us(Nfc* instance, uint32_t guard_time_us) {
    furi_check(instance);
    instance->guard_time_us = guard_time_us;
}

void nfc_set_mask_receive_time_fc(Nfc* instance, uint32_t mask_rx_time_fc) {
    furi_check(instance);
    instance->mask_rx_time_fc = mask_rx_time_fc;
}

void nfc_start(Nfc* instance, NfcEventCallback callback, void* context) {
    furi_check(instance);
    furi_check(instance->state == NfcStateIdle);
    furi_check(instance->worker_thread);
    furi_check(callback);
    furi_check(instance->config_state == NfcConfigurationStateDone);

    instance->callback = callback;
    instance->context = context;
    if(instance->mode == NfcModePoller) {
        furi_thread_set_callback(instance->worker_thread, nfc_worker_poller);
    } else {
        furi_thread_set_callback(instance->worker_thread, nfc_worker_listener);
    }
    instance->comm_state = NfcCommStateIdle;
    furi_thread_start(instance->worker_thread);
}

void nfc_stop(Nfc* instance) {
    furi_check(instance);
    furi_check(instance->state == NfcStateRunning);

    furi_hal_nfc_abort();
    furi_thread_join(instance->worker_thread);

    instance->state = NfcStateIdle;
}

FuriStatus nfc_wait_for_poller_ready(Nfc* instance, uint32_t timeout_ms) {
    /* Drain stale ready signal from previous poller_start() cycle.
     * nfc_poller_start() releases the semaphore in the worker's ready
     * handler but the caller never waits for it — the count leaks into
     * the next nfc_poller_detect() call, causing immediate return and
     * spurious abort.  Non-blocking acquire removes the stale count. */
    furi_semaphore_acquire(instance->poller_ready_sem, 0);
    return furi_semaphore_acquire(instance->poller_ready_sem, timeout_ms);
}

NfcError nfc_listener_tx(Nfc* instance, const BitBuffer* tx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);

    NfcError ret = NfcErrorNone;
    furi_hal_nfc_acquire();

    uint32_t timeout = furi_get_tick() + 3000; // 3 second timeout
    while(furi_hal_nfc_timer_block_tx_is_running()) {
        if(furi_get_tick() > timeout) break;
        furi_thread_yield();
    }

    FuriHalNfcError error =
        furi_hal_nfc_listener_tx(bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
    if(error != FuriHalNfcErrorNone) {
        FURI_LOG_D(TAG, "Failed in listener TX");
        ret = nfc_process_hal_error(error);
    }
    furi_hal_nfc_release();
    return ret;
}

static NfcError nfc_poller_trx_state_machine(Nfc* instance, uint32_t fwt_fc) {
    FuriHalNfcEvent event = 0;
    NfcError error = NfcErrorNone;
    while(true) {
        event = furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        FURI_LOG_D(TAG, "Event received: %i", event);

        if(event & FuriHalNfcEventTimerBlockTxExpired) {
            FURI_LOG_D(TAG, "BlockTx timer expired");
            if(instance->comm_state == NfcCommStateWaitBlockTxTimer) {
                FURI_LOG_D(TAG, "Transition: WaitBlockTxTimer -> ReadyTx");
                instance->comm_state = NfcCommStateReadyTx;
            }
        }
        if(event & FuriHalNfcEventTimeout) {
            if(instance->comm_state == NfcCommStateWaitTxEnd) {
                error = NfcErrorTimeout;
                FURI_LOG_D(TAG, "Timeout waiting for TxEnd");
                furi_hal_nfc_timer_fwt_stop();
                furi_hal_nfc_timer_block_tx_stop();
                break;
            }
        }
        if(event & FuriHalNfcEventTxEnd) {
            FURI_LOG_D(TAG, "TxEnd event");
            if(instance->comm_state == NfcCommStateWaitTxEnd) {
                FURI_LOG_D(TAG, "Transition: WaitTxEnd -> WaitRxStart");
                if(fwt_fc) {
                    furi_hal_nfc_timer_fwt_start(fwt_fc);
                    FURI_LOG_D(TAG, "FWT timer started: %lu", fwt_fc);
                }
                furi_hal_nfc_timer_block_tx_start_us(instance->fdt_poll_poll_us);
                FURI_LOG_D(TAG, "BlockTx timer started: %lu us", instance->fdt_poll_poll_us);
                instance->comm_state = NfcCommStateWaitRxStart;
            }
        }
        if(event & FuriHalNfcEventRxStart) {
            FURI_LOG_D(TAG, "RxStart event");
            if(instance->comm_state == NfcCommStateWaitRxStart) {
                FURI_LOG_D(TAG, "Transition: WaitRxStart -> WaitRxEnd");
                furi_hal_nfc_timer_block_tx_stop();
                furi_hal_nfc_timer_fwt_stop();
                instance->comm_state = NfcCommStateWaitRxEnd;
            }
        }
        if(event & FuriHalNfcEventRxEnd) {
            FURI_LOG_D(TAG, "RxEnd event");
            furi_hal_nfc_timer_block_tx_start(instance->fdt_poll_fc);
            furi_hal_nfc_timer_fwt_stop();
            FURI_LOG_D(TAG, "Transition: WaitRxEnd -> WaitBlockTxTimer");
            instance->comm_state = NfcCommStateWaitBlockTxTimer;
            break;
        }
        if(event & FuriHalNfcEventTimerFwtExpired) {
            FURI_LOG_D(TAG, "FWT timer expired");
            if(instance->comm_state == NfcCommStateWaitRxStart) {
                error = NfcErrorTimeout;
                FURI_LOG_D(TAG, "FWT Timeout error");
                if(furi_hal_nfc_timer_block_tx_is_running()) {
                    FURI_LOG_D(TAG, "Transition: WaitRxStart -> WaitBlockTxTimer (timeout)");
                    instance->comm_state = NfcCommStateWaitBlockTxTimer;
                } else {
                    FURI_LOG_D(TAG, "Transition: WaitRxStart -> ReadyTx (timeout)");
                    instance->comm_state = NfcCommStateReadyTx;
                }
                furi_hal_nfc_timer_fwt_stop();
                furi_hal_nfc_timer_block_tx_stop();
                break;
            }
        }
        if(event & FuriHalNfcEventAbortRequest) {
            error = NfcErrorInternal;
            furi_hal_nfc_timer_fwt_stop();
            furi_hal_nfc_timer_block_tx_stop();
            break;
        }
        if(instance->comm_state == NfcCommStateFailed) {
            error = NfcErrorInternal;
            break;
        }
        if(event == 0 || (event & FuriHalNfcEventTimeout)) {
            FURI_LOG_E(TAG, "State %d timed out — no transition event received", instance->state);
            FURI_LOG_D(TAG, "Unhandled timeout event, breaking state machine");
            error = NfcErrorTimeout;
            furi_hal_nfc_timer_fwt_stop();
            furi_hal_nfc_timer_block_tx_stop();
            break;
        }
    }
    return error;
}

NfcError nfc_iso14443a_poller_trx_custom_parity(
    Nfc* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
            if(event & FuriHalNfcEventAbortRequest) {
                ret = NfcErrorInternal;
                break;
            }
        }
        if(ret != NfcErrorNone) break;
        bit_buffer_write_bytes_with_parity(
            tx_buffer, instance->tx_buffer, sizeof(instance->tx_buffer), &instance->tx_bits);
        error =
            furi_hal_nfc_iso14443a_poller_tx_custom_parity(instance->tx_buffer, instance->tx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }
        if(instance->rx_bits >= 9) {
            if((instance->rx_bits % 9) != 0) {
                ret = NfcErrorDataFormat;
                break;
            }
        }

        bit_buffer_copy_bytes_with_parity(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError
    nfc_poller_trx(Nfc* instance, const BitBuffer* tx_buffer, BitBuffer* rx_buffer, uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
            if(event & FuriHalNfcEventAbortRequest) {
                ret = NfcErrorInternal;
                break;
            }
        }
        if(ret != NfcErrorNone) break;
        error =
            furi_hal_nfc_poller_tx(bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError nfc_iso14443a_listener_set_col_res_data(
    Nfc* instance,
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak) {
    furi_check(instance);

    FuriHalNfcError error =
        furi_hal_nfc_iso14443a_listener_set_col_res_data(uid, uid_len, atqa, sak);
    instance->comm_state = NfcCommStateIdle;
    return nfc_process_hal_error(error);
}

NfcError nfc_iso14443a_poller_trx_short_frame(

    Nfc* instance,
    NfcIso14443aShortFrame frame,
    BitBuffer* rx_buffer,
    uint32_t fwt)

{
    furi_check(instance);
    furi_check(rx_buffer);

    FuriHalNfcaShortFrame short_frame = (frame == NfcIso14443aShortFrameAllReqa) ?
                                            FuriHalNfcaShortFrameAllReq :
                                            FuriHalNfcaShortFrameSensReq;

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;

    do {
        furi_hal_nfc_trx_reset();

        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & FuriHalNfcEventTimerBlockTxExpired) {
                break;
            };
        }

        error = furi_hal_nfc_iso14443a_poller_trx_short_frame(short_frame);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }
        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }
        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError nfc_iso14443a_poller_trx_sdd_frame(
    Nfc* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);

    furi_check(instance->poller_state == NfcPollerStateReady);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
            if(event & FuriHalNfcEventAbortRequest) {
                ret = NfcErrorInternal;
                break;
            }
        }
        if(ret != NfcErrorNone) break;
        error = furi_hal_nfc_iso14443a_tx_sdd_frame(
            bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller TX");
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_T(TAG, "Failed TRX state machine");
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_D(TAG, "Failed in poller RX");
            ret = nfc_process_hal_error(error);
            break;
        }

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    return ret;
}

NfcError nfc_iso14443a_listener_tx_custom_parity(Nfc* instance, const BitBuffer* tx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;

    const uint8_t* tx_data = bit_buffer_get_data(tx_buffer);
    const uint8_t* tx_parity = bit_buffer_get_parity(tx_buffer);
    size_t tx_bits = bit_buffer_get_size(tx_buffer);

    error = furi_hal_nfc_iso14443a_listener_tx_custom_parity(tx_data, tx_parity, tx_bits);
    ret = nfc_process_hal_error(error);

    return ret;
}

NfcError nfc_iso15693_listener_tx_sof(Nfc* instance) {
    furi_check(instance);

    uint32_t timeout = furi_get_tick() + 3000; // 3 second timeout
    while(furi_hal_nfc_timer_block_tx_is_running()) {
        if(furi_get_tick() > timeout) break;
        furi_thread_yield();
    }

    FuriHalNfcError error = furi_hal_nfc_iso15693_listener_tx_sof();
    NfcError ret = nfc_process_hal_error(error);

    return ret;
}

NfcError nfc_felica_listener_set_sensf_res_data(
    Nfc* instance,
    const uint8_t* idm,
    const uint8_t idm_len,
    const uint8_t* pmm,
    const uint8_t pmm_len,
    const uint16_t sys_code) {
    furi_check(instance);

    FuriHalNfcError error =
        furi_hal_nfc_felica_listener_set_sensf_res_data(idm, idm_len, pmm, pmm_len, sys_code);
    instance->comm_state = NfcCommStateIdle;
    return nfc_process_hal_error(error);
}

void nfc_felica_listener_timer_anticol_start(Nfc* instance, uint8_t target_time_slot) {
    furi_check(instance);

    furi_hal_nfc_timer_block_tx_start(
        NFC_FELICA_LISTENER_RESPONSE_TIME_A_FC +
        target_time_slot * NFC_FELICA_LISTENER_RESPONSE_TIME_B_FC);
}

void nfc_felica_listener_timer_anticol_stop(Nfc* instance) {
    furi_check(instance);

    if(furi_hal_nfc_timer_block_tx_is_running()) {
        furi_hal_nfc_timer_block_tx_stop();
    }
}

#endif // FW_CFG_unit_tests
