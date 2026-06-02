#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_I2C_ADDR_7BIT 0x48 // 8-bit addr 0x90 (7-bit 0x48 shifted for STM32 I2C HAL)

/** @name PN532 Protocol Status Codes
 *
 * PN532 protocol-level status/error codes returned in the status byte of
 * InDataExchange, InCommunicateThru, and other command responses.
 *
 * Per the PN532 User Manual §7.3.6, the lower 6 bits of the response
 * status byte carry the error code; bit 6 (0x40) signals "More Information"
 * (ISO14443-4 card-side chaining).  Use the @ref FURI_LOG_PN532_STATUS macro
 * to log these values with automatic string decoding.
 *
 * @{ */
#define PN532_STATUS_SUCCESS                0x00 /**< Success (no error). */
#define PN532_STATUS_TIMEOUT                0x01 /**< Timeout: target did not respond in time. */
#define PN532_STATUS_CRC                    0x02 /**< CRC error detected in received data. */
#define PN532_STATUS_PARITY                 0x03 /**< Parity error detected in received data. */
#define PN532_STATUS_COLLISION_BITCOUNT     0x04 /**< Bit-count error during collision resolution. */
#define PN532_STATUS_MIFARE_FRAMING         0x05 /**< MIFARE framing error. */
#define PN532_STATUS_COLLISION_BITCOLLISION 0x06 /**< Bit-level collision detected. */
#define PN532_STATUS_NOBUFS                 0x07 /**< Internal buffer overflow (PN532 side). */
#define PN532_STATUS_RFNOBUFS               0x09 /**< RF buffer overflow (deprecated). */
#define PN532_STATUS_ACTIVE_TOOSLOW         0x0A /**< Active mode: initiator poll interval too short. */
#define PN532_STATUS_RFPROTO                0x0B /**< RF protocol error during active communication. */
#define PN532_STATUS_TOOHOT                 0x0D /**< PN532 die temperature too high, operation aborted. */
#define PN532_STATUS_INTERNAL_NOBUFS        0x0E /**< Internal RF buffer overflow. */
#define PN532_STATUS_INVAL                  0x10 /**< Invalid command or parameter. */
#define PN532_STATUS_DEP_INVALID_COMMAND    0x12 /**< ISO-DEP: invalid command received from peer. */
#define PN532_STATUS_DEP_BADDATA            0x13 /**< ISO-DEP: bad data format from peer. */
#define PN532_STATUS_MIFARE_AUTH \
    0x14 /**< MIFARE authentication failed (key or access conditions). */
#define PN532_STATUS_NOSECURE    0x18 /**< Secure channel not established (not authenticated). */
#define PN532_STATUS_I2CBUSY     0x19 /**< I2C bus busy: another master is transmitting. */
#define PN532_STATUS_UIDCHECKSUM 0x23 /**< UID checksum error (ISO14443-3B cascade). */
#define PN532_STATUS_DEPSTATE    0x25 /**< ISO-DEP state error: protocol state machine mismatch. */
#define PN532_STATUS_HCIINVAL    0x26 /**< HCI: invalid instruction from host. */
#define PN532_STATUS_CONTEXT     0x27 /**< Context error: operation incompatible with current mode. */
#define PN532_STATUS_RELEASED    0x29 /**< Target was released or has left the RF field. */
#define PN532_STATUS_CARDSWAPPED 0x2A /**< A different card was detected during the exchange. */
#define PN532_STATUS_NOCARD      0x2B /**< No card present in RF field (anti-collision failed). */
#define PN532_STATUS_MISMATCH    0x2C /**< Target mismatch: UID or SAK changed mid-session. */
#define PN532_STATUS_OVERCURRENT 0x2D /**< Overcurrent detected on RF driver. */
#define PN532_STATUS_NONAD       0x2E /**< Non-AD (Authenticated Device) mode violation. */
/** @} */

/** Log a PN532 protocol status byte with automatic string decoding.
 *
 * The status byte's lower 6 bits contain the PN532 error code
 * (per PN532 User Manual Section 7.3.6).  This macro masks those bits,
 * looks up the description via furi_hal_pn532_strerror(), and
 * formats the status byte as "prefix: status=0x%02X (description)".
 *
 * Bit 6 (0x40 = "More Information" / ISO-DEP chaining) is preserved
 * in the hex output for diagnostic clarity.
 *
 * Example output on a MIFARE auth failure:
 *   [FuriHalPN532][W] InDataExchange: status=0x14 (MIFARE authentication failed)
 *
 * @param[in] level     Log level (e.g., FuriLogLevelWarn, FuriLogLevelError).
 * @param[in] tag       Log tag string (e.g., TAG).
 * @param[in] msg       Context message describing the operation.
 * @param[in] status    PN532 protocol status byte (raw, from response[1]).
 */
#define FURI_LOG_PN532_STATUS(level, tag, msg, status) \
    furi_log_print_format(                             \
        level,                                         \
        tag,                                           \
        "%s: status=0x%02X (%s)",                      \
        (const char*)(msg),                            \
        (uint8_t)(status),                             \
        furi_hal_pn532_strerror((uint8_t)(status) & 0x3FU))

/** PN532 I2C transport-layer error codes.
 *
 * These errors originate from the I2C read/write exchange with the PN532
 * chip, not from the PN532 protocol status byte inside response frames.
 * Use furi_hal_pn532_strerror() to decode the protocol-level status byte.
 *
 * @see furi_hal_pn532_error_str() for human-readable string conversion. */
typedef enum {
    FuriHalPn532ErrorNone = 0, /**< Operation completed successfully. */
    FuriHalPn532ErrorTimeout, /**< I2C transaction timed out. */
    FuriHalPn532ErrorComm, /**< I2C communication error (NACK, bus fault). */
    FuriHalPn532ErrorInvalidAck, /**< PN532 ACK frame was malformed or missing. */
    FuriHalPn532ErrorInvalidFrame, /**< PN532 response frame failed checksum validation. */
    FuriHalPn532ErrorBufferOverflow, /**< Response larger than the caller's rx buffer. */
    FuriHalPn532ErrorUnsupported, /**< Command not supported by this PN532 firmware version. */
    FuriHalPn532ErrorAuth, /**< MIFARE authentication failed (status 0x14). */
    FuriHalPn532ErrorReleased, /**< Target released by initiator (status 0x29). */
    FuriHalPn532ErrorContext, /**< Command not acceptable in current context (status 0x27). */
    /* Additions: full NXP UM0701-02 error code table (status byte mapping).
     * Purely additive — existing integer values preserved for ABI compat.
     * Used for diagnostic logging via furi_hal_pn532_error_str(). */
    FuriHalPn532ErrorParity, /**< Parity error detected in received data (status 0x03). */
    FuriHalPn532ErrorBitCount, /**< Erroneous bit count during anti-collision (status 0x04). */
    FuriHalPn532ErrorBufferSize, /**< Communication buffer insufficient (status 0x07). */
    FuriHalPn532ErrorRFBufferOverflow, /**< RF buffer overflow by CIU (status 0x09). */
    FuriHalPn532ErrorFieldNotSwitched, /**< RF field not switched on in time (status 0x0A). */
    FuriHalPn532ErrorRFProtocol, /**< RF protocol error (status 0x0B). */
    FuriHalPn532ErrorTemperature, /**< PN532 die temperature too high (status 0x0D). */
    FuriHalPn532ErrorInvalidParameter, /**< Invalid command or parameter (status 0x10). */
    FuriHalPn532ErrorDEPInvalidCommand, /**< ISO-DEP: invalid command from peer (status 0x12). */
    FuriHalPn532ErrorDEPBadData, /**< ISO-DEP/MIFARE: bad data format (status 0x13). */
    FuriHalPn532ErrorNoSecure, /**< Secure channel not established (status 0x18). */
    FuriHalPn532ErrorI2CBusy, /**< I2C bus busy: another master transmitting (status 0x19). */
    FuriHalPn532ErrorUIDChecksum, /**< UID checksum error (ISO14443-3B cascade, status 0x23). */
    FuriHalPn532ErrorDEPInvalidState, /**< ISO-DEP: protocol state machine mismatch (status 0x25). */
    FuriHalPn532ErrorHCIInvalid, /**< HCI: invalid instruction from host (status 0x26). */
    FuriHalPn532ErrorCardSwapped, /**< Different card detected during exchange (status 0x2A). */
    FuriHalPn532ErrorNoCard, /**< No card present in RF field (status 0x2B). */
    FuriHalPn532ErrorDEPMismatch, /**< Target mismatch: UID or SAK changed (status 0x2C). */
    FuriHalPn532ErrorOverCurrent, /**< Overcurrent detected on RF driver (status 0x2D). */
    FuriHalPn532ErrorNADMissing, /**< Non-AD (Authenticated Device) mode violation (status 0x2E). */
} FuriHalPn532Error;

/* Internal helpers for compile-time type-checked PN532 error logging.
 *
 * These use __attribute__((__error__)) to produce clear diagnostic messages
 * when FuriHalPn532Error values are logged without conversion.
 *
 * Note: _Generic requires C11 or later.  The project is built with -std=gnu11
 * (or equivalent) so this is safe.  If the build standard is ever changed to
 * C99 or earlier, these macros will fail to compile with a clear error. */

/* Fires when a raw FuriHalPn532Error is detected by FURI_HAL_PN532_ERROR_LOG_GUARD. */
__attribute__((__error__(
    "FuriHalPn532Error logged directly: convert via furi_hal_pn532_error_str(err) first"))) static inline void
    furi_hal_pn532_internal_log_forbidden(void) {
}

/** @name Compile-time Checks for FuriHalPn532Error Logging
 *
 * These macros use C11 _Generic to verify that FuriHalPn532Error values
 * are NEVER passed raw to FURI_LOG format strings.  The only safe way
 * to log a PN532 transport error is via furi_hal_pn532_error_str(), and
 * these macros enforce that at compile time.
 *
 * Usage:
 * @code
 *   // ✅ Correct — type-checked, auto-converts to const char* for %%s
 *   FURI_LOG_D(TAG, "error: %s", FURI_HAL_PN532_ERROR_LOG(err));
 *   @endcode
 *
 * @{ */

/** @brief Log-safe FuriHalPn532Error string converter (compile-time checked).
 *
 * Wraps furi_hal_pn532_error_str() with a _Generic type guard.
 * - If @p err is FuriHalPn532Error: calls furi_hal_pn532_error_str(err)
 *   and returns const char* for use with %%s.
 * - If @p err is any other type: _Generic has no matching branch,
 *   producing a compile error like "'int' doesn't match any type in _Generic".
 *
 * Use this macro instead of calling furi_hal_pn532_error_str() directly
 * in FURI_LOG_* calls.  The _Generic check makes it impossible to pass
 * the wrong type, and the const char* result forces %%s format (not %%d)
 * so the printf format attribute catches mismatches too.
 *
 * @param[in] err  A FuriHalPn532Error value to convert for logging.
 * @return const char* suitable for use with %%s in FURI_LOG_*.
 */
#define FURI_HAL_PN532_ERROR_LOG(err) \
    _Generic((err), FuriHalPn532Error: furi_hal_pn532_error_str(err))

/** @brief Statement guard: fires if @p x is still a raw FuriHalPn532Error.
 *
 * Place this as a standalone statement before any FURI_LOG_* call to
 * verify that a FuriHalPn532Error value has been converted to a string.
 * If @p x is FuriHalPn532Error, the guard fires a compile error,
 * reminding the developer to use FURI_HAL_PN532_ERROR_LOG() or
 * furi_hal_pn532_error_str() before logging.
 *
 * Typical pattern — guard the *converted* variable to prove conversion
 * happened:
 * @code
 *   static void report_err(FuriHalPn532Error err) {
 *       const char* err_str = FURI_HAL_PN532_ERROR_LOG(err); // type-checked
 *       FURI_HAL_PN532_ERROR_LOG_GUARD(err_str);  // const char* -> no-op
 *       FURI_LOG_E(TAG, "failed: %s", err_str);
 *   }
 * @endcode
 *
 * @param[in] x  Value to check (passes for non-FuriHalPn532Error types).
 */
#define FURI_HAL_PN532_ERROR_LOG_GUARD(x) \
    _Generic((x), FuriHalPn532Error: furi_hal_pn532_internal_log_forbidden(), default: (void)0)

/** @} */

/** Activated target descriptor returned by InListPassiveTarget commands.
 *
 * Stores protocol-level attributes of a single activated NFC target
 * (ISO14443-3A, ISO14443-3B, FeliCa, or Jewel/Topaz) for use by
 * InDataExchange, InCommunicateThru, and other data-exchange commands.
 * Fields are populated by the poll_* family of functions; unused fields
 * are set to zero. */
typedef struct {
    uint8_t target_number; /**< PN532 target index (0..1). */
    uint8_t atqa[2]; /**< ATQ(A/B) response bytes from anti-collision. */
    uint8_t sak; /**< SAK (Select ACK) byte [ISO14443-3A only]. */
    uint8_t uid[10]; /**< UID / NFC-ID, MSB-first. */
    size_t uid_len; /**< Number of valid bytes in uid[]. */
    uint8_t app_data[4]; /**< Type B Application Data (from ATQB). */
    uint8_t proto_info[3]; /**< Type B Protocol Info (from ATQB). */
    uint8_t pmm[8]; /**< FeliCa PMm (Manufacturer / Card ID). */
    uint8_t ats[20]; /**< ATS bytes (ISO14443-4A, present when SAK bit 0x20 set). */
    size_t ats_len; /**< Number of valid bytes in ats[]. */
    bool iso_dep_active; /**< SAK bit 0x20 indicates ISO-DEP capability. */
} FuriHalPn532Target;

/** Initialise the PN532 over I2C.
 *
 * Sends SAMConfiguration (0x14) to put the chip into Normal SAM mode
 * (mode 0x01: SAM is used for crypto, not virtual/wired card emulation),
 * starts the software-ready poller thread, and verifies the chip is responsive.
 * Safe to call multiple times (re-initialises if previously de-initialised).
 * @returns true on success, false on I2C or SAMConfig failure. */
bool furi_hal_pn532_init(void);

/** Check whether the PN532 software-ready poller has ever seen the chip.
 * @returns true if the chip is (or was recently) responsive. */
bool furi_hal_pn532_is_ready(void);

/** Force PN532 reinit on next exchange.  Call after Comm errors (parity,
 *  CRC, no-response) that may leave the PN532 internal state corrupted. */
void furi_hal_pn532_force_reinit(void);

/** Set a global exchange deadline (tick).  When set, pn532_wait_ready_ms()
 *  clamps individual waits to the remaining time and pn532_exchange()
 *  returns FuriHalPn532ErrorTimeout once the deadline has passed.
 *  Use to cap the total time inside a plugin loop or time-budgeted section.
 *  @param deadline_tick  Absolute tick (furi_get_tick()) at which to bail.
 *  @see furi_hal_pn532_clear_exchange_deadline(). */
void furi_hal_pn532_set_exchange_deadline(uint32_t deadline_tick);

/** Clear the global exchange deadline.  Must be called on every exit path
 *  from the code section that called set_exchange_deadline(). */
void furi_hal_pn532_clear_exchange_deadline(void);

/** Read and return the PN532 status register (STS_REG) over I2C.
 * This is a hardware register read, not the software-ready flag.
 * The return value is true if the chip signals data-ready (RDY=1),
 * false otherwise or on I2C error. */
bool furi_hal_pn532_read_status(void);

/** Start the PN532 software ready poller.
 *
 * Legacy function name retained for API compatibility. This does not use a
 * PN532 INT/IRQ GPIO; it polls the PN532 I2C status byte over I2C1 SCL/SDA.
 * Called automatically by furi_hal_pn532_init() after a successful init.
 * Safe to call again if already running (no-op). */
void furi_hal_pn532_irq_start(void);

/** Stop the PN532 software ready poller.
 *
 * Legacy function name retained for API compatibility. Must be called during
 * NFC session teardown / HAL deinit. Safe to call even if the thread was never
 * started (no-op). */
void furi_hal_pn532_irq_stop(void);

/** Poll for an ISO14443-3A (NFC-A) target using InListPassiveTarget.
 * Uses default timeout (~500ms).  Populates target->atqa, sak, uid.
 * @param[out] target  Target descriptor to fill (required, not NULL).
 * @returns true if a card was found, false otherwise. */
bool furi_hal_pn532_poll_iso14443a(FuriHalPn532Target* target);

/** Poll for an ISO14443-3A target with a caller-specified timeout.
 * Useful when the default ~500 ms is too long (e.g. scanning fallback).
 * @param[out] target      Target descriptor to fill.
 * @param[in]  timeout_ms  Timeout in milliseconds.
 * @returns true if a card was found, false otherwise. */
bool furi_hal_pn532_poll_iso14443a_timeout(FuriHalPn532Target* target, uint32_t timeout_ms);

/** Decode a PN532 protocol status byte into a human-readable string.
 * @param[in] status_code  Lower 6 bits of the PN532 response status byte.
 * @returns A static string describing the status (never NULL).
 * @see furi_hal_pn532_error_str() for the transport-layer error enum. */
const char* furi_hal_pn532_strerror(uint8_t status_code);

/** @brief Return a human-readable description of a FuriHalPn532Error enum value.
 *
 * Maps the transport-level error enum returned by InDataExchange,
 * InCommunicateThru, TgSetData, and other PN532 command functions to
 * a short, readable string suitable for diagnostic logging.
 *
 * This differs from furi_hal_pn532_strerror() which decodes the PN532
 * protocol-level status byte (bits 0..5 of the InDataExchange response).
 * This helper maps the I2C transport-level error enum instead.
 *
 * @param[in] err  The FuriHalPn532Error value to describe.
 * @return  A static string (never NULL, never empty).
 */
const char* furi_hal_pn532_error_str(FuriHalPn532Error err);

/** @name SRIX Tag Operations
 *
 * Low-level InCommunicateThru wrappers for STMicroelectronics SRIX tags
 * (SRI512 / SRI4K / SRIX4K).  These operate at 106 kbps Type B framing
 * using the PN532's transparent thru mode to send/receive raw SRIX
 * command frames.
 *
 * @{ */
/** Detect an SRIX tag and return its Chip_ID (anti-collision).
 * @param[out] chip_id  The 8-bit chip identifier.
 * @returns true if a tag was detected. */
bool furi_hal_pn532_srix_detect(uint8_t* chip_id);

/** Select an SRIX tag by Chip_ID for subsequent read/write operations.
 * @param[in] chip_id  The chip identifier from furi_hal_pn532_srix_detect().
 * @returns true if selection was acknowledged. */
bool furi_hal_pn532_srix_select(uint8_t chip_id);

/** Read the UID of a selected SRIX tag.
 * @param[out] uid      Buffer (at least 8 bytes) for the UID.
 * @param[out] uid_len  Number of bytes written.
 * @returns true on success. */
bool furi_hal_pn532_srix_get_uid(uint8_t* uid, size_t* uid_len);

/** Read a single SRIX data block (4 bytes).
 * @param[in]  block_num  Block address (0..127 for SRIX4K).
 * @param[out] data       4-byte buffer for block contents.
 * @returns true on success. */
bool furi_hal_pn532_srix_read_block(uint8_t block_num, uint8_t* data);

/** Write a single SRIX data block (4 bytes).
 * @param[in] block_num  Block address.
 * @param[in] data       4-byte buffer with data to write.
 * @returns true on success. */
bool furi_hal_pn532_srix_write_block(uint8_t block_num, const uint8_t* data);
/** @} */

/** Send data to an in-listed target and receive its response (InDataExchange).
 *
 * Wraps the PN532 InDataExchange command (0x40).  The PN532 manages
 * ISO14443-3 framing automatically.  Use for standard APDU-style
 * exchanges with activated targets.
 *
 * @param[in]  target_number  Target index (from poll result).
 * @param[in]  tx_data        Bytes to send to the target.
 * @param[in]  tx_len         Length of tx_data.
 * @param[out] rx_data        Buffer for the response payload.
 * @param[in]  rx_size        Capacity of rx_data.
 * @param[out] rx_len         Number of bytes written to rx_data (may be NULL).
 * @return FuriHalPn532ErrorNone on success, FuriHalPn532ErrorComm on
 *         PN532 protocol error (including card-side NACK / timeout). */
FuriHalPn532Error furi_hal_pn532_in_data_exchange(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len);

/** InDataExchange variant that exposes the raw PN532 status byte to the caller.
 *
 * Identical to furi_hal_pn532_in_data_exchange() except that the PN532 status
 * byte (the first byte after the command echo in the chip response) is written
 * to *pn532_status before being stripped from the returned payload.
 *
 * Status byte layout (per PN532 User Manual §7.3.6):
 *   bits 0..5  Error code (0 = success); see furi_hal_pn532_strerror()
 *   bit  6     0x40 = "More Information" / card-side ISO14443-4 chaining
 *              active.  When set, the card has more data to send and the
 *              caller must issue an R(ACK) via a follow-up InDataExchange
 *              call to retrieve the next fragment.  This is how
 *              furi_hal_nfc_pn532_exchange_internal() drives multi-fragment
 *              ISO-DEP responses without truncating them.
 *   bit  7     reserved
 *
 * Returns FuriHalPn532ErrorComm if the lower 6 bits of the status byte are
 * non-zero (i.e., the PN532 reported an error).  pn532_status is still
 * populated in that case so the caller can inspect the failure mode.
 *
 * @param[in]  target_number  Target number returned by InListPassiveTarget.
 * @param[in]  tx_data        Bytes to send to the activated target.
 * @param[in]  tx_len         Length of tx_data, in bytes.
 * @param[out] rx_data        Buffer that receives the card response payload
 *                            (PN532 status byte already stripped).
 * @param[in]  rx_size        Size of rx_data, in bytes.
 * @param[out] rx_len         Optional: number of bytes written into rx_data.
 * @param[out] pn532_status   Optional: raw PN532 status byte.  Pass NULL if
 *                            the caller does not need to detect chaining.
 */
FuriHalPn532Error furi_hal_pn532_in_data_exchange_ex(
    uint8_t target_number,
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint8_t* pn532_status);

/** Send raw bytes to a tag via InCommunicateThru (transparent mode).
 *
 * Bypasses PN532's ISO14443-3/4 framing — the raw bytes are sent
 * bit-encoded to the RF field as-is.  Use for MIFARE Classic auth
 * (0x60/0x61), SRIX commands, Jewel/Topaz READ_ALL, and any other
 * protocol that PN532 does not natively support.
 *
 * @param[in]  tx_data  Raw bytes to transmit.
 * @param[in]  tx_len   Number of bytes to transmit.
 * @param[out] rx_data  Buffer for the card response.
 * @param[in]  rx_size  Capacity of rx_data.
 * @param[out] rx_len   Number of bytes received (may be NULL).
 * @return FuriHalPn532ErrorNone on success, or a transport/status error. */
FuriHalPn532Error furi_hal_pn532_in_communicate_thru(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len);

/** InCommunicateThru with caller-specified timeout.
 * @param[in]  tx_data     Raw bytes to transmit.
 * @param[in]  tx_len      Number of bytes to transmit.
 * @param[out] rx_data     Buffer for the card response.
 * @param[in]  rx_size     Capacity of rx_data.
 * @param[out] rx_len      Number of bytes received (may be NULL).
 * @param[in]  timeout_ms  I2C transaction timeout in milliseconds.
 * @see furi_hal_pn532_in_communicate_thru() */
FuriHalPn532Error furi_hal_pn532_in_communicate_thru_timeout(
    const uint8_t* tx_data,
    size_t tx_len,
    uint8_t* rx_data,
    size_t rx_size,
    size_t* rx_len,
    uint32_t timeout_ms);

/** Send a raw command frame to the PN532 over I2C (no ACK validation).
 * Low-level primitive.  Most callers should use the higher-level wrappers
 * (in_data_exchange, in_communicate_thru, etc.) instead.
 * @param[in] cmd      Command buffer (incl. TFI + checksum).
 * @param[in] cmd_len  Total command length in bytes.
 * @return FuriHalPn532ErrorNone on successful I2C write. */
FuriHalPn532Error furi_hal_pn532_send_command(const uint8_t* cmd, size_t cmd_len);

/** Read a raw response frame from the PN532 over I2C (incl. ACK).
 * Waits for RDY=1 via I2C status polling, then reads N+1 bytes.
 * @param[out] data       Buffer for the response payload.
 * @param[in]  data_size  Capacity of data buffer.
 * @param[out] data_len   Number of bytes written.
 * @param[in]  timeout_ms I2C polling timeout.
 * @return FuriHalPn532ErrorNone on success. */
FuriHalPn532Error furi_hal_pn532_read_response(
    uint8_t* data,
    size_t data_size,
    size_t* data_len,
    uint32_t timeout_ms);

/** Send InRelease (0x52) to release all in-listed targets.
 * Best-effort: uses the two-strikes-out ACK mechanism so a single transient
 * I2C glitch does NOT set pn532_ready=false and kill the NFC session. */
void furi_hal_pn532_in_release(void);

/** Poll for a FeliCa (JIS X 6319-4) target using InListPassiveTarget.
 * Populates target->pmm (8-byte PMm / NFC-ID2) and target->uid.
 * @param[out] target  Target descriptor to fill (required).
 * @returns true if a FeliCa card was found, false otherwise. */
bool furi_hal_pn532_poll_felica(FuriHalPn532Target* target);

/** Poll for an ISO14443-3B (NFC-B) target using InListPassiveTarget.
 * Populates target->uid, app_data, and proto_info from the ATQB.
 * @param[out] target  Target descriptor to fill (required).
 * @returns true if a Type B card was found, false otherwise. */
bool furi_hal_pn532_poll_iso14443b(FuriHalPn532Target* target);

/** Poll for Innovision Jewel / Topaz (NFC Type 1 Tag) cards.
 *
 * Uses InListPassiveTarget with BrTy=0x04 (Jewel/Topaz at 106 kbps).
 * On success, target->uid contains the 6-byte RID response (HR0, HR1, UID0..UID3)
 * and target->uid_len is set to 6.  target->atqa[0] is set to 0x0C (Jewel marker).
 *
 * @param[out] target  Pointer to target struct to fill, or NULL for presence-only check.
 * @returns true if a Jewel/Topaz card was found, false otherwise.
 */
bool furi_hal_pn532_poll_jewel(FuriHalPn532Target* target);

/** Poll for ISO15693 (vicinity) cards using InListPassiveTarget BrTy=0x05.
 *
 * Sends an INVENTORY request (flags 0x26, no mask) at 26 kbps.  On success,
 * target->uid contains the 8-byte UID in MSB-first order (reversed from the
 * PN532 LSB-first wire format) and target->uid_len is set to 8.  The DSFID
 * byte is stored in target->atqa[0] so upper layers that need it can read
 * it without a separate API call.
 *
 * @param[out] target  Pointer to target struct to fill, or NULL for
 *                     presence-only check.
 * @returns true if an ISO15693 card was found, false otherwise (including
 *          I2C errors, no card present, or short/malformed response).
 */
bool furi_hal_pn532_poll_iso15693(FuriHalPn532Target* target);

/** Authenticate with a MIFARE Classic sector using InDataExchange.
 * Sends MFAuthent (0x60/0x61) with the provided key and UID.
 * @param[in] target_number  Target index (from poll result).
 * @param[in] block_num      Block number within the sector to auth.
 * @param[in] key            6-byte sector key.
 * @param[in] key_type       0x60 = Key A, 0x61 = Key B.
 * @param[in] uid            Card UID (4 or 7 bytes).
 * @param[in] uid_len        Length of uid.
 * @return FuriHalPn532ErrorNone on successful auth. */
FuriHalPn532Error furi_hal_pn532_mf_auth(
    uint8_t target_number,
    uint8_t block_num,
    const uint8_t* key,
    uint8_t key_type,
    const uint8_t* uid,
    uint8_t uid_len);

/** Authenticate to a MIFARE Classic block via backdoor InCommunicateThru.
 * Sends raw MFAuthent (0x60/0x61) followed by sector key over
 * InCommunicateThru (bypassing PN532's InDataExchange path).
 * Used for cards that do not respond to standard InDataExchange auth.
 * @param[in] block_num     Block to authenticate to.
 * @param[in] key_type      0x60 = Key A, 0x61 = Key B.
 * @param[in] key           6-byte sector key.
 * @param[in] backdoor_type Backdoor variant (reserved for future use).
 * @returns true if auth was acknowledged. */
bool furi_hal_pn532_mf_backdoor_auth(
    uint8_t block_num,
    uint8_t key_type,
    const uint8_t* key,
    uint8_t backdoor_type);

/** Write block 0 (manufacturer block) of a MIFARE Classic card via
 * backdoor InCommunicateThru.  Requires prior successful auth with
 * furi_hal_pn532_mf_backdoor_auth().  After writing, increment the
 * UID BCC byte so the card remains internally consistent.
 * @param[in] block_num   Block to overwrite.
 * @param[in] block_data  16-byte block data (can modify UID + BCC).
 * @returns true on success. */
bool furi_hal_pn532_mf_backdoor_write_block0(uint8_t block_num, const uint8_t* block_data);

/** @name PN532 Target/Listener Mode Functions
 *
 * Put the PN532 into target (card-emulation) mode so it responds to
 * an external reader's REQA/WUPA.  Used for ISO14443-3A and ISO-DEP
 * emulation (UID-only; full tag emulation handled by caller's state
 * machine).  Not supported on all PN532 firmware versions.
 *
 * @{ */
/** Initialise the PN532 as a passive target (card emulation).
 * The chip will listen for a reader field and respond according to
 * the provided initialisation parameters (ATQA, SAK, UID, etc.).
 * @param[in] params      Initialisation data (protocol-specific).
 * @param[in] params_len  Length of params in bytes.
 * @param[in] timeout_ms  Maximum time to wait for reader activation.
 * @return FuriHalPn532ErrorNone if a reader activated the emulated card. */
FuriHalPn532Error
    furi_hal_pn532_tg_init_as_target(const uint8_t* params, size_t params_len, uint32_t timeout_ms);

/** Receive data from the external reader while in target mode.
 * Blocks until the reader sends a command frame or timeout expires.
 * @param[out] buf       Buffer for incoming data.
 * @param[in]  buf_size  Capacity of buf.
 * @param[out] out_len   Number of bytes received.
 * @param[in]  timeout_ms  Receive timeout.
 * @return FuriHalPn532ErrorNone on data received. */
FuriHalPn532Error
    furi_hal_pn532_tg_get_data(uint8_t* buf, size_t buf_size, size_t* out_len, uint32_t timeout_ms);

/** Send a response back to the external reader while in target mode.
 * @param[in] data      Response bytes to send.
 * @param[in] data_len  Number of bytes to send.
 * @return FuriHalPn532ErrorNone on success. */
FuriHalPn532Error furi_hal_pn532_tg_set_data(const uint8_t* data, size_t data_len);
/** @} */

#ifdef __cplusplus
}
#endif
