#include "furi_hal_pn532.h"
#include "furi_hal_nfc_pn532.h"
#include "furi_hal_i2c.h"
#include "furi_hal_nfc_i.h"
#include <furi_hal_cortex.h>
#include <stm32wbxx_ll_i2c.h>
#include <furi.h>
#include <string.h>
#include <inttypes.h>

#define TAG "FuriHalPN532"

#define PN532_PREAMBLE    0x00
#define PN532_STARTCODE1  0x00
#define PN532_STARTCODE2  0xFF
#define PN532_POSTAMBLE   0x00
#define PN532_I2C_SFI     0x00
#define PN532_HOSTTOPN532 0xD4
#define PN532_PN532TOHOST 0xD5
#define PN532_I2C_READY   0x01

#define PN532_CMD_GET_FIRMWARE_VERSION 0x02
#define PN532_CMD_SAM_CONFIGURATION    0x14
#define PN532_CMD_RF_CONFIGURATION     0x32
#define PN532_CMD_IN_DATA_EXCHANGE     0x40
#define PN532_CMD_IN_COMMUNICATE_THRU  0x42
#define PN532_CMD_IN_RELEASE           0x52
#define PN532_CMD_IN_LIST_PASSIVE      0x4A
#define PN532_CMD_WRITE_REGISTER       0x08
#define PN532_CMD_TG_INIT_AS_TARGET    0x8C
#define PN532_CMD_TG_GET_DATA          0x86
#define PN532_CMD_TG_SET_DATA          0x8E

/* CIU register for RF output drivers (needed for clone modules) */
#define PN532_REG_CIU_TxControl 0x6330
#define PN532_TXCONTROL_ENABLE \
    0x82 /* TX1RFEn | InitialRFOn (RFOff=0: keep RF on between commands) */

/* Compile-time backend capability bits for variant PN532-family chips.
 * Default 0 (genuine PN532). Override in board.mk for PN533/PN532Killer.
 * Modelled as backend capability, not as a runtime firmware-version probe,
 * because ISO15693 is not a native PN532 reader capability per NXP UM0701-02
 * and reference projects (nfcpy restricts InListPassiveTarget BrTy to 0-4). */
#ifndef FURI_HAL_PN532_CAPS
#define FURI_HAL_PN532_CAPS 0
#endif

#define FURI_HAL_PN532_CAP_ISO15693 (1u << 0)

static inline bool furi_hal_pn532_has_cap(uint8_t cap) {
    return (FURI_HAL_PN532_CAPS & cap) != 0;
}

#define PN532_I2C_RETRIES    3
#define PN532_MAX_TX_PAYLOAD 255
#define PN532_MAX_RX_FRAME   270

/* Timeout constants for PN532 operations */
#define PN532_TIMEOUT_ACK_MS \
    150 /* ACK frame response timeout —
                                          * increased to 150ms for shared I2C1
                                          * bus (PN532+PCF8574+OLED) with no
                                          * INT/RST pin; voltage drop during
                                          * read/write/emulate adds latency.     */
#define PN532_TIMEOUT_CMD_MS         300 /* FW version / SAM config (+50ms margin) */
#define PN532_TIMEOUT_POLL_MS        400 /* InListPassiveTarget poll (+50ms)       */
#define PN532_TIMEOUT_EXCHANGE_MS    1200 /* InDataExchange (1s→1.2s, voltage drop) */
#define PN532_TIMEOUT_EXCHANGE_4K_MS 1800 /* InDataExchange (MIFARE 4K →1.8s)     */
#define PN532_TIMEOUT_PRESENCE_MS    150 /* Fast presence re-poll (min 150ms)      */

/* DIY board transport invariant:
 * PN532 is wired only to I2C1 SCL/SDA through furi_hal_i2c_handle_power.
 * There are no PN532 INT/IRQ/RST GPIOs on this board, so readiness must be
 * detected by reading the PN532 I2C status byte (0x01 == READY). Do not add
 * furi_hal_i2c_handle_external/I2C3, GPIO IRQ, or hardware reset recovery here. */
#define PN532_USE_READY_POLLER 1
#define PN532_READY_POLL_INTERVAL_MS \
    25 /* 40Hz poll — no change needed (status check, not timeout) */

/* Retry backoff tables */
static const uint16_t pn532_write_backoff_ms[PN532_I2C_RETRIES] = {50, 100, 150};
static const uint16_t pn532_read_backoff_ms[PN532_I2C_RETRIES] = {10, 20, 30};

static const uint8_t pn532_ack_frame[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static volatile bool pn532_ready = false;
#define PN532_I2C_ADDR PN532_I2C_ADDR_7BIT
static uint8_t pn532_i2c_addr = PN532_I2C_ADDR;

/* Global exchange deadline (tick).  When non-zero, pn532_wait_ready_ms()
 * clamps its wait to the remaining time before the deadline, and
 * pn532_exchange() returns FuriHalPn532ErrorTimeout at the entry gate
 * once the deadline has passed.  Set by the supported-cards plugin loop
 * so no single plugin can exceed SUPPORTED_CARD_TOTAL_TIMEOUT_MS. */
static uint32_t pn532_exchange_deadline = 0;

typedef struct {
    uint32_t wait_calls;
    uint32_t wait_event_calls;
    uint32_t wait_event_hits;
    uint32_t wait_event_timeouts;
    uint32_t wait_poll_calls;
    uint32_t wait_poll_hits;
    uint32_t wait_poll_timeouts;
    uint32_t status_reads;
    uint32_t status_ready;
    uint32_t status_errors;
    uint32_t tx_frames;
    uint32_t tx_retries;
    uint32_t tx_failures;
    uint32_t ack_reads;
    uint32_t ack_wait_failures;
    uint32_t ack_rx_failures;
    uint32_t ack_mismatches;
    uint32_t response_reads;
    uint32_t response_timeouts;
    uint32_t response_invalid;
    uint32_t drain_calls;
    uint32_t drained_frames;
    uint32_t exchanges;
    uint32_t exchange_errors;
    uint32_t poller_starts;
    uint32_t poller_stops;
} FuriHalPn532DebugCounters;

static FuriHalPn532DebugCounters pn532_debug;

static void pn532_debug_reset(void) {
    memset(&pn532_debug, 0, sizeof(pn532_debug));
}

static void pn532_debug_log_summary(const char* reason) {
    FURI_LOG_D(
        TAG,
        "PN532 stats (%s): wait=%lu event=%lu hit=%lu timeout=%lu poll=%lu hit=%lu timeout=%lu",
        reason,
        (unsigned long)pn532_debug.wait_calls,
        (unsigned long)pn532_debug.wait_event_calls,
        (unsigned long)pn532_debug.wait_event_hits,
        (unsigned long)pn532_debug.wait_event_timeouts,
        (unsigned long)pn532_debug.wait_poll_calls,
        (unsigned long)pn532_debug.wait_poll_hits,
        (unsigned long)pn532_debug.wait_poll_timeouts);
    FURI_LOG_D(
        TAG,
        "PN532 stats (%s): status_reads=%lu ready=%lu errors=%lu tx=%lu retry=%lu tx_fail=%lu",
        reason,
        (unsigned long)pn532_debug.status_reads,
        (unsigned long)pn532_debug.status_ready,
        (unsigned long)pn532_debug.status_errors,
        (unsigned long)pn532_debug.tx_frames,
        (unsigned long)pn532_debug.tx_retries,
        (unsigned long)pn532_debug.tx_failures);
    FURI_LOG_D(
        TAG,
        "PN532 stats (%s): ack=%lu ack_wait_fail=%lu ack_rx_fail=%lu ack_bad=%lu resp=%lu resp_timeout=%lu resp_bad=%lu",
        reason,
        (unsigned long)pn532_debug.ack_reads,
        (unsigned long)pn532_debug.ack_wait_failures,
        (unsigned long)pn532_debug.ack_rx_failures,
        (unsigned long)pn532_debug.ack_mismatches,
        (unsigned long)pn532_debug.response_reads,
        (unsigned long)pn532_debug.response_timeouts,
        (unsigned long)pn532_debug.response_invalid);
    FURI_LOG_D(
        TAG,
        "PN532 stats (%s): drain_calls=%lu drained=%lu exchanges=%lu exchange_errors=%lu poller_start=%lu poller_stop=%lu",
        reason,
        (unsigned long)pn532_debug.drain_calls,
        (unsigned long)pn532_debug.drained_frames,
        (unsigned long)pn532_debug.exchanges,
        (unsigned long)pn532_debug.exchange_errors,
        (unsigned long)pn532_debug.poller_starts,
        (unsigned long)pn532_debug.poller_stops);
}

/* Software ready poller: a background thread polls the PN532 status byte over
 * I2C1 and signals pn532_ready_event when the chip has data ready
 * (status=0x01 READY). This is not a GPIO IRQ path; it uses only SCL/SDA.
 * It replaces the blocking poll loop in pn532_wait_ready_ms(), freeing the
 * RTOS scheduler and the I2C1 bus between poll intervals.
 *
 * Safety: a 1-byte I2C read only reads the status byte; the PN532 re-presents
 * the full response frame on the next complete read, so the poller thread
 * cannot accidentally consume ACK or response data. */
#define PN532_READY_POLLER_FLAG_READY (1u << 0) /* PN532 status=0x01 detected */
#define PN532_READY_POLLER_FLAG_STOP  (1u << 1) /* request thread shutdown   */
static FuriEventFlag* pn532_ready_event = NULL;
static FuriThread* pn532_ready_thread = NULL;

/* Drain ALL stale frames from the PN532 output buffer before a new write.
 * The PN532 I2C slave NACKs writes while it has pending output data (status=0x01).
 *
 * Bug that this fixes: if the PN532 queued TWO responses (e.g. a failed MfClassic
 * auth error frame followed by an abort-ACK frame), the old single-frame drain
 * left the second frame in place.  The PN532 kept status=0x01, causing NACK on
 * all subsequent writes → infinite TX retry storm → "Don't move" freeze.
 *
 * Fix: loop up to 4 times, draining one 270-byte frame per iteration, until
 * the status byte reads 0x00 (PN532 idle and ready to accept a new command).
 * A 5ms inter-frame settle delay lets the PN532 clear its ready latch before
 * the next status probe.
 *
 * 2026-05-25: raised cap to 8.  3 NfcSupportedCards plugins × 2 auth
 * attempts each on a HALTed MF Classic card can queue up to ~6 stale frames;
 * cap 4 was still insufficient.  8 covers the worst case with margin.
 * Each loop exits early once status reads idle so there is no fixed cost. */
static void pn532_drain_output(void) {
    pn532_debug.drain_calls++;
    static uint8_t drain[PN532_MAX_RX_FRAME];
    uint8_t frames_drained = 0;

    for(uint8_t i = 0; i < 8; i++) {
        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 20);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        pn532_debug.status_reads++;
        if(ok && status == PN532_I2C_READY) {
            pn532_debug.status_ready++;
        } else if(!ok) {
            pn532_debug.status_errors++;
        }

        if(!ok || status != PN532_I2C_READY) break; /* PN532 idle — nothing more to drain */

        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, drain, sizeof(drain), 150);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);

        frames_drained++;
        furi_delay_ms(10); /* shared I2C1 bus: allow PCF8574/display to finish */
    }

    if(frames_drained > 0) {
        pn532_debug.drained_frames += frames_drained;
        FURI_LOG_D(TAG, "drained %u stale PN532 frame(s)", frames_drained);
    }
}

/* NXP PN532 wakeup pulse — burst of 0x55 bytes on the I2C bus.
 * Per NXP UM0701-02 §6.2.4: a sequence of 0x55 bytes wakes the PN532
 * from a bus-locked state caused by a power glitch or reset without
 * a full power cycle.  This is the same pattern used by pn532-lib and
 * libnfc.  Best-effort: return value is ignored by callers; the
 * subsequent frame write is what we actually verify. */
static void pn532_wakeup_pulse(void) {
    static const uint8_t wakeup[] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pn532_i2c_addr, wakeup, sizeof(wakeup), 10);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    /* Shared I2C1 bus (PN532 + PCF8574 + OLED): settle before next frame */
    furi_delay_ms(20);
}

/* Note: furi_hal_i2c_bus_reset is NOT called on PN532 errors.
 * The 9 SCL pulse bus reset desyncs the PN532's I2C slave state machine,
 * causing permanent address NACK. With no hardware RST pin on this DIY board,
 * there is no way to recover once desynced. PN532 errors are treated as
 * transient timeouts and resolved by retrying the operation directly. */

/* Two-strikes-out ACK failure tracking.
 * A single ACK failure is almost always a transient I2C glitch (display
 * preempts the bus mid-transaction, button press triggers PCF8574 read,
 * etc).  Setting pn532_ready=false on a single failure causes downstream
 * guards like furi_hal_nfc_pn532_is_active() to report the chip as absent,
 * which can defeat MFC nested-attack guards and let the state machine spin.
 *
 * Rule: only mark the chip absent after two consecutive ACK failures
 * within a 200ms window.  Single failures are logged and ignored. */
#define PN532_ACK_STRIKES_WINDOW_MS 200
static uint8_t pn532_ack_strike_count = 0;
static uint32_t pn532_ack_strike_tick = 0;

static void pn532_ack_strike_record(void) {
    const uint32_t now = furi_get_tick();
    if((now - pn532_ack_strike_tick) > PN532_ACK_STRIKES_WINDOW_MS) {
        pn532_ack_strike_count = 1;
    } else {
        pn532_ack_strike_count++;
    }
    pn532_ack_strike_tick = now;
}

static void pn532_ack_strike_clear(void) {
    pn532_ack_strike_count = 0;
}

static bool pn532_ack_strikes_exceeded(void) {
    return pn532_ack_strike_count >= 2;
}

/* Two-strikes ACK failure handler.
 * Replaces the one-line `pn532_ready = false` after `pn532_read_ack()`
 * failure with the same transient-glitch-tolerant logic that
 * pn532_exchange() uses: only mark the chip absent after two ACK
 * failures in 200ms (verified by an I2C ping).  Single failures are
 * logged and ignored so a transient bus glitch from the display or
 * PCF8574 does not force a mid-session PN532 reinit cascade. */
static void pn532_handle_ack_failure(const char* site) {
    pn532_ack_strike_record();
    if(pn532_ack_strikes_exceeded()) {
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        uint8_t ping_status = 0;
        bool chip_alive =
            furi_hal_i2c_rx(&furi_hal_i2c_handle_power, PN532_I2C_ADDR_7BIT, &ping_status, 1, 10);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(!chip_alive) {
            pn532_ready = false;
            FURI_LOG_E(TAG, "%s: 2 ACK fails + ping fail — marking PN532 absent", site);
        } else {
            FURI_LOG_W(TAG, "%s: 2 ACK fails but chip ping ok, keeping ready", site);
        }
        pn532_ack_strike_clear();
    } else {
        FURI_LOG_W(TAG, "%s: ACK failed (transient, keeping ready)", site);
    }
}

static bool pn532_wait_ready_ms(uint32_t timeout_ms) {
    /* Apply global exchange deadline — cap wait to remaining time */
    if(pn532_exchange_deadline > 0) {
        uint32_t now = furi_get_tick();
        if((int32_t)(now - pn532_exchange_deadline) >= 0) {
            FURI_LOG_D(TAG, "wait_ready_ms: global deadline expired");
            return false;
        }
        uint32_t remaining = pn532_exchange_deadline - now;
        if(remaining < timeout_ms) timeout_ms = remaining;
    }
    pn532_debug.wait_calls++;
#if PN532_USE_READY_POLLER
    if(pn532_ready_event) {
        /* Software ready-poller path: wait for the I2C1 status poller thread
         * to detect status=0x01. No GPIO IRQ line is used. */
        pn532_debug.wait_event_calls++;
        uint32_t result = furi_event_flag_wait(
            pn532_ready_event,
            PN532_READY_POLLER_FLAG_READY,
            FuriFlagWaitAny | FuriFlagNoClear,
            timeout_ms);
        if((result & PN532_READY_POLLER_FLAG_READY) == 0) {
            pn532_debug.wait_event_timeouts++;
            /* Timeout or spurious wake — check for clean abort */
            if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
                /* Consume the abort flag so it does not persist into the next
                 * pn532_exchange() call — a stale flag would cause every
                 * subsequent probe to bail out immediately ("abort set, giving up").
                 * furi_hal_nfc_pn532_wait_event() also clears this flag, but it
                 * is not called in the direct-exchange (scanner) path. */
                furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
                FURI_LOG_D(TAG, "PN532 wait ready: abort (ready poller path)");
            } else {
                /* BUG FIX (NFC crash after MfUltralight 0x13 error):
                 * A single ready-poller timeout is almost always transient —
                 * the PN532 is in a protocol-error state (e.g. after returning
                 * status 0x13 for an unsupported command) but the hardware is
                 * alive.  Unconditionally setting pn532_ready=false here caused
                 * set_mode() to enter the recovery reinit path, which stopped
                 * and restarted the poller thread multiple times in quick succession,
                 * leading to a crash (heap exhaustion or NULL dereference).
                 * Fix: apply the same two-strikes logic as pn532_handle_ack_failure()
                 * — only mark the chip absent after two consecutive timeouts within
                 * 200ms AND a failed I2C ping confirms the chip is truly gone. */
                FURI_LOG_W(TAG, "PN532 wait ready: timeout (ready poller path)");
                pn532_handle_ack_failure("pn532_wait_ready_ms");
            }
            return false;
        }
        /* Consume the flag — the next caller must wait for the next READY signal */
        pn532_debug.wait_event_hits++;
        furi_event_flag_clear(pn532_ready_event, PN532_READY_POLLER_FLAG_READY);
        return true;
    }
#endif

    /* Blocking-poll fallback — used during furi_hal_pn532_init() before the
     * ready poller thread is started, and as a safety net if it is stopped. */
    pn532_debug.wait_poll_calls++;
    uint32_t start = furi_get_tick();
    for(uint8_t attempt = 0;; attempt++) {
        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 30);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        pn532_debug.status_reads++;
        if(ok && status == PN532_I2C_READY) {
            pn532_debug.status_ready++;
            pn532_debug.wait_poll_hits++;
            FURI_LOG_D(TAG, "PN532 ready after %d retries", attempt);
            return true;
        } else if(!ok) {
            pn532_debug.status_errors++;
        }
        if((furi_get_tick() - start) >= timeout_ms) {
            pn532_debug.wait_poll_timeouts++;
            if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
                furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
                FURI_LOG_D(TAG, "PN532 wait ready timeout (abort pending, not a hw error)");
            } else {
                /* BUG FIX (NFC crash after MfUltralight 0x13 error — blocking path):
                 * Same two-strikes logic as the ready-poller path above. The blocking
                 * fallback is used when the poller thread is not yet running.
                 * A single timeout here is almost always a transient condition — the
                 * PN532 is in a protocol-error state but the hardware is alive.
                 * Only mark the chip absent after two consecutive timeouts within 200ms
                 * AND a failed I2C ping confirms the chip is truly gone. */
                FURI_LOG_W(TAG, "PN532 wait ready timeout");
                pn532_handle_ack_failure("pn532_wait_ready_ms_poll");
            }
            return false;
        }
        if(attempt > 0 && (furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort)) {
            furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
            FURI_LOG_D(TAG, "PN532 wait ready: abort early exit after %d attempts", attempt);
            return false;
        }
        furi_delay_ms(30); /* shared I2C1 bus: allow competing devices to release */
    }
}

/* Software ready polling thread.
 * Runs at FuriThreadPriorityNormal+1 during NFC sessions.  Every 25ms it does
 * a 1-byte I2C read (the PN532 status byte).  When status=0x01 (READY) it sets
 * PN532_READY_POLLER_FLAG_READY on pn532_ready_event so pn532_wait_ready_ms()
 * can unblock.
 *
 * The 1-byte read is safe: the PN532 presents a fresh READY+frame sequence on
 * every new I2C transaction, so the single-byte status check does NOT consume
 * ACK or response frame data that pn532_read_ack()/pn532_read_response() need. */
static int32_t pn532_ready_poller_run(void* ctx) {
    UNUSED(ctx);
    FURI_LOG_D(TAG, "PN532 software ready poller started on I2C1 SCL/SDA");
    while(true) {
        /* Check for shutdown request */
        uint32_t tflags = furi_thread_flags_get();
        if(tflags & PN532_READY_POLLER_FLAG_STOP) break;

        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 10);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        pn532_debug.status_reads++;

        if(ok && status == PN532_I2C_READY) {
            pn532_debug.status_ready++;
            furi_event_flag_set(pn532_ready_event, PN532_READY_POLLER_FLAG_READY);
        } else if(!ok) {
            pn532_debug.status_errors++;
        }

        /* 40 Hz poll (was 100 Hz).  The 1-byte I2C status read costs ~30µs
         * per cycle but contends with foreground scanner and display I/O on
         * the shared I2C1 bus.  25ms is well below the PN532's command
         * round-trip times (>= 5ms) so we never miss a READY edge.
         * Keep I2C RX timeout tight (10ms) — a 1-byte read at 100kHz takes
         * ~400µs; 10ms generously covers bus contention.  A longer timeout
         * here would starve OLED/PCF8574 on shared I2C1 since poller runs
         * every 25ms (40% bus utilization at 10ms). */
        furi_delay_ms(PN532_READY_POLL_INTERVAL_MS);
    }
    FURI_LOG_D(TAG, "PN532 software ready poller stopped");
    return 0;
}

static bool pn532_probe_address(void) {
    /* Try both known PN532 I2C addresses: 0x48 (A0=1,A1=0) and 0x24 (A0=0,A1=1).
     * Clone modules or bus glitches can leave the chip responding on either. */
    static const uint8_t probe_addrs[] = {PN532_I2C_ADDR_7BIT, 0x24};
    for(size_t i = 0; i < sizeof(probe_addrs); i++) {
        uint8_t status = 0;
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool found = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, probe_addrs[i], &status, 1, 150);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        pn532_debug.status_reads++;

        if(found) {
            if(status == PN532_I2C_READY) pn532_debug.status_ready++;
            pn532_i2c_addr = probe_addrs[i];
            FURI_LOG_I(TAG, "PN532 found at 0x%02X on I2C1", pn532_i2c_addr);
            return true;
        }
        pn532_debug.status_errors++;
    }
    FURI_LOG_W(TAG, "PN532 not responding at any known address");
    return false;
}

static bool pn532_write_frame(const uint8_t* cmd, size_t cmd_len) {
    if(cmd_len > PN532_MAX_TX_PAYLOAD) return false;

    static uint8_t frame[PN532_MAX_TX_PAYLOAD + 10];
    size_t pos = 0;
    uint8_t checksum = 0;
    furi_check(cmd_len < PN532_MAX_TX_PAYLOAD - 1);
    const uint8_t len = (uint8_t)(cmd_len + 1);

    /* PN532 frame format: PREAMBLE + STARTCODE + LEN + LCS + DATA[HOSTTOPN532 + cmd] + DCS + POSTAMBLE */
    frame[pos++] = PN532_PREAMBLE;
    frame[pos++] = PN532_STARTCODE1;
    frame[pos++] = PN532_STARTCODE2;
    frame[pos++] = len;
    frame[pos++] = (uint8_t)(~len + 1);
    frame[pos++] = PN532_HOSTTOPN532;
    checksum += PN532_HOSTTOPN532;

    for(size_t i = 0; i < cmd_len; i++) {
        frame[pos++] = cmd[i];
        checksum += cmd[i];
    }

    frame[pos++] = (uint8_t)(~checksum + 1);
    frame[pos++] = PN532_POSTAMBLE;

    /* Give PN532 2ms to switch from address-ACK mode to receive mode.
     * One I2C byte-time at 100kHz is ~90µs; 2ms is well above spec and
     * sufficient for clone boards with 4.7kΩ pull-ups to 3.3V.
     * Increased to 10ms for shared I2C1 bus (PN532+PCF8574+OLED), voltage drop. */
    furi_delay_ms(10);

    /* Pre-write wakeup pulse: catches the case where the PN532 is in a
     * bus-locked state from a previous incomplete transaction.  Cheap
     * (8 bytes I2C) and required by pn532-lib / libnfc recovery pattern. */
    pn532_wakeup_pulse();

    /* Drain stale output before writing — avoids write-NACK when PN532
     * has pending data from a previous incomplete read.
     * Clear stale READY flag first — poller thread may have set it for data
     * we're about to drain, causing pn532_read_ack() to return prematurely.
     * Guard NULL: pn532_ready_event is allocated late in pn532_init_internal(),
     * but pn532_write_frame() is called before that during firmware query. */
    if(pn532_ready_event) {
        furi_event_flag_clear(pn532_ready_event, PN532_READY_POLLER_FLAG_READY);
    }
    pn532_drain_output();

    for(uint8_t attempt = 0; attempt < PN532_I2C_RETRIES; attempt++) {
        if(attempt > 0) {
            pn532_debug.tx_retries++;
            static const uint8_t ack_cancel[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
            furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
            furi_hal_i2c_tx(
                &furi_hal_i2c_handle_power, pn532_i2c_addr, ack_cancel, sizeof(ack_cancel), 10);
            furi_hal_i2c_release(&furi_hal_i2c_handle_power);
            furi_delay_ms(20); /* shared I2C1 bus: allow bus to settle between retries */
        }
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        bool ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pn532_i2c_addr, frame, pos, 150);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        if(ok) {
            pn532_debug.tx_frames++;
            return true;
        }
        FURI_LOG_W(TAG, "I2C TX retry %u/%u", attempt + 1, PN532_I2C_RETRIES);
        furi_delay_ms(pn532_write_backoff_ms[attempt]);
    }
    pn532_debug.tx_failures++;
    FURI_LOG_E(TAG, "TX frame failed after %u retries", PN532_I2C_RETRIES);

    /* Final wakeup attempt: one more frame write after a NXP wakeup pulse.
     * Catches the most common real-world I2C lockup (post power-glitch, post
     * reset without power cycle).  If this also fails, fall through to the
     * existing reinit path — no SAM reconfiguration here, that is the
     * caller's responsibility via furi_hal_pn532_init(). */
    pn532_wakeup_pulse();
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool wakeup_ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_power, pn532_i2c_addr, frame, pos, 150);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    if(wakeup_ok) {
        pn532_debug.tx_frames++;
        FURI_LOG_I(TAG, "TX frame recovered via wakeup pulse");
        return true;
    }
    FURI_LOG_D(TAG, "Wakeup recovery failed; propagating error");

    /* Mark PN532 as needing reinit.  The next pn532_exchange() call will invoke
     * furi_hal_pn532_init() (SAM config) before attempting any further commands.
     * Without this, the caller keeps retrying writes to a stuck chip. */
    pn532_ready = false;
    return false;
}

static bool pn532_read_ack(void) {
    pn532_debug.ack_reads++;
    uint8_t buf[7] = {0};
    if(!pn532_wait_ready_ms(PN532_TIMEOUT_ACK_MS)) {
        pn532_debug.ack_wait_failures++;
        FURI_LOG_E(TAG, "pn532_read_ack: wait_ready failed");
        return false;
    }

    furi_delay_ms(10); /* shared I2C1 bus: settle before RX (PCF8574/display may contend) */

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = false;
    for(uint8_t retry = 0; retry < PN532_I2C_RETRIES; retry++) {
        ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, buf, sizeof(buf), 150);
        if(!ok) {
            pn532_debug.ack_rx_failures++;
            FURI_LOG_W(TAG, "pn532_read_ack: RX failed, retry %u/3", retry + 1);
            furi_hal_i2c_release(&furi_hal_i2c_handle_power);
            furi_delay_ms(pn532_read_backoff_ms[retry]);
            furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
            continue;
        }
        if(buf[0] == PN532_I2C_READY) {
            break;
        }
        FURI_LOG_W(TAG, "pn532_read_ack: status 0x%02X != 0x01, retry %u/3", buf[0], retry + 1);
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        furi_delay_ms(pn532_read_backoff_ms[retry]);
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok || buf[0] != PN532_I2C_READY) {
        pn532_debug.ack_rx_failures++;
        FURI_LOG_E(TAG, "pn532_read_ack: failed after retries, buf[0]=0x%02X ok=%d", buf[0], ok);
        return false;
    }
    bool match = memcmp(&buf[1], pn532_ack_frame, sizeof(pn532_ack_frame)) == 0;
    if(!match) {
        pn532_debug.ack_mismatches++;
        FURI_LOG_E(TAG, "pn532_read_ack: ACK mismatch");
        FURI_LOG_E(
            TAG,
            "  got:      %02X %02X %02X %02X %02X %02X",
            buf[1],
            buf[2],
            buf[3],
            buf[4],
            buf[5],
            buf[6]);
        FURI_LOG_E(
            TAG,
            "  expected: %02X %02X %02X %02X %02X %02X",
            pn532_ack_frame[0],
            pn532_ack_frame[1],
            pn532_ack_frame[2],
            pn532_ack_frame[3],
            pn532_ack_frame[4],
            pn532_ack_frame[5]);
    }
    return match;
}

static FuriHalPn532Error
    pn532_read_raw_response(uint8_t* rx, size_t rx_size, uint32_t timeout_ms) {
    if(!rx || rx_size < 8) return FuriHalPn532ErrorInvalidFrame;
    pn532_debug.response_reads++;
    if(!pn532_wait_ready_ms(timeout_ms)) {
        pn532_debug.response_timeouts++;
        return FuriHalPn532ErrorTimeout;
    }

    furi_delay_ms(10); /* shared I2C1 bus: settle before RX (PCF8574/display may contend) */

    bool ok = false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);

    // Read the maximum possible frame in a single transaction.
    // PN532 I2C requires reading the entire frame in one go, as every new read transaction
    // starts with a READY byte, and incomplete reads will cause desync.
    // 150ms for shared I2C1 bus (PN532+PCF8574+OLED), no INT/RST pin, voltage drop.
    ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, rx, rx_size, 150);

    uint8_t retry_count = 0;
    while(!ok || rx[0] != PN532_I2C_READY) {
        if(retry_count >= PN532_I2C_RETRIES) break;
        retry_count++;
        furi_hal_i2c_release(&furi_hal_i2c_handle_power);
        furi_delay_ms(pn532_read_backoff_ms[retry_count - 1]);
        furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
        ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, rx, rx_size, 150);
    }

    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    if(!ok || rx[0] != PN532_I2C_READY) {
        pn532_debug.response_timeouts++;
        return FuriHalPn532ErrorTimeout;
    }

    return FuriHalPn532ErrorNone;
}

static FuriHalPn532Error pn532_parse_response_frame(
    const uint8_t* rx,
    size_t rx_size,
    uint8_t* payload,
    size_t payload_size,
    size_t* out_len) {
    if(!rx || rx_size < 8 || !payload) {
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }
    if(rx[0] != PN532_I2C_READY) {
        pn532_debug.response_timeouts++;
        return FuriHalPn532ErrorTimeout;
    }

    const uint8_t* frame = &rx[1];
    const size_t frame_size = rx_size - 1;

    if(frame[0] != PN532_PREAMBLE || frame[1] != PN532_STARTCODE1 ||
       frame[2] != PN532_STARTCODE2) {
        FURI_LOG_E(
            TAG, "pn532_read_response: bad header %02X %02X %02X", frame[0], frame[1], frame[2]);
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    uint8_t len = frame[3];
    uint8_t lcs = frame[4];
    size_t header_offset = 5; // offset of TFI after LEN+LCS

    if(len == 0xFFU) {
        /* Extended frame: LEN=0xFF, followed by LEN_HI, LEN_LO, ELCS.
         * Real length = (LEN_HI << 8) | LEN_LO. */
        if(frame_size < 7) {
            FURI_LOG_E(TAG, "pn532_read_response: extended frame too short");
            pn532_debug.response_invalid++;
            return FuriHalPn532ErrorInvalidFrame;
        }
        uint16_t ext_len = ((uint16_t)frame[5] << 8) | frame[6];
        uint8_t ext_lcs = frame[7];
        if((uint8_t)((frame[5] + frame[6]) + ext_lcs) != 0) {
            FURI_LOG_E(TAG, "pn532_read_response: bad extended LCS");
            pn532_debug.response_invalid++;
            return FuriHalPn532ErrorInvalidFrame;
        }
        if(ext_len < 2) {
            pn532_debug.response_invalid++;
            return FuriHalPn532ErrorInvalidFrame;
        }
        len = (ext_len > 254) ? 254 : (uint8_t)ext_len; // clamp for safety
        lcs = ext_lcs;
        header_offset = 8; // TFI after extended header
        FURI_LOG_W(TAG, "pn532_read_response: extended frame len=%u", ext_len);
    }
    if((uint8_t)(len + lcs) != 0) {
        FURI_LOG_E(TAG, "pn532_read_response: bad LCS %02X + %02X", len, lcs);
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    if(len < 2) {
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    const size_t required_frame_size = header_offset + 1U + len;
    if(required_frame_size > frame_size) {
        FURI_LOG_E(
            TAG, "pn532_read_response: short frame (%zu < %zu)", frame_size, required_frame_size);
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    if(frame[header_offset] != PN532_PN532TOHOST) {
        FURI_LOG_E(
            TAG, "pn532_read_response: bad body[0]=0x%02X (expected 0xD5)", frame[header_offset]);
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    size_t content_len = len - 1;
    if(content_len > payload_size) {
        FURI_LOG_E(
            TAG,
            "pn532_read_response: content_len (%zu) > payload_size (%zu)",
            content_len,
            payload_size);
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorBufferOverflow;
    }

    uint8_t dcs = frame[header_offset + len];
    uint8_t checksum = 0;
    for(size_t i = 0; i < len; i++) {
        checksum += frame[header_offset + i];
    }
    if((uint8_t)(checksum + dcs) != 0) {
        FURI_LOG_E(TAG, "DCS checksum failure");
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    if(frame[header_offset + 1 + len] != PN532_POSTAMBLE) {
        FURI_LOG_E(
            TAG, "pn532_read_response: bad postamble 0x%02X", frame[header_offset + 1 + len]);
        pn532_debug.response_invalid++;
        return FuriHalPn532ErrorInvalidFrame;
    }

    memcpy(payload, &frame[header_offset + 1], content_len);
    if(out_len) *out_len = content_len;
    return FuriHalPn532ErrorNone;
}

static FuriHalPn532Error pn532_read_response_ex(
    uint8_t* payload,
    size_t payload_size,
    size_t* out_len,
    uint32_t timeout_ms) {
    /* Static buffer: eliminates 270 bytes from the NFC worker task stack.
     * Safe because: (1) callers are serialized under the I2C acquire/release
     * mutex — only one exchange is in flight at a time; (2) this function is
     * never called from the ready poller thread or from interrupt context;
     * (3) buffer is overwritten by pn532_read_raw_response() before any read. */
    static uint8_t rx[PN532_MAX_RX_FRAME];
    memset(rx, 0, sizeof(rx));
    FuriHalPn532Error error = pn532_read_raw_response(rx, sizeof(rx), timeout_ms);
    if(error != FuriHalPn532ErrorNone) return error;
    return pn532_parse_response_frame(rx, sizeof(rx), payload, payload_size, out_len);
}

static bool pn532_read_response(
    uint8_t* payload,
    size_t payload_size,
    size_t* out_len,
    uint32_t timeout_ms) {
    return pn532_read_response_ex(payload, payload_size, out_len, timeout_ms) ==
           FuriHalPn532ErrorNone;
}

/* Write a register in the PN532 CIU register bank */
static bool pn532_write_register(uint16_t reg, uint8_t value) {
    uint8_t cmd[] = {
        PN532_CMD_WRITE_REGISTER,
        (uint8_t)(reg >> 8), /* High byte */
        (uint8_t)(reg & 0xFF), /* Low byte */
        value};
    if(!pn532_write_frame(cmd, sizeof(cmd))) {
        FURI_LOG_W(TAG, "WriteRegister 0x%04X write failed", reg);
        return false;
    }
    if(!pn532_read_ack()) {
        uint8_t dummy_buf[PN532_MAX_RX_FRAME];
        size_t dummy_len;
        pn532_read_response(dummy_buf, sizeof(dummy_buf), &dummy_len, PN532_TIMEOUT_ACK_MS);
        FURI_LOG_W(TAG, "WriteRegister 0x%04X ACK failed", reg);
        return false;
    }
    /* Read and discard response */
    uint8_t dummy[8];
    size_t dummy_len = 0;
    if(!pn532_read_response(dummy, sizeof(dummy), &dummy_len, PN532_TIMEOUT_ACK_MS)) {
        return false;
    }
    return true;
}

static uint32_t pn532_last_init_fail_tick = 0;

static bool pn532_init_internal(void) {
    /* Stop the software ready poller thread if it is still running from a previous
     * session.  It must not be polling while we reset pn532_ready and
     * take the I2C bus for SAM configuration. */
    furi_hal_pn532_irq_stop();
    pn532_debug_reset();

    pn532_ready = false;
    /* Clear any stale abort flag left from a previous NFC session */
    furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
    FURI_LOG_I(
        TAG,
        "PN532 init start (transport=I2C1 SCL/SDA, ready=%s, irq_gpio=none, rst_gpio=none)",
        PN532_USE_READY_POLLER ? "software-poller" : "blocking-poll");
    if(!pn532_probe_address()) {
        FURI_LOG_W(TAG, "PN532 not detected on I2C");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 detected at 0x%02X, querying firmware...", pn532_i2c_addr);

    uint8_t cmd_fw[] = {PN532_CMD_GET_FIRMWARE_VERSION};
    if(!pn532_write_frame(cmd_fw, sizeof(cmd_fw)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 firmware command failed");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 firmware ACK received");
    uint8_t fw_rsp[24];
    size_t fw_len = 0;
    if(!pn532_read_response(fw_rsp, sizeof(fw_rsp), &fw_len, PN532_TIMEOUT_CMD_MS) || fw_len < 5 ||
       fw_rsp[0] != (PN532_CMD_GET_FIRMWARE_VERSION + 1)) {
        FURI_LOG_E(TAG, "PN532 invalid firmware response");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 firmware response OK");

    uint8_t cmd_sam[] = {PN532_CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    if(!pn532_write_frame(cmd_sam, sizeof(cmd_sam)) || !pn532_read_ack()) {
        FURI_LOG_E(TAG, "PN532 SAM config command failed");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 SAM ACK received");
    uint8_t sam_rsp[16];
    size_t sam_len = 0;
    if(!pn532_read_response(sam_rsp, sizeof(sam_rsp), &sam_len, PN532_TIMEOUT_CMD_MS) ||
       sam_len < 1 || sam_rsp[0] != (PN532_CMD_SAM_CONFIGURATION + 1)) {
        FURI_LOG_E(TAG, "PN532 invalid SAM response");
        return false;
    }
    FURI_LOG_I(TAG, "PN532 SAM configured");

    uint8_t dummy[8];
    size_t dummy_len = 0;

    /* RFConfiguration: Enable RF field (RFCFG_FIELD=1) — critical for clone modules */
    uint8_t cmd_rf_on[] = {PN532_CMD_RF_CONFIGURATION, 0x01, 0x01};
    if(!pn532_write_frame(cmd_rf_on, sizeof(cmd_rf_on))) {
        FURI_LOG_W(TAG, "RFConfiguration FIELD_ON write failed");
    } else if(!pn532_read_ack()) {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, PN532_TIMEOUT_ACK_MS);
        FURI_LOG_W(TAG, "RFConfiguration FIELD_ON ACK failed");
    } else {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, PN532_TIMEOUT_CMD_MS);
        FURI_LOG_I(TAG, "RFConfiguration FIELD_ON applied");
    }

    /* RFConfiguration: Max retries for clone module reliability
     * MxRtyATR=0xFF (not used, P2P only), MxRtyPSL=0x01, MxRtyPassiveActivation=0x05
     * PassiveActivation=0x05 gives clone PN532 more internal retries per
     * InListPassiveTarget. Bank cards (ISO-DEP) need more RF energy harvesting
     * time before responding to REQA. 2 retries was too few. */
    uint8_t cmd_retry[] = {PN532_CMD_RF_CONFIGURATION, 0x05, 0xFF, 0x01, 0x05};
    if(!pn532_write_frame(cmd_retry, sizeof(cmd_retry))) {
        FURI_LOG_W(TAG, "RFConfiguration retries config write failed");
    } else if(!pn532_read_ack()) {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, PN532_TIMEOUT_ACK_MS);
        FURI_LOG_W(TAG, "RFConfiguration retries config ACK failed");
    } else {
        pn532_read_response(dummy, sizeof(dummy), &dummy_len, PN532_TIMEOUT_CMD_MS);
        FURI_LOG_I(TAG, "RFConfiguration max retries applied");
    }
    furi_delay_ms(1000); /* Clone module: RF field needs 1000ms to stabilize after RFConfiguration.
                         * Original PN532 needs ~50ms, but clone modules with weak power rail
                         * (no bulk caps) need 20x longer for RF oscillator startup. */

    /* Enable RF output drivers (critical for clone modules) */
    /* CIU_TxControl: TX1RFEn | TX2RFEn | InitialRFOn */
    if(!pn532_write_register(PN532_REG_CIU_TxControl, PN532_TXCONTROL_ENABLE)) {
        FURI_LOG_W(TAG, "TxControl write failed, continuing anyway");
    } else {
        FURI_LOG_I(TAG, "RF output drivers enabled");
    }
    furi_delay_ms(500); /* Clone module: RF output drivers need 500ms to fully engage.
                         * Without this, InListPassiveTarget sends WUPA/REQA before
                         * the RF carrier is stable → card sees corrupted frame → no response. */

    uint8_t post_init_status = 0;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool status_ok =
        furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &post_init_status, 1, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);
    FURI_LOG_I(TAG, "Post-init PN532 status: 0x%02X (ok=%d)", post_init_status, status_ok);

    pn532_ready = true;
    FURI_LOG_I(TAG, "PN532 initialized over I2C1 at 0x%02X", pn532_i2c_addr);

    /* Start the software ready polling thread now that the PN532 is ready.
     * From this point, pn532_wait_ready_ms() will use the event-flag path
     * instead of the blocking poll loop. */
    furi_hal_pn532_irq_start();

    return true;
}

bool furi_hal_pn532_init(void) {
    uint32_t now = furi_get_tick();
    if(pn532_last_init_fail_tick > 0 &&
       (now - pn532_last_init_fail_tick) < furi_ms_to_ticks(500)) {
        return false;
    }
    bool ok = pn532_init_internal();
    if(ok) {
        pn532_last_init_fail_tick = 0;
    } else {
        pn532_last_init_fail_tick = furi_get_tick();
    }
    return ok;
}

void furi_hal_pn532_irq_start(void) {
#if PN532_USE_READY_POLLER
    if(pn532_ready_thread) return; /* already running */
    /* BUG FIX (MF Classic retry crash): if pn532_ready_event is non-NULL but
     * pn532_ready_thread is NULL (stale event from a previous session that was
     * not fully cleaned up), the original furi_check(!event) would
     * crash.  Free the stale event defensively instead. */
    if(pn532_ready_event) {
        FURI_LOG_W(TAG, "ready poller start: stale event found, freeing before restart");
        furi_event_flag_free(pn532_ready_event);
        pn532_ready_event = NULL;
    }
    pn532_ready_event = furi_event_flag_alloc();
    if(!pn532_ready_event) {
        FURI_LOG_E(TAG, "irq_start: event flag alloc failed");
        return;
    }
    pn532_ready_thread = furi_thread_alloc_ex("Pn532Ready", 768, pn532_ready_poller_run, NULL);
    if(!pn532_ready_thread) {
        FURI_LOG_E(TAG, "irq_start: thread alloc failed");
        furi_event_flag_free(pn532_ready_event);
        pn532_ready_event = NULL;
        return;
    }
    furi_thread_set_priority(pn532_ready_thread, FuriThreadPriorityNormal + 1);
    furi_thread_start(pn532_ready_thread);
    pn532_debug.poller_starts++;
    FURI_LOG_D(TAG, "PN532 software ready poller started");
#else
    FURI_LOG_D(TAG, "PN532 software ready poller disabled; using blocking status polling");
#endif
}

void furi_hal_pn532_irq_stop(void) {
#if PN532_USE_READY_POLLER
    if(!pn532_ready_thread) return;
    furi_thread_flags_set(furi_thread_get_id(pn532_ready_thread), PN532_READY_POLLER_FLAG_STOP);
    furi_thread_join(pn532_ready_thread);
    furi_thread_free(pn532_ready_thread);
    pn532_ready_thread = NULL;
    furi_event_flag_free(pn532_ready_event);
    pn532_ready_event = NULL;
    pn532_debug.poller_stops++;
    FURI_LOG_D(TAG, "PN532 software ready poller stopped");
    pn532_debug_log_summary("poller-stop");
#endif
}

bool furi_hal_pn532_is_ready(void) {
    return pn532_ready;
}

bool furi_hal_pn532_read_status(void) {
    if(!pn532_ready) return false;

    uint8_t status = 0;
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
    bool ok = furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, &status, 1, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_power);

    return ok && (status == PN532_I2C_READY);
}

const char* furi_hal_pn532_error_str(FuriHalPn532Error err) {
    switch(err) {
    case FuriHalPn532ErrorNone:
        return "None";
    case FuriHalPn532ErrorTimeout:
        return "Timeout";
    case FuriHalPn532ErrorComm:
        return "Comm";
    case FuriHalPn532ErrorInvalidAck:
        return "InvalidAck";
    case FuriHalPn532ErrorInvalidFrame:
        return "InvalidFrame";
    case FuriHalPn532ErrorBufferOverflow:
        return "BufferOverflow";
    case FuriHalPn532ErrorUnsupported:
        return "Unsupported";
    case FuriHalPn532ErrorAuth:
        return "Auth";
    case FuriHalPn532ErrorReleased:
        return "Released";
    case FuriHalPn532ErrorContext:
        return "Context";
    case FuriHalPn532ErrorParity:
        return "Parity";
    case FuriHalPn532ErrorBitCount:
        return "BitCount";
    case FuriHalPn532ErrorBufferSize:
        return "BufferSize";
    case FuriHalPn532ErrorRFBufferOverflow:
        return "RFBufferOverflow";
    case FuriHalPn532ErrorFieldNotSwitched:
        return "FieldNotSwitched";
    case FuriHalPn532ErrorRFProtocol:
        return "RFProtocol";
    case FuriHalPn532ErrorTemperature:
        return "Temperature";
    case FuriHalPn532ErrorInvalidParameter:
        return "InvalidParameter";
    case FuriHalPn532ErrorDEPInvalidCommand:
        return "DEPInvalidCommand";
    case FuriHalPn532ErrorDEPBadData:
        return "DEPBadData";
    case FuriHalPn532ErrorNoSecure:
        return "NoSecure";
    case FuriHalPn532ErrorI2CBusy:
        return "I2CBusy";
    case FuriHalPn532ErrorUIDChecksum:
        return "UIDChecksum";
    case FuriHalPn532ErrorDEPInvalidState:
        return "DEPInvalidState";
    case FuriHalPn532ErrorHCIInvalid:
        return "HCIInvalid";
    case FuriHalPn532ErrorCardSwapped:
        return "CardSwapped";
    case FuriHalPn532ErrorNoCard:
        return "NoCard";
    case FuriHalPn532ErrorDEPMismatch:
        return "DEPMismatch";
    case FuriHalPn532ErrorOverCurrent:
        return "OverCurrent";
    case FuriHalPn532ErrorNADMissing:
        return "NADMissing";
    default:
        return "Unknown";
    }
}

const char* furi_hal_pn532_strerror(uint8_t status_code) {
    switch(status_code) {
    case PN532_STATUS_SUCCESS:
        return "No error";
    case PN532_STATUS_TIMEOUT:
        return "Timeout";
    case PN532_STATUS_CRC:
        return "CRC error";
    case PN532_STATUS_PARITY:
        return "Parity error";
    case PN532_STATUS_COLLISION_BITCOUNT:
        return "Collision during bit count";
    case PN532_STATUS_MIFARE_FRAMING:
        return "MIFARE framing error";
    case PN532_STATUS_COLLISION_BITCOLLISION:
        return "Collision during bit collision";
    case PN532_STATUS_NOBUFS:
        return "RF buffer overflow";
    case PN532_STATUS_RFNOBUFS:
        return "RF no buffer space";
    case PN532_STATUS_ACTIVE_TOOSLOW:
        return "Active mode too slow";
    case PN532_STATUS_RFPROTO:
        return "RF protocol error";
    case PN532_STATUS_TOOHOT:
        return "Temperature too high";
    case PN532_STATUS_INTERNAL_NOBUFS:
        return "Internal buffer overflow";
    case PN532_STATUS_INVAL:
        return "Invalid parameter";
    case PN532_STATUS_DEP_INVALID_COMMAND:
        return "DEP invalid command";
    case PN532_STATUS_DEP_BADDATA:
        return "DEP bad data";
    case PN532_STATUS_MIFARE_AUTH:
        return "MIFARE authentication failed";
    case PN532_STATUS_NOSECURE:
        return "Key not valid / not secure";
    case PN532_STATUS_I2CBUSY:
        return "I2C bus busy";
    case PN532_STATUS_UIDCHECKSUM:
        return "UID checksum error";
    case PN532_STATUS_DEPSTATE:
        return "DEP state error";
    case PN532_STATUS_HCIINVAL:
        return "HCI invalid";
    case PN532_STATUS_CONTEXT:
        return "Context error";
    case PN532_STATUS_RELEASED:
        return "Released";
    case PN532_STATUS_CARDSWAPPED:
        return "Card swapped";
    case PN532_STATUS_NOCARD:
        return "No card available";
    case PN532_STATUS_MISMATCH:
        return "Mismatch";
    case PN532_STATUS_OVERCURRENT:
        return "Overcurrent";
    case PN532_STATUS_NONAD:
        return "Non-AD (anti-collision)";
    default:
        return "Unknown error";
    }
}

/* Consecutive exchange failure counter (time-windowed).
 * Tracks sequential exchange failures (ACK fail, response timeout, bad frame)
 * within a 5-second sliding window.  After N failures within that window,
 * pn532_ready is forced to false to trigger a full PN532 reinit via
 * furi_hal_pn532_init().  Resets on any successful exchange.
 *
 * The time window prevents false positives from spaced-out timeouts —
 * three failures hours apart should NOT trigger a reinit.  The 5-second
 * window matches typical NFC transaction duration: if the PN532 fails
 * 3 exchanges within 5 seconds, it is likely in a protocol-error state
 * that needs reinit.  If failures are spaced >5s apart, they are likely
 * transient (user moved card away, temporary RF field dropout). */
#define PN532_EXCHANGE_FAIL_LIMIT     3
#define PN532_EXCHANGE_FAIL_WINDOW_MS 5000
static uint8_t pn532_exchange_fail_count = 0;
static uint32_t pn532_exchange_fail_first_tick = 0;

static void pn532_exchange_fail_record(void) {
    const uint32_t now = furi_get_tick();
    if(pn532_exchange_fail_first_tick == 0) {
        pn532_exchange_fail_first_tick = now;
    } else if((now - pn532_exchange_fail_first_tick) > PN532_EXCHANGE_FAIL_WINDOW_MS) {
        /* Window expired — reset counter and start a new window */
        pn532_exchange_fail_count = 0;
        pn532_exchange_fail_first_tick = now;
    }
    pn532_exchange_fail_count++;
    if(pn532_exchange_fail_count >= PN532_EXCHANGE_FAIL_LIMIT) {
        pn532_ready = false;
        pn532_exchange_fail_count = 0;
        pn532_exchange_fail_first_tick = 0;
        FURI_LOG_W(
            TAG,
            "%d exchange failures in %dms window — forcing PN532 reinit",
            PN532_EXCHANGE_FAIL_LIMIT,
            PN532_EXCHANGE_FAIL_WINDOW_MS);
    }
}

void furi_hal_pn532_force_reinit(void) {
    pn532_ready = false;
}

void furi_hal_pn532_set_exchange_deadline(uint32_t deadline_tick) {
    pn532_exchange_deadline = deadline_tick;
}

void furi_hal_pn532_clear_exchange_deadline(void) {
    pn532_exchange_deadline = 0;
}

static FuriHalPn532Error pn532_exchange(
    const uint8_t* cmd,
    size_t cmd_len,
    uint8_t expected_response,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms) {
    /* Fast-fail: if global deadline already passed, skip the exchange entirely
     * without touching the PN532 or wasting I2C bus time. */
    if(pn532_exchange_deadline > 0) {
        uint32_t now = furi_get_tick();
        if((int32_t)(now - pn532_exchange_deadline) >= 0) {
            FURI_LOG_D(
                TAG, "pn532_exchange: global deadline expired, skipping cmd=0x%02X", cmd[0]);
            return FuriHalPn532ErrorTimeout;
        }
    }

    FURI_LOG_D(
        TAG,
        "pn532_exchange: ENTER cmd=0x%02X ready=%d timeout=%lu",
        cmd[0],
        pn532_ready,
        timeout_ms);
    pn532_debug.exchanges++;
    bool did_reinit = false;
    if(!pn532_ready) {
        if(!furi_hal_pn532_init()) {
            pn532_debug.exchange_errors++;
            FURI_LOG_E(TAG, "pn532_exchange: PN532 not ready and init failed");
            return FuriHalPn532ErrorComm;
        }
        did_reinit = true;
    }

    /* PN532 settle after full reinit — SAM config + RF init take time.
     * Without this, the first command after reinit can NACK or timeout.
     * 500ms for clone module settle after full reinit (was 20ms). */
    if(did_reinit) {
        furi_delay_ms(500);
    }

    /* NOTE: I2C bus drain removed — pn532_drain_output() inside pn532_write_frame()
     * already handles stale frame drain (8 iterations x 270 bytes). The old drain
     * raced with the ready poller thread's READY flag, causing spurious ACK retries. */

    if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
        /* Consume the abort flag — leaving it set would cause every subsequent
         * pn532_exchange() in this thread to immediately fail, even after the
         * abort condition (nfc_stop) has passed and a new session started. */
        furi_thread_flags_clear(FuriHalNfcEventInternalTypeAbort);
        pn532_debug.exchange_errors++;
        FURI_LOG_D(TAG, "pn532_exchange: abort set, giving up");
        return FuriHalPn532ErrorComm;
    }

    if(!pn532_write_frame(cmd, cmd_len)) {
        pn532_debug.exchange_errors++;
        FURI_LOG_D(TAG, "pn532_exchange: write failed");
        return FuriHalPn532ErrorComm;
    }

    if(!pn532_read_ack()) {
        pn532_debug.exchange_errors++;
        FURI_LOG_D(TAG, "pn532_exchange: ACK failed");
        /* Two-strikes-out: a single ACK failure is almost always a transient
         * I2C glitch (display/PCF8574 preempting the bus).  Only do the
         * "chip alive" ping and mark absent if we've seen two failures within
         * 200ms — that pattern indicates real chip trouble, not bus contention. */
        pn532_handle_ack_failure("pn532_exchange");
        pn532_exchange_fail_record();
        return FuriHalPn532ErrorComm;
    }
    /* ACK succeeded — clear any prior strikes */
    pn532_ack_strike_clear();

    /* PN532 command processing settle: after ACK, the PN532 needs time to
     * process the command and prepare the response. Without this, the I2C
     * read can get a stale READY byte or NACK. 10ms for shared I2C1 bus
     * (PN532+PCF8574+OLED), voltage drop during read/write/emulate. */
    furi_delay_ms(10);

    size_t payload_len = 0;
    if(!pn532_read_response(rx_data, rx_size, &payload_len, timeout_ms)) {
        if(furi_thread_flags_get() & FuriHalNfcEventInternalTypeAbort) {
            /* Clean abort — the PN532 hardware is still alive.
              * Do NOT set pn532_ready = false here: that would make
              * furi_hal_nfc_pn532_is_active() return false and corrupt the
              * NFC worker state machine while it is still running.
              * Return Comm (not Timeout) so the caller treats this as an
              * interrupted exchange rather than a hardware error. */
            FURI_LOG_D(TAG, "pn532_exchange: aborted mid-exchange (clean stop)");
            /* Drain any pending response data so the PN532 output buffer
             * is ready for the next command.  Without this drain, the PN532
             * NACKs the next I2C write, forcing 3 retries and risking a
             * TX-failure cascade.  Best-effort: if the drain fails (e.g.
             * PN532 still busy), the next write_frame -> drain_output
             * will catch it later. */
            furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
            uint8_t drain[PN532_MAX_RX_FRAME];
            furi_hal_i2c_rx(&furi_hal_i2c_handle_power, pn532_i2c_addr, drain, sizeof(drain), 150);
            furi_hal_i2c_release(&furi_hal_i2c_handle_power);
            pn532_debug.exchange_errors++;
            return FuriHalPn532ErrorComm;
        }
        pn532_debug.exchange_errors++;
        FURI_LOG_D(TAG, "pn532_exchange: response timeout");
        /* Do NOT count response timeouts as exchange failures — during
         * MF Classic dict attack, wrong-key auth causes the card to HALT,
         * producing expected timeouts.  Only count genuine transport errors
         * (write fail, ACK fail, bad response frame). */
        /* Best-effort drain: clone PN532 may eventually go READY with a stale
         * response after its own internal timeout. If we don't drain it, the
         * next exchange reads stale data → corrupt state → cascade failures. */
        if(pn532_wait_ready_ms(50)) {
            uint8_t drain_buf[PN532_MAX_RX_FRAME];
            furi_hal_i2c_acquire(&furi_hal_i2c_handle_power);
            furi_hal_i2c_rx(
                &furi_hal_i2c_handle_power, pn532_i2c_addr, drain_buf, sizeof(drain_buf), 150);
            furi_hal_i2c_release(&furi_hal_i2c_handle_power);
            FURI_LOG_D(TAG, "pn532_exchange: drained stale frame after timeout");
        }
        return FuriHalPn532ErrorTimeout;
    }

    if((payload_len == 0) || (rx_data[0] != expected_response)) {
        pn532_debug.exchange_errors++;
        FURI_LOG_W(
            TAG,
            "pn532_exchange: bad response[0]=0x%02X expected=0x%02X",
            rx_data[0],
            expected_response);
        pn532_exchange_fail_record();
        return FuriHalPn532ErrorInvalidFrame;
    }

    pn532_exchange_fail_count = 0;
    pn532_exchange_fail_first_tick = 0;
    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}

bool furi_hal_pn532_poll_iso14443a_timeout(FuriHalPn532Target* target, uint32_t timeout_ms) {
    FURI_LOG_D(TAG, "poll_iso14443a: ENTER timeout=%lu", timeout_ms);
    if(target) memset(target, 0, sizeof(*target));

    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x00};
    uint8_t response[64] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_LIST_PASSIVE + 1,
        response,
        sizeof(response),
        &response_len,
        timeout_ms);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2) {
        FURI_LOG_W(TAG, "poll_iso14443a: response too short=%zu", response_len);
        return false;
    }
    FURI_LOG_D(TAG, "InListPassiveTarget nb_targets=%u", response[1]);
    if(response[1] == 0) {
        FURI_LOG_D(TAG, "poll_iso14443a: no targets found");
        return false;
    }
    if(response[1] > 1) {
        FURI_LOG_D(TAG, "poll_iso14443a: %u tags present, processing first only", response[1]);
    }
    if(response_len < 7) return false;
    if(!target) return true;

    FURI_LOG_D(
        TAG,
        "poll_iso14443a: Tg=%d ATQA=%02X%02X SAK=%02X UIDlen=%d",
        response[2],
        response[3],
        response[4],
        response[5],
        response[6]);
    target->target_number = response[2];
    /* NOTE: byte order swap.
     * PN532 delivers ATQA big-endian (high byte first = response[3],
     * low byte = response[4]).  The MIFARE Classic detect and type
     * handlers expect little-endian (atqa[0]=low, atqa[1]=high).
     * Swap here so all downstream consumers see the correct byte order. */
    target->atqa[0] = response[4];
    target->atqa[1] = response[3];
    target->sak = response[5];
    target->uid_len = response[6];
    if((7U + target->uid_len) > response_len) return false;
    if(target->uid_len > sizeof(target->uid)) return false;
    memcpy(target->uid, &response[7], target->uid_len);

    // Extract ATS if present (ISO14443-4A capable card)
    target->iso_dep_active = (response[5] & 0x20) != 0;
    if(target->iso_dep_active) {
        const size_t ats_offset = 7 + target->uid_len;
        if(ats_offset < response_len) {
            target->ats_len = response_len - ats_offset;
            if(target->ats_len > sizeof(target->ats)) {
                target->ats_len = sizeof(target->ats);
            }
            memcpy(target->ats, &response[ats_offset], target->ats_len);
            FURI_LOG_D(TAG, "poll_iso14443a: ISO-DEP active, ATS %zu bytes", target->ats_len);
        }
    }

    return true;
}

bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target) {
    return furi_hal_pn532_poll_iso14443a_timeout(target, PN532_TIMEOUT_POLL_MS);
}

bool furi_hal_pn532_srix_detect(uint8_t* chip_id) {
    uint8_t cmd[] = {0x06, 0x00};
    uint8_t response[16] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532_get_target_number(),
        cmd,
        sizeof(cmd),
        response,
        sizeof(response),
        &response_len);
    if(error != FuriHalPn532ErrorNone || response_len < 2) return false;

    if(response[0] != 0x00) return false;

    if(chip_id && response_len >= 2) {
        *chip_id = response[1];
    }

    return true;
}

bool furi_hal_pn532_srix_select(uint8_t chip_id) {
    uint8_t cmd[] = {0x0E, chip_id};
    uint8_t response[8] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532_get_target_number(),
        cmd,
        sizeof(cmd),
        response,
        sizeof(response),
        &response_len);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 1) return false;

    return (response[0] == PN532_STATUS_SUCCESS);
}

bool furi_hal_pn532_srix_get_uid(uint8_t* uid, size_t* uid_len) {
    if(!uid || !uid_len) return false;

    uint8_t cmd[] = {0x0B};
    uint8_t response[16] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532_get_target_number(),
        cmd,
        sizeof(cmd),
        response,
        sizeof(response),
        &response_len);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2 || response[0] != 0x00) return false;

    size_t copy_len = response_len - 1;
    if(copy_len > 8) copy_len = 8;
    memcpy(uid, &response[1], copy_len);
    *uid_len = copy_len;

    return true;
}

bool furi_hal_pn532_srix_read_block(uint8_t block_num, uint8_t* data) {
    if(!data) return false;

    uint8_t cmd[] = {0x08, block_num};
    uint8_t response[16] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532_get_target_number(),
        cmd,
        sizeof(cmd),
        response,
        sizeof(response),
        &response_len);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 5 || response[0] != 0x00) return false;

    memcpy(data, &response[1], 4);
    return true;
}

bool furi_hal_pn532_srix_write_block(uint8_t block_num, const uint8_t* data) {
    if(!data) return false;

    uint8_t cmd[] = {0x09, block_num, data[0], data[1], data[2], data[3]};
    uint8_t response[8] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_in_data_exchange(
        furi_hal_nfc_pn532_get_target_number(),
        cmd,
        sizeof(cmd),
        response,
        sizeof(response),
        &response_len);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 1) return false;

    return (response[0] == PN532_STATUS_SUCCESS);
}

bool furi_hal_pn532_poll_felica(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));

    /* InListPassiveTarget: BrTy=0x01 selects FeliCa 212 kbps per NXP UM0701-02
     * §7.3.13. The previous BrTy=0x00 (Type A 106 kbps) only worked by accident
     * via the Type A poll pipeline; spec-correct value matches Adafruit and nfcpy. */
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x02, 0x01};
    uint8_t response[64] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_LIST_PASSIVE + 1,
        response,
        sizeof(response),
        &response_len,
        PN532_TIMEOUT_POLL_MS);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2) return false;
    if(response[1] == 0) return false;
    if(response[1] > 1) {
        FURI_LOG_W(TAG, "poll_felica: %u tags present, processing first only", response[1]);
    }
    if(response_len < 19) return false;
    if(!target) return true;

    // FeliCa target format: [cmd+1][NbTg][Tg][IDm(8)][PMm(8)][SysCode(2)]...
    // At response: [0]=0x4B [1]=NbTg [2]=Tg [3..10]=IDm [11..18]=PMm
    target->target_number = response[2];
    target->uid_len = 8;
    memcpy(target->uid, &response[3], 8);
    memcpy(target->pmm, &response[11], 8);
    target->sak = 0; // Not applicable for FeliCa

    return true;
}

bool furi_hal_pn532_poll_iso14443b(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));

    /* InListPassiveTarget: cmd[1] = BrTy = 0x03 (ISO14443-B at 106 kbps)
     * cmd[2] = AFI (0x00 = any application)
     * This was previously 0x01 which is BrTy for ISO14443-A, causing all
     * Type B polls to silently act as duplicate Type A polls. */
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x03, 0x00};
    uint8_t response[64] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_LIST_PASSIVE + 1,
        response,
        sizeof(response),
        &response_len,
        PN532_TIMEOUT_POLL_MS);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2) return false;
    if(response[1] == 0) return false;
    if(response[1] > 1) {
        FURI_LOG_W(TAG, "poll_iso14443b: %u tags present, processing first only", response[1]);
    }
    if(response_len < 15) return false;
    if(!target) return true;
    if(response[3] != 0x50) return false;

    // Type B response: [cmd+1][NbTg][Tg][ATQB(12 bytes minimum)]
    // ATQB: [0x50][PUPI(4)][AppData(4)][ProtoInfo(3)]
    // So at response: [3]=0x50, [4..7]=PUPI, [8..11]=AppData, [12..14]=ProtoInfo
    target->target_number = response[2];
    // Store PUPI (4 bytes) as UID
    target->uid_len = 4;
    memcpy(target->uid, &response[4], 4);
    memcpy(target->app_data, &response[8], 4);
    memcpy(target->proto_info, &response[12], 3);
    target->sak = 0; // not applicable for Type B
    target->atqa[0] = 0x50; // marker for Type B
    target->atqa[1] = 0x00;

    return true;
}

bool furi_hal_pn532_poll_jewel(FuriHalPn532Target* target) {
    if(target) memset(target, 0, sizeof(*target));

    /* InListPassiveTarget: BrTy=0x04 (Jewel/Topaz at 106 kbps).
     * The PN532 sends a 7-bit SENS_REQ (0x26) internally and returns the
     * RID response: [ATQA(2)][RID(6)] = 8 bytes total in the target data.
     * RID format: [HR0][HR1][UID0][UID1][UID2][UID3] — 6 bytes.
     * ATQA for Jewel is always 0x000C (little-endian: 0x0C, 0x00). */
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x04};
    uint8_t response[32] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_LIST_PASSIVE + 1,
        response,
        sizeof(response),
        &response_len,
        PN532_TIMEOUT_POLL_MS);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 2) return false;
    if(response[1] == 0) {
        FURI_LOG_D(TAG, "poll_jewel: no targets found");
        return false;
    }
    /* Minimum response: [cmd+1][NbTg][Tg][ATQA(2)][RID(6)] = 11 bytes */
    if(response_len < 11) {
        FURI_LOG_W(TAG, "poll_jewel: response too short (%zu)", response_len);
        return false;
    }
    if(!target) return true;

    /* response layout (0-indexed):
     * [0] = 0x4B (cmd+1)
     * [1] = NbTg (number of targets, should be 1)
     * [2] = Tg   (target number, 1-based)
     * [3] = ATQA low byte  (0x0C for Jewel)
     * [4] = ATQA high byte (0x00 for Jewel)
     * [5..10] = RID: HR0, HR1, UID0, UID1, UID2, UID3 */
    target->target_number = response[2];
    target->atqa[0] = response[3]; /* ATQA low  = 0x0C */
    target->atqa[1] = response[4]; /* ATQA high = 0x00 */
    /* Store the 6-byte RID (HR0, HR1, UID0..UID3) as the UID */
    target->uid_len = 6;
    memcpy(target->uid, &response[5], 6);
    target->sak = 0; /* Not applicable for Jewel */

    FURI_LOG_D(
        TAG,
        "poll_jewel: Tg=%d ATQA=%02X%02X HR0=%02X HR1=%02X UID=%02X%02X%02X%02X",
        target->target_number,
        target->atqa[1],
        target->atqa[0],
        target->uid[0],
        target->uid[1],
        target->uid[2],
        target->uid[3],
        target->uid[4],
        target->uid[5]);

    return true;
}

bool furi_hal_pn532_poll_iso15693(FuriHalPn532Target* target) {
    /* ISO15693 is NOT a native PN532 reader capability per NXP UM0701-02
     * (which lists Type A/B, FeliCa, Jewel, NFC-DEP only). nfcpy's PN532 driver
     * restricts InListPassiveTarget BrTy to 0-4 for the same reason. Modeled
     * as a backend capability bit; only enable for PN533 / PN532Killer-style
     * superset chips via board.mk. */
    if(!furi_hal_pn532_has_cap(FURI_HAL_PN532_CAP_ISO15693)) return false;

    if(target) memset(target, 0, sizeof(*target));

    /* InListPassiveTarget: MaxTg=1, BrTy=0x05 (ISO15693 at 26 kbps),
     * InitiatorData = [0x26, 0x01] = INVENTORY request flags + mask length.
     * Flags 0x26 = INVENTORY flag + nb_slots=1; mask_len = 0x01 (no mask data). */
    uint8_t cmd[] = {PN532_CMD_IN_LIST_PASSIVE, 0x01, 0x05, 0x26, 0x01};
    uint8_t response[32] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_LIST_PASSIVE + 1,
        response,
        sizeof(response),
        &response_len,
        PN532_TIMEOUT_POLL_MS);

    if(error != FuriHalPn532ErrorNone) return false;

    /* Minimum valid response: [cmd+1][NbTg][Tg][DSFID][UID(8)] = 12 bytes */
    if(response_len < 12) return false;

    /* response[0] = 0x4B (cmd echo), response[1] = NbTg */
    if(response[1] == 0) return false; /* no card found */

    if(!target) return true; /* caller only wanted presence check */

    /* response[2]    = Tg (target number, 1-based)
     * response[3]    = DSFID
     * response[4..11] = UID, LSB-first (PN532 wire format per ISO15693) */
    target->target_number = response[2];
    target->uid_len = 8;

    /* Reverse UID from LSB-first (wire) to MSB-first (Flipper convention) */
    for(int i = 0; i < 8; i++) {
        target->uid[i] = response[4 + (7 - i)];
    }

    /* Store DSFID in atqa[0] for upper layers that need it */
    target->atqa[0] = response[3]; /* DSFID */
    target->atqa[1] = 0x00;
    target->sak = 0; /* not applicable for ISO15693 */

    FURI_LOG_D(
        TAG,
        "poll_iso15693: Tg=%d DSFID=%02X UID=%02X%02X%02X%02X%02X%02X%02X%02X",
        target->target_number,
        target->atqa[0],
        target->uid[0],
        target->uid[1],
        target->uid[2],
        target->uid[3],
        target->uid[4],
        target->uid[5],
        target->uid[6],
        target->uid[7]);

    return true;
}

FuriHalPn532Error furi_hal_pn532_in_data_exchange_ex(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint8_t* pn532_status) {
    if(tx_len > PN532_MAX_TX_PAYLOAD - 3) return FuriHalPn532ErrorComm;

    /* Static buffers: eliminates 527 bytes from the NFC worker task stack per call.
     * Safe: all callers run in the NFC worker task under the I2C mutex (serialized);
     * this function is never called from the ready poller thread or interrupt context;
     * both buffers are fully overwritten before use. */
    static uint8_t cmd[PN532_MAX_TX_PAYLOAD + 2];
    static uint8_t response[PN532_MAX_RX_FRAME];
    memset(cmd, 0, sizeof(cmd));
    memset(response, 0, sizeof(response));
    cmd[0] = PN532_CMD_IN_DATA_EXCHANGE;
    cmd[1] = target_number;
    if(tx_len) memcpy(&cmd[2], tx_data, tx_len);

    /* Use longer timeout for large command payloads (>250 bytes).
     * MIFARE Classic 4K reads are typically short; this heuristic catches
     * long ISO14443-4 chained APDUs or large FeliCa transactions. */
    const uint32_t exchange_timeout = (tx_len > 250) ? PN532_TIMEOUT_EXCHANGE_4K_MS :
                                                       PN532_TIMEOUT_EXCHANGE_MS;

    size_t response_len = 0;
    FuriHalPn532Error error = pn532_exchange(
        cmd,
        tx_len + 2,
        PN532_CMD_IN_DATA_EXCHANGE + 1,
        response,
        sizeof(response),
        &response_len,
        exchange_timeout);

    if(error != FuriHalPn532ErrorNone) return error;
    if(response_len < 2) return FuriHalPn532ErrorInvalidFrame;

    /* response[0] = 0x41 (cmd echo), response[1] = PN532 status byte.
     * Expose the raw status byte to the caller before stripping; bit 6
     * (0x40) signals card-side ISO14443-4 chaining is active so the
     * caller can drive the R(ACK) loop in furi_hal_nfc_pn532_exchange_internal(). */
    const uint8_t status = response[1];
    if(pn532_status) *pn532_status = status;

    /* Lower 6 bits of the status byte = PN532 error code (0x00 = success). */
    if((status & 0x3FU) != PN532_STATUS_SUCCESS) {
        FURI_LOG_PN532_STATUS(FuriLogLevelWarn, TAG, "InDataExchange", status);
        /* Map distinct PN532 protocol status codes to richer transport errors. */
        const uint8_t err_code = (uint8_t)(status & 0x3FU);
        if(err_code == PN532_STATUS_TIMEOUT) return FuriHalPn532ErrorTimeout;
        if(err_code == PN532_STATUS_MIFARE_AUTH) return FuriHalPn532ErrorAuth;
        if(err_code == PN532_STATUS_RELEASED) return FuriHalPn532ErrorReleased;
        if(err_code == PN532_STATUS_CONTEXT) return FuriHalPn532ErrorContext;
        return FuriHalPn532ErrorComm;
    }

    const size_t payload_len = response_len - 2;
    if(payload_len > rx_size) {
        FURI_LOG_E(TAG, "in_data_exchange_ex overflow: %zu > %zu", payload_len, rx_size);
        return FuriHalPn532ErrorBufferOverflow;
    }
    if(payload_len) memcpy(rx_data, &response[2], payload_len);
    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}

FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len) {
    /* Thin wrapper around _ex that ignores the raw PN532 status byte.
     * Callers that need to detect ISO14443-4 card-side chaining (bit 6 of the
     * status byte) must use furi_hal_pn532_in_data_exchange_ex() directly and
     * drive the R(ACK) loop themselves. */
    return furi_hal_pn532_in_data_exchange_ex(
        target_number, tx_data, tx_len, rx_data, rx_size, rx_len, NULL);
}

static FuriHalPn532Error furi_hal_pn532_in_communicate_thru_impl(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms) {
    if(tx_len > PN532_MAX_TX_PAYLOAD - 2) return FuriHalPn532ErrorComm;

    /* Static buffers: eliminates 527 bytes from the NFC worker task stack per call.
     * Safe: all callers are serialized under the I2C mutex in the NFC worker task;
     * not called from the ready poller thread or interrupt context. */
    static uint8_t cmd[PN532_MAX_TX_PAYLOAD + 2];
    static uint8_t response[PN532_MAX_RX_FRAME];
    memset(cmd, 0, sizeof(cmd));
    memset(response, 0, sizeof(response));
    cmd[0] = PN532_CMD_IN_COMMUNICATE_THRU;
    if(tx_len) memcpy(&cmd[1], tx_data, tx_len);

    size_t response_len = 0;
    FuriHalPn532Error error = pn532_exchange(
        cmd, tx_len + 1, 0x43, response, sizeof(response), &response_len, timeout_ms);
    if(error != FuriHalPn532ErrorNone) return error;
    if(response_len < 2) return FuriHalPn532ErrorInvalidFrame;
    if(response[1] != PN532_STATUS_SUCCESS) {
        FURI_LOG_PN532_STATUS(FuriLogLevelWarn, TAG, "InCommunicateThru", response[1]);
        if(response[1] == PN532_STATUS_TIMEOUT) return FuriHalPn532ErrorTimeout;
        /* Map PN532 protocol status codes to richer transport errors so the
         * upper FSM can distinguish wrong-key, lost-target, and protocol-state
         * faults. Per NXP UM0701-02: 0x14=MifareAuth, 0x29=Released, 0x27=Context. */
        const uint8_t status = (uint8_t)(response[1] & 0x3FU);
        if(status == PN532_STATUS_MIFARE_AUTH) return FuriHalPn532ErrorAuth;
        if(status == PN532_STATUS_RELEASED) return FuriHalPn532ErrorReleased;
        if(status == PN532_STATUS_CONTEXT) return FuriHalPn532ErrorContext;
        return FuriHalPn532ErrorComm;
    }

    const size_t payload_len = response_len - 2;
    if(payload_len > rx_size) return FuriHalPn532ErrorBufferOverflow;
    if(payload_len) memcpy(rx_data, &response[2], payload_len);
    if(rx_len) *rx_len = payload_len;
    return FuriHalPn532ErrorNone;
}

FuriHalPn532Error furi_hal_pn532_in_communicate_thru(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len) {
    return furi_hal_pn532_in_communicate_thru_impl(
        tx_data, tx_len, rx_data, rx_size, rx_len, PN532_TIMEOUT_EXCHANGE_MS);
}

FuriHalPn532Error furi_hal_pn532_in_communicate_thru_timeout(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms) {
    return furi_hal_pn532_in_communicate_thru_impl(
        tx_data, tx_len, rx_data, rx_size, rx_len, timeout_ms);
}

FuriHalPn532Error furi_hal_pn532_mf_auth(
    uint8_t target_number,
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len) {
    furi_check(key);
    furi_check(uid);

    uint8_t cmd[14];
    cmd[0] = PN532_CMD_IN_DATA_EXCHANGE;
    cmd[1] = target_number;
    cmd[2] = key_type ? 0x61 : 0x60;
    cmd[3] = block_num;
    memcpy(&cmd[4], key, 6);
    size_t copy_len = (uid_len >= 4) ? 4 : uid_len;
    memcpy(&cmd[10], uid, copy_len);
    if(copy_len < 4) memset(&cmd[10 + copy_len], 0, 4 - copy_len);

    uint8_t resp[4];
    size_t resp_len = sizeof(resp);
    FuriHalPn532Error err = pn532_exchange(
        cmd,
        sizeof(cmd),
        PN532_CMD_IN_DATA_EXCHANGE + 1,
        resp,
        sizeof(resp),
        &resp_len,
        /* MIFARE Classic InDataExchange auth: PN532 runs a full 3-pass
         * handshake internally (anticoll-resume + AUTH + verify); on clone
         * modules at 3.3 V this can exceed 300 ms.  Use the 1 s
         * InDataExchange timeout — the same value every other
         * InDataExchange site uses — to avoid host-side timeouts that
         * leave a stale response in the PN532 output buffer. */
        PN532_TIMEOUT_EXCHANGE_MS);

    if(err == FuriHalPn532ErrorNone && resp_len >= 2) {
        uint8_t status = resp[1];
        if(status == PN532_STATUS_SUCCESS) {
            return FuriHalPn532ErrorNone;
        }
        if(status == PN532_STATUS_MIFARE_AUTH) { // 0x14: auth error (wrong key)
            return FuriHalPn532ErrorAuth;
        }
        if(status == PN532_STATUS_RELEASED) { // 0x29: target released
            return FuriHalPn532ErrorReleased;
        }
        if(status == 0x13) { // DEP bad data: clone PN532 doesn't support native auth
            FURI_LOG_W(TAG, "mf_auth: native auth not supported (0x13)");
            return FuriHalPn532ErrorUnsupported;
        }
        if(status == 0x01) { // Timeout from clone module
            return FuriHalPn532ErrorTimeout;
        }
    }

    return FuriHalPn532ErrorComm;
}

bool furi_hal_pn532_mf_backdoor_auth(
    uint8_t block_num,
    uint8_t key_type,
    const uint8_t* key,
    uint8_t backdoor_type) {
    furi_check(key);

    uint8_t auth_cmd = (backdoor_type == 0) ? 0x64 : 0x65;

    uint8_t cmd[10] = {PN532_CMD_IN_COMMUNICATE_THRU, auth_cmd, block_num, key_type};
    memcpy(&cmd[4], key, 6);

    uint8_t response[8] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_send_command(cmd, sizeof(cmd));
    if(error != FuriHalPn532ErrorNone) return false;

    error = furi_hal_pn532_read_response(response, sizeof(response), &response_len, 150);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 1) return false;

    return (response[0] == PN532_STATUS_SUCCESS);
}

bool furi_hal_pn532_mf_backdoor_write_block0(uint8_t block_num, const uint8_t* block_data) {
    furi_check(block_data);

    uint8_t cmd_wr[19] = {PN532_CMD_IN_COMMUNICATE_THRU, 0xA0, block_num};
    memcpy(&cmd_wr[3], block_data, 16);

    uint8_t response[8] = {0};
    size_t response_len = 0;

    FuriHalPn532Error error = furi_hal_pn532_send_command(cmd_wr, sizeof(cmd_wr));
    if(error != FuriHalPn532ErrorNone) return false;

    error = furi_hal_pn532_read_response(response, sizeof(response), &response_len, 200);
    if(error != FuriHalPn532ErrorNone) return false;
    if(response_len < 1) return false;

    return (response[0] == PN532_STATUS_SUCCESS);
}

FuriHalPn532Error furi_hal_pn532_send_command(const uint8_t* cmd, size_t cmd_len) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(cmd_len == 0) return FuriHalPn532ErrorNone;

    if(!pn532_write_frame(cmd, cmd_len)) return FuriHalPn532ErrorComm;
    if(!pn532_read_ack()) {
        /* Two-strikes-out: same logic as pn532_exchange() — a single ACK
         * miss is almost always a transient I2C glitch.  Marking
         * pn532_ready=false here (as the old code did) defeats the
         * strike fix and triggers a full PN532 reinit on the next call. */
        pn532_handle_ack_failure("send_command");
        return FuriHalPn532ErrorInvalidAck;
    }
    pn532_ack_strike_clear();
    return FuriHalPn532ErrorNone;
}

/** Send InRelease (0x52) to release all in-listed targets.
 * Uses pn532_exchange() so a single transient ACK failure does NOT set
 * pn532_ready=false.  InRelease is best-effort cleanup; if it fails the
 * PN532 is still usable and the next InListPassiveTarget re-establishes
 * a clean state.
 *
 * Timeout is 150ms (I2C bus floor, matches PCF8574 / pn532_read_ack).
 * This is the transaction wait, not a settle delay — for clone-module
 * settle after release, callers should add furi_delay_ms(500). */
void furi_hal_pn532_in_release(void) {
    if(!pn532_ready) return;
    const uint8_t cmd[] = {PN532_CMD_IN_RELEASE, 0x00}; /* Release all targets */
    uint8_t resp[4] = {0};
    size_t resp_len = 0;
    /* Ignore errors — InRelease is best-effort */
    pn532_exchange(cmd, sizeof(cmd), 0x53, resp, sizeof(resp), &resp_len, 150);
}

FuriHalPn532Error furi_hal_pn532_read_response(
    uint8_t* data,
    size_t data_size,
    size_t* data_len,
    uint32_t timeout_ms) {
    if(!pn532_ready) return FuriHalPn532ErrorComm;

    FuriHalPn532Error error = pn532_read_response_ex(
        data, data_size, data_len, timeout_ms > 0 ? timeout_ms : PN532_TIMEOUT_EXCHANGE_MS);
    return error;
}

// Target/Listener mode: initialize PN532 as an NFC target (tag)
// params: [MODE][SENS_RES(2)][NFCID1(3)][SEL_RES(1)][FeliCaParams(16)][NFCID3t(10)][GB_len][historical...]
// See Seeed PN532/emulatetag.cpp for reference.
FuriHalPn532Error furi_hal_pn532_tg_init_as_target(
    const uint8_t* params,
    size_t params_len,
    uint32_t timeout_ms) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(!params || params_len == 0) return FuriHalPn532ErrorComm;

    uint8_t cmd[PN532_MAX_TX_PAYLOAD] = {0};
    cmd[0] = PN532_CMD_TG_INIT_AS_TARGET;
    memcpy(
        &cmd[1],
        params,
        params_len > (PN532_MAX_TX_PAYLOAD - 1) ? (PN532_MAX_TX_PAYLOAD - 1) : params_len);
    const size_t cmd_len = params_len + 1;

    uint8_t resp[4] = {0};
    size_t resp_len = sizeof(resp);
    FuriHalPn532Error err = pn532_exchange(
        cmd, cmd_len, PN532_CMD_TG_INIT_AS_TARGET + 1, resp, sizeof(resp), &resp_len, timeout_ms);

    if(err != FuriHalPn532ErrorNone) return err;
    // Response: [cmd+1][status]
    if(resp_len < 2) return FuriHalPn532ErrorComm;
    // status 0x00 = success, 0x01 = timeout (no initiator)
    return (resp[1] == PN532_STATUS_SUCCESS) ? FuriHalPn532ErrorNone : FuriHalPn532ErrorTimeout;
}

// Target/Listener mode: receive data from initiator (reader)
// Returns received data in buf, length in out_len.
FuriHalPn532Error furi_hal_pn532_tg_get_data(
    uint8_t* buf,
    size_t buf_size,
    size_t* out_len,
    uint32_t timeout_ms) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(!buf || !out_len) return FuriHalPn532ErrorComm;

    uint8_t cmd[] = {PN532_CMD_TG_GET_DATA};
    // Use send_command + read_response for flexible-length response
    if(!pn532_write_frame(cmd, sizeof(cmd))) return FuriHalPn532ErrorComm;
    if(!pn532_read_ack()) {
        /* Two-strikes-out: tolerate transient ACK glitches without
         * forcing a full PN532 reinit on the next call. */
        pn532_handle_ack_failure("tg_get_data");
        return FuriHalPn532ErrorInvalidAck;
    }
    pn532_ack_strike_clear();

    // Response: [cmd+1][status][data...]
    uint8_t resp[PN532_MAX_RX_FRAME] = {0};
    size_t resp_len = 0;
    if(!pn532_read_response(resp, sizeof(resp), &resp_len, timeout_ms)) {
        return FuriHalPn532ErrorComm;
    }
    if(resp_len < 2) return FuriHalPn532ErrorComm;
    if(resp[0] != (PN532_CMD_TG_GET_DATA + 1)) return FuriHalPn532ErrorInvalidFrame;
    if(resp[1] != PN532_STATUS_SUCCESS)
        return (resp[1] == PN532_STATUS_TIMEOUT) ? FuriHalPn532ErrorTimeout :
                                                   FuriHalPn532ErrorComm;

    const size_t data_len = resp_len - 2;
    if(data_len > buf_size) return FuriHalPn532ErrorBufferOverflow;
    if(data_len > 0) memcpy(buf, &resp[2], data_len);
    *out_len = data_len;
    return FuriHalPn532ErrorNone;
}

// Target/Listener mode: send data to initiator (reader)
FuriHalPn532Error furi_hal_pn532_tg_set_data(const uint8_t* data, size_t data_len) {
    if(!pn532_ready && !furi_hal_pn532_init()) return FuriHalPn532ErrorComm;
    if(!data || data_len == 0) return FuriHalPn532ErrorComm;
    if(data_len > (PN532_MAX_TX_PAYLOAD - 1)) return FuriHalPn532ErrorComm;

    uint8_t cmd[PN532_MAX_TX_PAYLOAD] = {0};
    cmd[0] = PN532_CMD_TG_SET_DATA;
    memcpy(&cmd[1], data, data_len);

    uint8_t resp[4] = {0};
    size_t resp_len = sizeof(resp);
    FuriHalPn532Error err = pn532_exchange(
        cmd, data_len + 1, PN532_CMD_TG_SET_DATA + 1, resp, sizeof(resp), &resp_len, 1000);

    if(err != FuriHalPn532ErrorNone) return err;
    if(resp_len < 2) return FuriHalPn532ErrorComm;
    return (resp[1] == PN532_STATUS_SUCCESS) ? FuriHalPn532ErrorNone : FuriHalPn532ErrorComm;
}
