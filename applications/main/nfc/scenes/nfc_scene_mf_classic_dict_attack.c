#include "../nfc_app_i.h"

#include <bit_lib/bit_lib.h>
#include <dolphin/dolphin.h>

#define TAG "NfcMfClassicDictAttack"

#define NFC_DICT_ATTACK_POLLER_STOP_TIMEOUT_MS 100

/* Bail out of dict attack after this many keys with zero success.
 * With 80ms auth timeout: 20 keys × 2 (A+B) × ~150ms = ~6s.
 * If no key found in 20 attempts, card likely uses non-dict keys.
 * Most MIFARE Classic cards with dictionary keys match within the
 * first 10-15 common keys (FFFFFFFFFFFF, A0A1A2A3A4A5, etc.). */
#define NFC_DICT_ATTACK_BAIL_OUT_KEYS 20

/* Chunk size for throttled file copy: 512 bytes matches internal SD buffer size */
#define NFC_DICT_COPY_CHUNK_SIZE 512

/* Yield every 8 chunks (~4KB) during file copy to let display thread refresh.
 * Without this, storage_common_copy() holds SPI1 continuously for 200ms+ on
 * large dictionary files, starving the display and triggering the watchdog. */
#define NFC_DICT_COPY_YIELD_INTERVAL 8

/**
 * Throttled file copy that yields the CPU periodically during the transfer
 * to allow the display thread to refresh on SPI1-shared hardware.
 *
 * Like storage_common_copy() but yields every NFC_DICT_COPY_YIELD_INTERVAL
 * chunks, preventing watchdog timeouts during large dictionary copies.
 * Only handles regular files (not directories).
 */
static FS_Error
    nfc_dict_storage_copy_throttled(Storage* storage, const char* old_path, const char* new_path) {
    furi_check(storage);

    FS_Error error;
    FileInfo fileinfo;
    error = storage_common_stat(storage, old_path, &fileinfo);

    if(error != FSE_OK || file_info_is_dir(&fileinfo)) {
        return error;
    }

    File* source = storage_file_alloc(storage);
    File* dest = storage_file_alloc(storage);
    uint8_t* buffer = malloc(NFC_DICT_COPY_CHUNK_SIZE);
    size_t chunks_since_yield = 0;

    do {
        if(!storage_file_open(source, old_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            error = storage_file_get_error(source);
            break;
        }
        if(!storage_file_open(dest, new_path, FSAM_WRITE, FSOM_CREATE_NEW)) {
            error = storage_file_get_error(dest);
            break;
        }

        uint64_t remaining = storage_file_size(source);
        while(remaining > 0) {
            const size_t read_size = remaining > NFC_DICT_COPY_CHUNK_SIZE ?
                                         NFC_DICT_COPY_CHUNK_SIZE :
                                         (size_t)remaining;

            if(storage_file_read(source, buffer, read_size) != read_size) {
                error = storage_file_get_error(source);
                break;
            }
            if(storage_file_write(dest, buffer, read_size) != read_size) {
                error = storage_file_get_error(dest);
                break;
            }
            remaining -= read_size;

            /* Yield every N chunks to let display thread refresh.
             * On SPI1-shared hardware (display + SD card), a continuous
             * copy holds the bus for 200ms+.  Yielding periodically breaks
             * this into shorter intervals. */
            if(++chunks_since_yield >= NFC_DICT_COPY_YIELD_INTERVAL) {
                furi_thread_yield();
                chunks_since_yield = 0;
            }
        }
        if(error != FSE_OK) break;
    } while(false);

    free(buffer);
    storage_file_free(source);
    storage_file_free(dest);
    return error;
}

static void nfc_scene_mf_classic_dict_attack_update_view(NfcApp* instance);
static void nfc_scene_mf_classic_dict_attack_prepare_view(NfcApp* instance);

typedef enum {
    DictAttackStateCUIDDictInProgress,
    DictAttackStateUserDictInProgress,
    DictAttackStateSystemDictInProgress,
} DictAttackState;

NfcCommand nfc_dict_attack_worker_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolMfClassic);

    NfcCommand command = NfcCommandContinue;
    MfClassicPollerEvent* mfc_event = event.event_data;

    NfcApp* instance = context;
    if(mfc_event->type == MfClassicPollerEventTypeCardDetected) {
        instance->nfc_dict_context.is_card_present = true;
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventCardDetected);
    } else if(mfc_event->type == MfClassicPollerEventTypeCardLost) {
        instance->nfc_dict_context.is_card_present = false;
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventCardLost);
    } else if(mfc_event->type == MfClassicPollerEventTypeRequestMode) {
        const MfClassicData* mfc_data =
            nfc_device_get_data(instance->nfc_device, NfcProtocolMfClassic);
        mfc_event->data->poller_mode.mode = (instance->nfc_dict_context.enhanced_dict) ?
                                                MfClassicPollerModeDictAttackEnhanced :
                                                MfClassicPollerModeDictAttackStandard;
        mfc_event->data->poller_mode.data = mfc_data;
        mfc_event->data->poller_mode.nested_phase = instance->nfc_dict_context.nested_phase;
        mfc_event->data->poller_mode.prng_type = instance->nfc_dict_context.prng_type;
        mfc_event->data->poller_mode.backdoor = instance->nfc_dict_context.backdoor;
        instance->nfc_dict_context.sectors_total =
            mf_classic_get_total_sectors_num(mfc_data->type);
        mf_classic_get_read_sectors_and_keys(
            mfc_data,
            &instance->nfc_dict_context.sectors_read,
            &instance->nfc_dict_context.keys_found);
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeRequestKey) {
        MfClassicKey key = {};
        /* Bail out early if many keys tried with zero success — card likely
         * uses non-dict keys.  Avoids wasting minutes on timeout-heavy dicts. */
        if(instance->nfc_dict_context.keys_found == 0 &&
           instance->nfc_dict_context.dict_keys_current >= NFC_DICT_ATTACK_BAIL_OUT_KEYS) {
            FURI_LOG_I(
                TAG,
                "Dict bail-out: %zu keys tried, 0 found",
                instance->nfc_dict_context.dict_keys_current);
            mfc_event->data->key_request_data.key_provided = false;
        } else if(keys_dict_get_next_key(
                      instance->nfc_dict_context.dict, key.data, sizeof(MfClassicKey))) {
            mfc_event->data->key_request_data.key = key;
            mfc_event->data->key_request_data.key_provided = true;
            instance->nfc_dict_context.dict_keys_current++;
            if(instance->nfc_dict_context.dict_keys_current % 10 == 0) {
                view_dispatcher_send_custom_event(
                    instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
            }
        } else {
            mfc_event->data->key_request_data.key_provided = false;
        }
    } else if(mfc_event->type == MfClassicPollerEventTypeDataUpdate) {
        MfClassicPollerEventDataUpdate* data_update = &mfc_event->data->data_update;
        instance->nfc_dict_context.sectors_read = data_update->sectors_read;
        instance->nfc_dict_context.keys_found = data_update->keys_found;
        instance->nfc_dict_context.current_sector = data_update->current_sector;
        instance->nfc_dict_context.nested_phase = data_update->nested_phase;
        instance->nfc_dict_context.prng_type = data_update->prng_type;
        instance->nfc_dict_context.backdoor = data_update->backdoor;
        instance->nfc_dict_context.nested_target_key = data_update->nested_target_key;
        instance->nfc_dict_context.msb_count = data_update->msb_count;
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeNextSector) {
        keys_dict_rewind(instance->nfc_dict_context.dict);
        instance->nfc_dict_context.dict_keys_current = 0;
        instance->nfc_dict_context.current_sector =
            mfc_event->data->next_sector_data.current_sector;
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeFoundKeyA) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeFoundKeyB) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeKeyAttackStart) {
        instance->nfc_dict_context.key_attack_current_sector =
            mfc_event->data->key_attack_data.current_sector;
        instance->nfc_dict_context.is_key_attack = true;
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeKeyAttackStop) {
        keys_dict_rewind(instance->nfc_dict_context.dict);
        instance->nfc_dict_context.is_key_attack = false;
        instance->nfc_dict_context.dict_keys_current = 0;
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackDataUpdate);
    } else if(mfc_event->type == MfClassicPollerEventTypeSuccess) {
        const MfClassicData* mfc_data = nfc_poller_get_data(instance->poller);
        nfc_device_set_data(instance->nfc_device, NfcProtocolMfClassic, mfc_data);
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDictAttackComplete);
        command = NfcCommandStop;
    }

    return command;
}

void nfc_dict_attack_dict_attack_result_callback(DictAttackEvent event, void* context) {
    furi_assert(context);
    NfcApp* instance = context;

    if(event == DictAttackEventSkipPressed) {
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventDictAttackSkip);
    }
}

static void nfc_scene_mf_classic_dict_attack_stop_poller_bounded(NfcApp* instance) {
    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);
    instance->poller = NULL;
}

static void nfc_scene_mf_classic_dict_attack_save_backdoor_state(NfcApp* instance) {
    instance->nfc_dict_context.saved_nested_phase = instance->nfc_dict_context.nested_phase;
    instance->nfc_dict_context.saved_prng_type = instance->nfc_dict_context.prng_type;
    instance->nfc_dict_context.saved_backdoor = instance->nfc_dict_context.backdoor;
}

static void nfc_scene_mf_classic_dict_attack_restore_backdoor_state(NfcApp* instance) {
    if(instance->nfc_dict_context.saved_nested_phase != MfClassicNestedPhaseNone) {
        instance->nfc_dict_context.nested_phase = instance->nfc_dict_context.saved_nested_phase;
        instance->nfc_dict_context.prng_type = instance->nfc_dict_context.saved_prng_type;
        instance->nfc_dict_context.backdoor = instance->nfc_dict_context.saved_backdoor;
    }
}

static void nfc_scene_mf_classic_dict_attack_transition_phase(
    NfcApp* instance,
    DictAttackState next_state) {
    nfc_scene_mf_classic_dict_attack_save_backdoor_state(instance);

    nfc_scene_mf_classic_dict_attack_stop_poller_bounded(instance);
    if(instance->nfc_dict_context.dict) {
        keys_dict_free(instance->nfc_dict_context.dict);
        instance->nfc_dict_context.dict = NULL;
    }

    scene_manager_set_scene_state(
        instance->scene_manager, NfcSceneMfClassicDictAttack, next_state);
    nfc_scene_mf_classic_dict_attack_prepare_view(instance);

    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfClassic);
    nfc_poller_start(instance->poller, nfc_dict_attack_worker_callback, instance);

    nfc_scene_mf_classic_dict_attack_restore_backdoor_state(instance);
    nfc_scene_mf_classic_dict_attack_update_view(instance);
}

static void nfc_scene_mf_classic_dict_attack_update_view(NfcApp* instance) {
    NfcMfClassicDictAttackContext* mfc_dict = &instance->nfc_dict_context;

    if(mfc_dict->is_key_attack) {
        dict_attack_set_key_attack(instance->dict_attack, mfc_dict->key_attack_current_sector);
    } else {
        dict_attack_reset_key_attack(instance->dict_attack);
        dict_attack_set_sectors_total(instance->dict_attack, mfc_dict->sectors_total);
        dict_attack_set_sectors_read(instance->dict_attack, mfc_dict->sectors_read);
        dict_attack_set_keys_found(instance->dict_attack, mfc_dict->keys_found);
        dict_attack_set_current_dict_key(instance->dict_attack, mfc_dict->dict_keys_current);
        dict_attack_set_current_sector(instance->dict_attack, mfc_dict->current_sector);
        dict_attack_set_nested_phase(instance->dict_attack, mfc_dict->nested_phase);
        dict_attack_set_prng_type(instance->dict_attack, mfc_dict->prng_type);
        dict_attack_set_backdoor(instance->dict_attack, mfc_dict->backdoor);
        dict_attack_set_nested_target_key(instance->dict_attack, mfc_dict->nested_target_key);
        dict_attack_set_msb_count(instance->dict_attack, mfc_dict->msb_count);
    }

    dict_attack_update_header(instance->dict_attack);
}

static void nfc_scene_mf_classic_dict_attack_prepare_view(NfcApp* instance) {
    uint32_t state =
        scene_manager_get_scene_state(instance->scene_manager, NfcSceneMfClassicDictAttack);
    if(state == DictAttackStateCUIDDictInProgress) {
        /* Guard: nfc_device_get_uid() furi_check-crashes if protocol is invalid.
         * After plugin verification timeouts the PN532 may have been reinitialised,
         * leaving nfc_device in an inconsistent state.  Skip CUID dict if the
         * device does not hold valid MF Classic data. */
        NfcProtocol dev_protocol = nfc_device_get_protocol(instance->nfc_device);
        if(dev_protocol != NfcProtocolMfClassic) {
            FURI_LOG_W(
                TAG, "Device protocol %d != MfClassic, skipping CUID dict", (int)dev_protocol);
            state = DictAttackStateUserDictInProgress;
        } else {
            size_t cuid_len = 0;
            const uint8_t* cuid = nfc_device_get_uid(instance->nfc_device, &cuid_len);

            if(cuid_len < 4) {
                FURI_LOG_W(TAG, "UID length %zu invalid, skipping CUID dictionary", cuid_len);
                state = DictAttackStateUserDictInProgress;
            } else {
                FuriString* cuid_dict_path = furi_string_alloc_printf(
                    "%s/mf_classic_dict_%08lx.nfc",
                    EXT_PATH("nfc/assets"),
                    (uint32_t)bit_lib_bytes_to_num_be(cuid + (cuid_len - 4), 4));

                do {
                    if(!keys_dict_check_presence(furi_string_get_cstr(cuid_dict_path))) {
                        furi_thread_yield();
                        state = DictAttackStateUserDictInProgress;
                        break;
                    }

                    /* Pre-scan: skip if dict file is too large for available RAM */
                    if(!keys_dict_check_available_ram(
                           furi_string_get_cstr(cuid_dict_path), sizeof(MfClassicKey))) {
                        FURI_LOG_W(TAG, "CUID dict too large for RAM, skipping");
                        state = DictAttackStateUserDictInProgress;
                        break;
                    }

                    instance->nfc_dict_context.dict = keys_dict_alloc(
                        furi_string_get_cstr(cuid_dict_path),
                        KeysDictModeOpenExisting,
                        sizeof(MfClassicKey));
                    furi_thread_yield();

                    if(keys_dict_get_total_keys(instance->nfc_dict_context.dict) == 0) {
                        keys_dict_free(instance->nfc_dict_context.dict);
                        state = DictAttackStateUserDictInProgress;
                        break;
                    }

                    dict_attack_set_header(instance->dict_attack, "MF Classic CUID Dictionary");
                } while(false);

                furi_string_free(cuid_dict_path);
            }
        }
    }
    if(state == DictAttackStateUserDictInProgress) {
        do {
            instance->nfc_dict_context.enhanced_dict = true;

            // Remove is idempotent — no need to check presence first.
            storage_common_remove(instance->storage, NFC_APP_MF_CLASSIC_DICT_SYSTEM_NESTED_PATH);
            /* Yield between SD operations to allow the display thread to run.
             * storage_common_copy() holds SPI1 for up to 234ms per call on large
             * dictionary files, blocking the display and triggering the watchdog. */
            furi_thread_yield();
            if(keys_dict_check_presence(NFC_APP_MF_CLASSIC_DICT_SYSTEM_PATH)) {
                FS_Error copy_result = nfc_dict_storage_copy_throttled(
                    instance->storage,
                    NFC_APP_MF_CLASSIC_DICT_SYSTEM_PATH,
                    NFC_APP_MF_CLASSIC_DICT_SYSTEM_NESTED_PATH);
                if(copy_result != FSE_OK) {
                    FURI_LOG_E(TAG, "Failed to copy system dict to nested: %d", copy_result);
                    notification_message(instance->notifications, &sequence_error);
                }
            }

            furi_thread_yield();
            if(!keys_dict_check_presence(NFC_APP_MF_CLASSIC_DICT_USER_PATH)) {
                state = DictAttackStateSystemDictInProgress;
                break;
            }

            storage_common_remove(instance->storage, NFC_APP_MF_CLASSIC_DICT_USER_NESTED_PATH);
            furi_thread_yield();
            FS_Error copy_result = nfc_dict_storage_copy_throttled(
                instance->storage,
                NFC_APP_MF_CLASSIC_DICT_USER_PATH,
                NFC_APP_MF_CLASSIC_DICT_USER_NESTED_PATH);
            if(copy_result != FSE_OK) {
                FURI_LOG_E(TAG, "Failed to copy user dict to nested: %d", copy_result);
                notification_message(instance->notifications, &sequence_error);
            }

            furi_thread_yield();

            /* Pre-scan: skip user dict if too large for available RAM */
            if(!keys_dict_check_available_ram(
                   NFC_APP_MF_CLASSIC_DICT_USER_PATH, sizeof(MfClassicKey))) {
                FURI_LOG_W(TAG, "User dict too large for RAM, skipping to system dict");
                furi_thread_yield();
                state = DictAttackStateSystemDictInProgress;
                break;
            }

            instance->nfc_dict_context.dict = keys_dict_alloc(
                NFC_APP_MF_CLASSIC_DICT_USER_PATH, KeysDictModeOpenAlways, sizeof(MfClassicKey));
            furi_thread_yield();
            if(keys_dict_get_total_keys(instance->nfc_dict_context.dict) == 0) {
                keys_dict_free(instance->nfc_dict_context.dict);
                state = DictAttackStateSystemDictInProgress;
                break;
            }

            dict_attack_set_header(instance->dict_attack, "MF Classic User Dictionary");
        } while(false);
    }
    if(state == DictAttackStateSystemDictInProgress) {
        /* System dict is the last resort — no fallback dict tier available.
         * keys_dict_alloc() already handles OOM gracefully (falls back to
         * stream-based key iteration), so we always attempt the alloc even
         * if a RAM pre-scan suggests tight memory. */
        instance->nfc_dict_context.dict = keys_dict_alloc(
            NFC_APP_MF_CLASSIC_DICT_SYSTEM_PATH, KeysDictModeOpenExisting, sizeof(MfClassicKey));
        furi_thread_yield();
        dict_attack_set_header(instance->dict_attack, "MF Classic System Dictionary");
    }

    if(!instance->nfc_dict_context.dict) {
        return;
    }
    instance->nfc_dict_context.dict_keys_total =
        keys_dict_get_total_keys(instance->nfc_dict_context.dict);
    dict_attack_set_total_dict_keys(
        instance->dict_attack, instance->nfc_dict_context.dict_keys_total);
    instance->nfc_dict_context.dict_keys_current = 0;

    dict_attack_set_callback(
        instance->dict_attack, nfc_dict_attack_dict_attack_result_callback, instance);
    nfc_scene_mf_classic_dict_attack_update_view(instance);

    scene_manager_set_scene_state(instance->scene_manager, NfcSceneMfClassicDictAttack, state);
}

void nfc_scene_mf_classic_dict_attack_on_enter(void* context) {
    NfcApp* instance = context;

    scene_manager_set_scene_state(
        instance->scene_manager, NfcSceneMfClassicDictAttack, DictAttackStateCUIDDictInProgress);
    nfc_scene_mf_classic_dict_attack_prepare_view(instance);
    dict_attack_set_card_state(instance->dict_attack, true);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewDictAttack);
    nfc_blink_read_start(instance);

    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfClassic);
    nfc_poller_start(instance->poller, nfc_dict_attack_worker_callback, instance);
}

static void nfc_scene_mf_classic_dict_attack_notify_read(NfcApp* instance) {
    if(!instance->poller) {
        FURI_LOG_W(TAG, "notify_read: stale event, poller NULL");
        return;
    }
    const MfClassicData* mfc_data = nfc_poller_get_data(instance->poller);
    bool is_card_fully_read = mf_classic_is_card_read(mfc_data);
    if(is_card_fully_read) {
        notification_message(instance->notifications, &sequence_success);
    } else {
        notification_message(instance->notifications, &sequence_semi_success);
    }
}

bool nfc_scene_mf_classic_dict_attack_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    uint32_t state =
        scene_manager_get_scene_state(instance->scene_manager, NfcSceneMfClassicDictAttack);
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventDictAttackComplete) {
            if(!instance->poller) {
                FURI_LOG_W(TAG, "Stale DictAttackComplete ignored: poller NULL");
                consumed = true;
                return consumed;
            }
            bool ran_nested_dict = instance->nfc_dict_context.nested_phase !=
                                   MfClassicNestedPhaseNone;
            if(state == DictAttackStateCUIDDictInProgress) {
                nfc_scene_mf_classic_dict_attack_transition_phase(
                    instance, DictAttackStateUserDictInProgress);
                consumed = true;
            } else if(state == DictAttackStateUserDictInProgress && !(ran_nested_dict)) {
                nfc_scene_mf_classic_dict_attack_transition_phase(
                    instance, DictAttackStateSystemDictInProgress);
                consumed = true;
            } else {
                nfc_scene_mf_classic_dict_attack_notify_read(instance);
                scene_manager_next_scene(instance->scene_manager, NfcSceneReadSuccess);
                dolphin_deed(DolphinDeedNfcReadSuccess);
                consumed = true;
            }
        } else if(event.event == NfcCustomEventCardDetected) {
            dict_attack_set_card_state(instance->dict_attack, true);
            consumed = true;
        } else if(event.event == NfcCustomEventCardLost) {
            dict_attack_set_card_state(instance->dict_attack, false);
            consumed = true;
        } else if(event.event == NfcCustomEventDictAttackDataUpdate) {
            nfc_scene_mf_classic_dict_attack_update_view(instance);
        } else if(event.event == NfcCustomEventDictAttackSkip) {
            if(!instance->poller) {
                FURI_LOG_W(TAG, "Stale DictAttackSkip ignored: poller NULL");
                consumed = true;
                return consumed;
            }
            const MfClassicData* mfc_data = nfc_poller_get_data(instance->poller);
            nfc_device_set_data(instance->nfc_device, NfcProtocolMfClassic, mfc_data);
            bool ran_nested_dict = instance->nfc_dict_context.nested_phase !=
                                   MfClassicNestedPhaseNone;
            if(state == DictAttackStateCUIDDictInProgress) {
                if(instance->nfc_dict_context.is_card_present) {
                    nfc_scene_mf_classic_dict_attack_transition_phase(
                        instance, DictAttackStateUserDictInProgress);
                } else {
                    nfc_scene_mf_classic_dict_attack_notify_read(instance);
                    scene_manager_next_scene(instance->scene_manager, NfcSceneReadSuccess);
                    dolphin_deed(DolphinDeedNfcReadSuccess);
                }
                consumed = true;
            } else if(state == DictAttackStateUserDictInProgress && !(ran_nested_dict)) {
                if(instance->nfc_dict_context.is_card_present) {
                    nfc_scene_mf_classic_dict_attack_transition_phase(
                        instance, DictAttackStateSystemDictInProgress);
                } else {
                    nfc_scene_mf_classic_dict_attack_notify_read(instance);
                    scene_manager_next_scene(instance->scene_manager, NfcSceneReadSuccess);
                    dolphin_deed(DolphinDeedNfcReadSuccess);
                }
                consumed = true;
            } else {
                nfc_scene_mf_classic_dict_attack_notify_read(instance);
                scene_manager_next_scene(instance->scene_manager, NfcSceneReadSuccess);
                dolphin_deed(DolphinDeedNfcReadSuccess);
                consumed = true;
            }
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_next_scene(instance->scene_manager, NfcSceneExitConfirm);
        consumed = true;
    }
    return consumed;
}

void nfc_scene_mf_classic_dict_attack_on_exit(void* context) {
    NfcApp* instance = context;

    nfc_scene_mf_classic_dict_attack_stop_poller_bounded(instance);

    dict_attack_reset(instance->dict_attack);
    scene_manager_set_scene_state(
        instance->scene_manager, NfcSceneMfClassicDictAttack, DictAttackStateCUIDDictInProgress);

    if(instance->nfc_dict_context.dict) {
        keys_dict_free(instance->nfc_dict_context.dict);
        instance->nfc_dict_context.dict = NULL;
    }

    instance->nfc_dict_context.current_sector = 0;
    instance->nfc_dict_context.sectors_total = 0;
    instance->nfc_dict_context.sectors_read = 0;
    instance->nfc_dict_context.keys_found = 0;
    instance->nfc_dict_context.dict_keys_total = 0;
    instance->nfc_dict_context.dict_keys_current = 0;
    instance->nfc_dict_context.is_key_attack = false;
    instance->nfc_dict_context.key_attack_current_sector = 0;
    instance->nfc_dict_context.is_card_present = false;
    instance->nfc_dict_context.nested_phase = MfClassicNestedPhaseNone;
    instance->nfc_dict_context.prng_type = MfClassicPrngTypeUnknown;
    instance->nfc_dict_context.backdoor = MfClassicBackdoorUnknown;
    instance->nfc_dict_context.saved_nested_phase = MfClassicNestedPhaseNone;
    instance->nfc_dict_context.saved_prng_type = MfClassicPrngTypeUnknown;
    instance->nfc_dict_context.saved_backdoor = MfClassicBackdoorUnknown;
    instance->nfc_dict_context.nested_target_key = 0;
    instance->nfc_dict_context.msb_count = 0;
    instance->nfc_dict_context.enhanced_dict = false;

    // Clean up temporary files used for nested dictionary attack
    storage_common_remove(instance->storage, NFC_APP_MF_CLASSIC_DICT_USER_NESTED_PATH);
    storage_common_remove(instance->storage, NFC_APP_MF_CLASSIC_DICT_SYSTEM_NESTED_PATH);

    nfc_blink_stop(instance);
}
