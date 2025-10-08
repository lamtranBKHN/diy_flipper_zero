#ifndef FW_CFG_unit_tests

#include "nfc.h"

#include <furi_hal_nfc.h>
#include <furi/furi.h>

#define TAG "NfcCore"

#define NFC_MAX_BUFFER_SIZE (512)

// --- Internal State Enums ---
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

// --- Main Struct ---
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
};

typedef bool (*NfcWorkerPollerStateHandler)(Nfc* instance);

// --- Static Data ---
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

// --- Helper Functions ---

// START: Added HAL Event Logging Helper
static void nfc_log_hal_event(const char* action, FuriHalNfcEvent event_mask) {
    FURI_LOG_D(TAG, "%s with mask 0x%08X:", action, event_mask);
    if(event_mask & FuriHalNfcEventRxStart) FURI_LOG_D(TAG, "  - RxStart: Reception has started");
    if(event_mask & FuriHalNfcEventRxEnd) FURI_LOG_D(TAG, "  - RxEnd: Reception has finished");
    if(event_mask & FuriHalNfcEventTxEnd) FURI_LOG_D(TAG, "  - TxEnd: Transmission has finished");
    if(event_mask & FuriHalNfcEventTimerFwtExpired) FURI_LOG_D(TAG, "  - FwtExpired: Frame Wait Timer expired");
    if(event_mask & FuriHalNfcEventTimerBlockTxExpired) FURI_LOG_D(TAG, "  - BlockTxExpired: Guard/blocking timer expired");
    if(event_mask & FuriHalNfcEventFieldOn) FURI_LOG_D(TAG, "  - FieldOn: External field detected");
    if(event_mask & FuriHalNfcEventFieldOff) FURI_LOG_D(TAG, "  - FieldOff: External field removed");
    if(event_mask & FuriHalNfcEventListenerActive) FURI_LOG_D(TAG, "  - ListenerActive: Listener has been activated");
    if(event_mask & FuriHalNfcEventAbortRequest) FURI_LOG_D(TAG, "  - AbortRequest: User/system requested abort");
}
// END: Added HAL Event Logging Helper

static NfcError nfc_process_hal_error(FuriHalNfcError error) {
    NfcError ret = NfcErrorNone;
    if(error != FuriHalNfcErrorNone) {
        FURI_LOG_W(TAG, "Processing HAL error code: %d", error);
    }
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
    default:
        ret = NfcErrorInternal;
    }
    return ret;
}

// --- Worker Threads ---
static int32_t nfc_worker_listener(void* context) {
    furi_assert(context);
    Nfc* instance = context;
    FURI_LOG_I(TAG, "Listener worker thread started");

    furi_assert(instance->callback);
    furi_assert(instance->config_state == NfcConfigurationStateDone);

    instance->state = NfcStateRunning;
    furi_hal_nfc_event_start();

    NfcEventData event_data = {.buffer = bit_buffer_alloc(NFC_MAX_BUFFER_SIZE)};
    NfcEvent nfc_event = {.data = event_data};
    NfcCommand command = NfcCommandContinue;

    FURI_LOG_D(TAG, "Entering listener event loop...");
    while(true) {
        FuriHalNfcEvent event = furi_hal_nfc_listener_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        nfc_log_hal_event("Listener worker received HAL event", event);

        if(event & FuriHalNfcEventAbortRequest) {
            FURI_LOG_I(TAG, "Abort request received, exiting listener loop.");
            nfc_event.type = NfcEventTypeUserAbort;
            instance->callback(nfc_event, instance->context);
            break;
        }
        if(event & FuriHalNfcEventFieldOn) {
            FURI_LOG_D(TAG, "Event: Field ON");
            nfc_event.type = NfcEventTypeFieldOn;
            instance->callback(nfc_event, instance->context);
        }
        if(event & FuriHalNfcEventFieldOff) {
            FURI_LOG_D(TAG, "Event: Field OFF");
            nfc_event.type = NfcEventTypeFieldOff;
            instance->callback(nfc_event, instance->context);
            furi_hal_nfc_listener_idle();
        }
        if(event & FuriHalNfcEventListenerActive) {
            FURI_LOG_D(TAG, "Event: Listener Activated");
            nfc_event.type = NfcEventTypeListenerActivated;
            instance->callback(nfc_event, instance->context);
        }
        if(event & FuriHalNfcEventRxEnd) {
            FURI_LOG_D(TAG, "Event: RX End");
            furi_hal_nfc_timer_block_tx_start(instance->fdt_listen_fc);

            nfc_event.type = NfcEventTypeRxEnd;
            furi_hal_nfc_listener_rx(
                instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
            bit_buffer_copy_bits(event_data.buffer, instance->rx_buffer, instance->rx_bits);
            FURI_LOG_D(
                TAG, "Dispatching RxEnd to user callback, received %zu bits", instance->rx_bits);
            command = instance->callback(nfc_event, instance->context);
            FURI_LOG_D(TAG, "User callback returned command: %d", command);
            if(command == NfcCommandStop) {
                break;
            } else if(command == NfcCommandReset) {
                furi_hal_nfc_listener_enable_rx();
            } else if(command == NfcCommandSleep) {
                furi_hal_nfc_listener_idle();
            }
        }
    }

    FURI_LOG_I(TAG, "Listener worker thread finished. Cleaning up.");
    furi_hal_nfc_reset_mode();
    instance->config_state = NfcConfigurationStateIdle;

    bit_buffer_free(event_data.buffer);
    furi_hal_nfc_low_power_mode_start();
    return 0;
}

bool nfc_worker_poller_start_handler(Nfc* instance) {
    FURI_LOG_D(TAG, "Poller state: START -> Turning field ON");
    furi_hal_nfc_poller_field_on();
    if(instance->guard_time_us) {
        FURI_LOG_T(TAG, "Starting guard timer: %lu us", instance->guard_time_us);
        furi_hal_nfc_timer_block_tx_start_us(instance->guard_time_us);
        FuriHalNfcEvent event = furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        nfc_log_hal_event("Guard timer wait received event", event);
        furi_assert(event & FuriHalNfcEventTimerBlockTxExpired);
        FURI_LOG_T(TAG, "Guard timer expired");
    }
    instance->poller_state = NfcPollerStateReady;
    return false;
}

bool nfc_worker_poller_ready_handler(Nfc* instance) {
    FURI_LOG_D(TAG, "Poller state: READY -> Calling user callback");
    NfcCommand command = NfcCommandContinue;

    NfcEvent event = {.type = NfcEventTypePollerReady};
    command = instance->callback(event, instance->context);
    FURI_LOG_D(TAG, "User callback returned command: %d", command);

    if(command == NfcCommandReset) {
        instance->poller_state = NfcPollerStateReset;
    } else if(command == NfcCommandStop) {
        instance->poller_state = NfcPollerStateStop;
    }
    return false;
}

bool nfc_worker_poller_reset_handler(Nfc* instance) {
    FURI_LOG_D(TAG, "Poller state: RESET -> Cycling power");
    furi_hal_nfc_low_power_mode_start();
    furi_delay_ms(100);
    furi_hal_nfc_low_power_mode_stop();
    instance->poller_state = NfcPollerStateStart;
    return false;
}

bool nfc_worker_poller_stop_handler(Nfc* instance) {
    FURI_LOG_D(TAG, "Poller state: STOP -> Shutting down");
    furi_hal_nfc_reset_mode();
    instance->config_state = NfcConfigurationStateIdle;
    furi_hal_nfc_low_power_mode_start();
    furi_delay_ms(10);
    instance->poller_state = NfcPollerStateStart;
    return true; // Exit the loop
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
    FURI_LOG_I(TAG, "Poller worker thread started");
    furi_assert(instance->callback);
    instance->state = NfcStateRunning;
    instance->poller_state = NfcPollerStateStart;

    furi_hal_nfc_event_start();

    FURI_LOG_D(TAG, "Entering poller state machine loop...");
    bool exit = false;
    while(!exit) {
        // NO LOCKS HERE - The state handlers will manage their own.
        exit = nfc_worker_poller_state_handlers[instance->poller_state](instance);
    }
    FURI_LOG_I(TAG, "Poller worker thread finished.");
    return 0;
}


Nfc* nfc_alloc(void) {
    FURI_LOG_I(TAG, "Allocating new NFC instance");

    // DO NOT ACQUIRE THE HARDWARE LOCK HERE.
    // furi_check(furi_hal_nfc_acquire() == FuriHalNfcErrorNone); 

    Nfc* instance = malloc(sizeof(Nfc));

    instance->state = NfcStateIdle;
    instance->comm_state = NfcCommStateIdle;
    instance->config_state = NfcConfigurationStateIdle;

    instance->worker_thread = furi_thread_alloc();
    furi_thread_set_name(instance->worker_thread, "NfcWorker");
    furi_thread_set_context(instance->worker_thread, instance);
    furi_thread_set_priority(instance->worker_thread, FuriThreadPriorityHighest);
    furi_thread_set_stack_size(instance->worker_thread, 8 * 1024);

    FURI_LOG_D(TAG, "NFC instance and worker thread allocated successfully.");
    return instance;
}

// CORRECTED nfc_free
void nfc_free(Nfc* instance) {
    furi_check(instance);
    furi_check(instance->state == NfcStateIdle);
    FURI_LOG_I(TAG, "Freeing NFC instance %p", instance);

    furi_thread_free(instance->worker_thread);
    free(instance);

    // DO NOT RELEASE A LOCK THAT WAS NEVER ACQUIRED.
    // furi_hal_nfc_release();
}

void nfc_config(Nfc* instance, NfcMode mode, NfcTech tech) {
    furi_check(instance);
    furi_check(mode < NfcModeNum);
    furi_check(tech < NfcTechNum);
    furi_check(instance->config_state == NfcConfigurationStateIdle);
    
    // --- DETAILED LOGGING ADDED ---
    FURI_LOG_I(TAG, "--- Entering nfc_config ---");

    // --- CRITICAL FIX: Acquire the hardware lock before using it ---
    FURI_LOG_D(TAG, "Attempting to acquire NFC hardware lock...");
    furi_check(furi_hal_nfc_acquire() == FuriHalNfcErrorNone);
    FURI_LOG_D(TAG, "Hardware lock ACQUIRED.");

    FuriHalNfcTech hal_tech = nfc_tech_table[mode][tech];
    if(hal_tech == FuriHalNfcTechInvalid) {
        FURI_LOG_E(TAG, "Unsupported mode for this tech, crashing!");
        // Release lock before crashing to be safe
        furi_hal_nfc_release();
        furi_crash("Unsupported mode for given tech");
    }
    
    FuriHalNfcMode hal_mode = (mode == NfcModePoller) ? FuriHalNfcModePoller :
                                                        FuriHalNfcModeListener;

    FURI_LOG_D(TAG, "Calling furi_hal_nfc_low_power_mode_stop()...");
    furi_hal_nfc_low_power_mode_stop();
    FURI_LOG_D(TAG, "Returned from furi_hal_nfc_low_power_mode_stop().");

    FURI_LOG_D(TAG, "Calling furi_hal_nfc_set_mode()...");
    furi_hal_nfc_set_mode(hal_mode, hal_tech);
    FURI_LOG_D(TAG, "Returned from furi_hal_nfc_set_mode().");

    instance->mode = mode;
    instance->config_state = NfcConfigurationStateDone;

    // --- CRITICAL FIX: Release the hardware lock when done ---
    FURI_LOG_D(TAG, "Attempting to release NFC hardware lock...");
    furi_hal_nfc_release();
    FURI_LOG_D(TAG, "Hardware lock RELEASED.");

    // --- DETAILED LOGGING ADDED ---
    FURI_LOG_I(TAG, "--- Exiting nfc_config ---");
}

void nfc_set_fdt_poll_fc(Nfc* instance, uint32_t fdt_poll_fc) {
    furi_check(instance);
    FURI_LOG_D(TAG, "Set FDT Poll (Frame Delay Time): %lu fc", fdt_poll_fc);
    instance->fdt_poll_fc = fdt_poll_fc;
}

void nfc_set_fdt_listen_fc(Nfc* instance, uint32_t fdt_listen_fc) {
    furi_check(instance);
    FURI_LOG_D(TAG, "Set FDT Listen (Frame Delay Time): %lu fc", fdt_listen_fc);
    instance->fdt_listen_fc = fdt_listen_fc;
}

void nfc_set_fdt_poll_poll_us(Nfc* instance, uint32_t fdt_poll_poll_us) {
    furi_check(instance);
    FURI_LOG_D(TAG, "Set FDT Poll-Poll (Mask RX Time): %lu us", fdt_poll_poll_us);
    instance->fdt_poll_poll_us = fdt_poll_poll_us;
}

void nfc_set_guard_time_us(Nfc* instance, uint32_t guard_time_us) {
    furi_check(instance);
    FURI_LOG_D(TAG, "Set Guard Time: %lu us", guard_time_us);
    instance->guard_time_us = guard_time_us;
}

void nfc_set_mask_receive_time_fc(Nfc* instance, uint32_t mask_rx_time_fc) {
    furi_check(instance);
    instance->mask_rx_time_fc = mask_rx_time_fc;
}

void nfc_start(Nfc* instance, NfcEventCallback callback, void* context) {
    furi_check(instance);
    furi_check(instance->worker_thread);
    furi_check(callback);
    furi_check(instance->config_state == NfcConfigurationStateDone);
    FURI_LOG_I(
        TAG, "Starting worker thread in mode: %s", (instance->mode == NfcModePoller) ? "Poller" : "Listener");

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
    FURI_LOG_I(TAG, "Stopping worker thread...");

    if(instance->mode == NfcModeListener) {
        furi_hal_nfc_abort();
    }
    furi_thread_join(instance->worker_thread);

    instance->state = NfcStateIdle;
    FURI_LOG_I(TAG, "Worker thread stopped.");
}

static NfcError nfc_poller_trx_state_machine(Nfc* instance, uint32_t fwt_fc) {
    FuriHalNfcEvent event = 0;
    NfcError error = NfcErrorNone;
    FURI_LOG_D(TAG, "Entering TRX state machine, fwt: %lu", fwt_fc);

    while(true) {
        event = furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
        nfc_log_hal_event("TRX SM received event", event);

        if(event & FuriHalNfcEventTimerBlockTxExpired) {
            if(instance->comm_state == NfcCommStateWaitBlockTxTimer) {
                FURI_LOG_T(TAG, "TRX SM: Block TX timer expired, ready to TX");
                instance->comm_state = NfcCommStateReadyTx;
            }
        }
        if(event & FuriHalNfcEventTxEnd) {
            if(instance->comm_state == NfcCommStateWaitTxEnd) {
                FURI_LOG_T(TAG, "TRX SM: TX ended, starting FWT and poll timers");
                if(fwt_fc) {
                    furi_hal_nfc_timer_fwt_start(fwt_fc);
                }
                furi_hal_nfc_timer_block_tx_start_us(instance->fdt_poll_poll_us);
                instance->comm_state = NfcCommStateWaitRxStart;
            }
        }
        if(event & FuriHalNfcEventRxStart) {
            if(instance->comm_state == NfcCommStateWaitRxStart) {
                FURI_LOG_T(TAG, "TRX SM: RX started, stopping timers");
                furi_hal_nfc_timer_block_tx_stop();
                furi_hal_nfc_timer_fwt_stop();
                instance->comm_state = NfcCommStateWaitRxEnd;
            }
        }
        if(event & FuriHalNfcEventRxEnd) {
            FURI_LOG_T(TAG, "TRX SM: RX ended, transaction complete");
            furi_hal_nfc_timer_block_tx_start(instance->fdt_poll_fc);
            furi_hal_nfc_timer_fwt_stop();
            instance->comm_state = NfcCommStateWaitBlockTxTimer;
            break; // Success
        }
        if(event & FuriHalNfcEventTimerFwtExpired) {
            if(instance->comm_state == NfcCommStateWaitRxStart) {
                FURI_LOG_W(TAG, "TRX SM: FWT Timeout!");
                error = NfcErrorTimeout;
                furi_hal_nfc_timer_fwt_stop(); // Ensure FWT timer is stopped on timeout
                if(furi_hal_nfc_timer_block_tx_is_running()) {
                    instance->comm_state = NfcCommStateWaitBlockTxTimer;
                } else {
                    instance->comm_state = NfcCommStateReadyTx;
                }
                break; // Timeout error
            }
        }
    }
    FURI_LOG_D(TAG, "Exiting TRX state machine with error code: %d", error);
    return error;
}

NfcError nfc_listener_tx(Nfc* instance, const BitBuffer* tx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);
    FURI_LOG_D(TAG, "Listener TX initiated, tx_bits: %zu", bit_buffer_get_size(tx_buffer));

    NfcError ret = NfcErrorNone;

    while(furi_hal_nfc_timer_block_tx_is_running()) {
    }

    FuriHalNfcError error =
        furi_hal_nfc_listener_tx(bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
    if(error != FuriHalNfcErrorNone) {
        FURI_LOG_W(TAG, "Failed in listener TX, HAL error: %d", error);
        ret = nfc_process_hal_error(error);
    }
    return ret;
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
    FURI_LOG_D(TAG, "Poller TRX (Custom Parity) initiated, tx_bits: %zu, fwt: %lu", bit_buffer_get_size(tx_buffer), fwt);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FURI_LOG_T(TAG, "Waiting for block TX timer...");
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            nfc_log_hal_event("TRX custom parity wait received event", event);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
        }
        bit_buffer_write_bytes_with_parity(
            tx_buffer, instance->tx_buffer, sizeof(instance->tx_buffer), &instance->tx_bits);
        error =
            furi_hal_nfc_iso14443a_poller_tx_custom_parity(instance->tx_buffer, instance->tx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller TX (Custom Parity), HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_W(TAG, "TRX state machine failed with error: %d", ret);
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller RX, HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        FURI_LOG_D(TAG, "Poller RX successful, received %zu bits", instance->rx_bits);
        if(instance->rx_bits >= 9) {
            if((instance->rx_bits % 9) != 0) {
                ret = NfcErrorDataFormat;
                break;
            }
        }

        bit_buffer_copy_bytes_with_parity(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    FURI_LOG_D(TAG, "Poller TRX (Custom Parity) finished, final status: %d", ret);
    return ret;
}

NfcError
    nfc_poller_trx(Nfc* instance, const BitBuffer* tx_buffer, BitBuffer* rx_buffer, uint32_t fwt) {
    furi_check(instance);
    furi_check(tx_buffer);
    furi_check(rx_buffer);
    furi_check(instance->poller_state == NfcPollerStateReady);
    FURI_LOG_D(
        TAG, "Poller TRX (Standard) initiated, tx_bits: %zu, fwt: %lu", bit_buffer_get_size(tx_buffer), fwt);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FURI_LOG_T(TAG, "Waiting for block TX timer...");
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            nfc_log_hal_event("Poller TRX wait received event", event);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
        }
        error =
            furi_hal_nfc_poller_tx(bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller TX, HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_W(TAG, "TRX state machine failed with error: %d", ret);
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller RX, HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        FURI_LOG_D(TAG, "Poller RX successful, received %zu bits", instance->rx_bits);

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    FURI_LOG_D(TAG, "Poller TRX (Standard) finished, final status: %d", ret);
    return ret;
}

NfcError nfc_iso14443a_listener_set_col_res_data(
    Nfc* instance,
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak) {
    furi_check(instance);
    FURI_LOG_D(TAG, "Setting ISO14443A listener collision/resolution data");
    FuriHalNfcError error =
        furi_hal_nfc_iso14443a_listener_set_col_res_data(uid, uid_len, atqa, sak);
    instance->comm_state = NfcCommStateIdle;
    return nfc_process_hal_error(error);
}

NfcError nfc_iso14443a_poller_trx_short_frame(
    Nfc* instance,
    NfcIso14443aShortFrame frame,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_check(instance);
    furi_check(rx_buffer);
    furi_check(instance->poller_state == NfcPollerStateReady);
    FURI_LOG_D(TAG, "Poller TRX (Short Frame) initiated, frame type: %d, fwt: %lu", frame, fwt);

    FuriHalNfcaShortFrame short_frame = (frame == NfcIso14443aShortFrameAllReqa) ?
                                            FuriHalNfcaShortFrameAllReq :
                                            FuriHalNfcaShortFrameSensReq;

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FURI_LOG_T(TAG, "Waiting for block TX timer...");
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            nfc_log_hal_event("TRX short frame wait received event", event);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
        }
        error = furi_hal_nfc_iso14443a_poller_trx_short_frame(short_frame);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller TX (Short Frame), HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_W(TAG, "TRX state machine failed with error: %d", ret);
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller RX, HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        FURI_LOG_D(TAG, "Poller RX successful, received %zu bits", instance->rx_bits);

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    FURI_LOG_D(TAG, "Poller TRX (Short Frame) finished, final status: %d", ret);
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
    FURI_LOG_D(
        TAG, "Poller TRX (SDD Frame) initiated, tx_bits: %zu, fwt: %lu", bit_buffer_get_size(tx_buffer), fwt);

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;
    do {
        furi_hal_nfc_trx_reset();
        while(furi_hal_nfc_timer_block_tx_is_running()) {
            FURI_LOG_T(TAG, "Waiting for block TX timer...");
            FuriHalNfcEvent event =
                furi_hal_nfc_poller_wait_event(FURI_HAL_NFC_EVENT_WAIT_FOREVER);
            nfc_log_hal_event("TRX SDD frame wait received event", event);
            if(event & FuriHalNfcEventTimerBlockTxExpired) break;
        }
        error = furi_hal_nfc_iso14443a_tx_sdd_frame(
            bit_buffer_get_data(tx_buffer), bit_buffer_get_size(tx_buffer));
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller TX (SDD Frame), HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        instance->comm_state = NfcCommStateWaitTxEnd;
        ret = nfc_poller_trx_state_machine(instance, fwt);
        if(ret != NfcErrorNone) {
            FURI_LOG_W(TAG, "TRX state machine failed with error: %d", ret);
            break;
        }

        error = furi_hal_nfc_poller_rx(
            instance->rx_buffer, sizeof(instance->rx_buffer), &instance->rx_bits);
        if(error != FuriHalNfcErrorNone) {
            FURI_LOG_W(TAG, "Failed in poller RX, HAL error: %d", error);
            ret = nfc_process_hal_error(error);
            break;
        }
        FURI_LOG_D(TAG, "Poller RX successful, received %zu bits", instance->rx_bits);

        bit_buffer_copy_bits(rx_buffer, instance->rx_buffer, instance->rx_bits);
    } while(false);

    FURI_LOG_D(TAG, "Poller TRX (SDD Frame) finished, final status: %d", ret);
    return ret;
}

NfcError nfc_iso14443a_listener_tx_custom_parity(Nfc* instance, const BitBuffer* tx_buffer) {
    furi_check(instance);
    furi_check(tx_buffer);
    FURI_LOG_D(
        TAG, "Listener TX (Custom Parity) initiated, tx_bits: %zu", bit_buffer_get_size(tx_buffer));

    NfcError ret = NfcErrorNone;
    FuriHalNfcError error = FuriHalNfcErrorNone;

    const uint8_t* tx_data = bit_buffer_get_data(tx_buffer);
    const uint8_t* tx_parity = bit_buffer_get_parity(tx_buffer);
    size_t tx_bits = bit_buffer_get_size(tx_buffer);

    error = furi_hal_nfc_iso14443a_listener_tx_custom_parity(tx_data, tx_parity, tx_bits);
    ret = nfc_process_hal_error(error);
    if(ret != NfcErrorNone) {
        FURI_LOG_W(TAG, "Failed in listener TX (Custom Parity), HAL error: %d", error);
    }
    return ret;
}

NfcError nfc_iso15693_listener_tx_sof(Nfc* instance) {
    furi_check(instance);
    FURI_LOG_D(TAG, "Listener TX (ISO15693 SOF)");

    while(furi_hal_nfc_timer_block_tx_is_running()) {
    }

    FuriHalNfcError error = furi_hal_nfc_iso15693_listener_tx_sof();
    NfcError ret = nfc_process_hal_error(error);
    if(ret != NfcErrorNone) {
        FURI_LOG_W(TAG, "Failed in listener TX (ISO15693 SOF), HAL error: %d", error);
    }
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
    FURI_LOG_D(TAG, "Setting Felica listener SENSF_RES data");

    FuriHalNfcError error =
        furi_hal_nfc_felica_listener_set_sensf_res_data(idm, idm_len, pmm, pmm_len, sys_code);
    instance->comm_state = NfcCommStateIdle;
    return nfc_process_hal_error(error);
}

#endif // FW_CFG_unit_tests