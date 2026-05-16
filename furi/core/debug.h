#ifndef FURI_DEBUG_H
#define FURI_DEBUG_H

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start timing measurement
 *
 * Usage:
 * @code
 * FURI_DEBUG_TIMING_START(operation_name);
 * // ... code to measure ...
 * FURI_DEBUG_TIMING_END(operation_name, "TAG");
 * @endcode
 */
#define FURI_DEBUG_TIMING_START(name) uint32_t name##_start = furi_get_tick()

/**
 * @brief End timing measurement and log result
 *
 * @param name Name used in FURI_DEBUG_TIMING_START
 * @param tag Log tag for output
 */
#define FURI_DEBUG_TIMING_END(name, tag) \
    FURI_LOG_D(tag, "%s took %lu ms", #name, furi_get_tick() - name##_start)

/**
 * @brief Conditional breakpoint
 *
 * Triggers hardware breakpoint if condition is true
 *
 * @param cond Condition to check
 */
#define FURI_DEBUG_BREAK_IF(cond) \
    do {                          \
        if(cond) {                \
            __BKPT(0);            \
        }                         \
    } while(0)

/**
 * @brief Log current stack usage
 *
 * Logs remaining heap space for current thread
 */
#define FURI_DEBUG_STACK_CHECK() \
    FURI_LOG_D("STACK", "Free: %lu bytes", furi_thread_get_heap_size())

/**
 * @brief Assert with message
 *
 * Similar to furi_check but with custom message
 *
 * @param cond Condition to check
 * @param msg Message to log on failure
 */
#define FURI_DEBUG_ASSERT(cond, msg)         \
    do {                                     \
        if(!(cond)) {                        \
            FURI_LOG_E("ASSERT", "%s", msg); \
            furi_crash();                    \
        }                                    \
    } while(0)

/**
 * @brief Log function entry
 *
 * Logs when function is entered
 */
#define FURI_DEBUG_LOG_ENTRY(tag) FURI_LOG_T(tag, "Entry: %s", __func__)

/**
 * @brief Log function exit
 *
 * Logs when function is exited
 */
#define FURI_DEBUG_LOG_EXIT(tag) FURI_LOG_T(tag, "Exit: %s", __func__)

/**
 * @brief Log function entry and exit
 *
 * Automatically logs entry and exit
 *
 * Usage:
 * @code
 * void my_function() {
 *     FURI_DEBUG_LOG_ENTRY_EXIT("TAG");
 *     // ... function body ...
 * }
 * @endcode
 */
#define FURI_DEBUG_LOG_ENTRY_EXIT(tag) \
    FURI_DEBUG_LOG_ENTRY(tag);         \
    FuriDebugLogExit __log_exit_##__LINE__(tag)

/**
 * @brief RAII-style function exit logger
 *
 * Internal helper for FURI_DEBUG_LOG_ENTRY_EXIT
 */
typedef struct {
    const char* tag;
} FuriDebugLogExit;

static inline void furi_debug_log_exit_dtor(FuriDebugLogExit* log) {
    FURI_LOG_T(log->tag, "Exit: %s", __func__);
}

#define FURI_DEBUG_LOG_ENTRY_EXIT(tag)                 \
    FuriDebugLogExit __log_exit_##__LINE__ = {tag};    \
    __attribute__((cleanup(furi_debug_log_exit_dtor))) \
    FuriDebugLogExit* __log_exit_ptr_##__LINE__ = &__log_exit_##__LINE__

/**
 * @brief Measure and log function execution time
 *
 * Automatically logs execution time when function exits
 *
 * Usage:
 * @code
 * void my_function() {
 *     FURI_DEBUG_TIMING_FUNCTION("TAG");
 *     // ... function body ...
 * }
 * @endcode
 */
#define FURI_DEBUG_TIMING_FUNCTION(tag) \
    FURI_DEBUG_TIMING_START(__func__);  \
    FuriDebugTiming __timing_##__LINE__(tag, #__func__)

/**
 * @brief RAII-style function timing logger
 *
 * Internal helper for FURI_DEBUG_TIMING_FUNCTION
 */
typedef struct {
    const char* tag;
    const char* name;
} FuriDebugTiming;

static inline void furi_debug_timing_dtor(FuriDebugTiming* timing) {
    FURI_LOG_D(
        timing->tag, "%s took %lu ms", timing->name, furi_get_tick() - timing->name##_start);
}

#define FURI_DEBUG_TIMING_FUNCTION(tag)                     \
    FuriDebugTiming __timing_##__LINE__ = {tag, #__func__}; \
    __attribute__((cleanup(furi_debug_timing_dtor)))        \
    FuriDebugTiming* __timing_ptr_##__LINE__ = &__timing_##__LINE__

/**
 * @brief Log variable value
 *
 * Logs variable name and value
 *
 * @param var Variable to log
 * @param tag Log tag
 */
#define FURI_DEBUG_LOG_VAR(var, tag) FURI_LOG_D(tag, "%s = %lu", #var, (uint32_t)(var))

/**
 * @brief Log variable value in hex
 *
 * Logs variable name and value in hexadecimal
 *
 * @param var Variable to log
 * @param tag Log tag
 */
#define FURI_DEBUG_LOG_VAR_HEX(var, tag) FURI_LOG_D(tag, "%s = 0x%08lX", #var, (uint32_t)(var))

/**
 * @brief Log buffer contents
 *
 * Logs buffer in hex format
 *
 * @param buf Buffer to log
 * @param len Buffer length
 * @param tag Log tag
 */
#define FURI_DEBUG_LOG_BUFFER(buf, len, tag)                      \
    do {                                                          \
        FURI_LOG_D(tag, "%s (%u bytes):", #buf, (uint32_t)(len)); \
        for(uint32_t i = 0; i < (uint32_t)(len); i++) {           \
            FURI_LOG_RAW_D(tag, "%02X ", ((uint8_t*)(buf))[i]);   \
            if((i + 1) % 16 == 0) FURI_LOG_RAW_D(tag, "\n");      \
        }                                                         \
        FURI_LOG_RAW_D(tag, "\n");                                \
    } while(0)

/**
 * @brief Count and log function calls
 *
 * Increments counter and logs periodically
 *
 * Usage:
 * @code
 * void my_function() {
 *     FURI_DEBUG_COUNT_CALLS("TAG", 100);
 *     // ... function body ...
 * }
 * @endcode
 *
 * @param tag Log tag
 * @param interval Log interval (every N calls)
 */
#define FURI_DEBUG_COUNT_CALLS(tag, interval)                        \
    do {                                                             \
        static uint32_t __call_count_##tag = 0;                      \
        __call_count_##tag++;                                        \
        if(__call_count_##tag % (interval) == 0) {                   \
            FURI_LOG_D(tag, "Called %lu times", __call_count_##tag); \
        }                                                            \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif // FURI_DEBUG_H
