# Profiling Flipper Zero Firmware

## Overview

Flipper Zero firmware doesn't include built-in profiling tools, but several methods are available for measuring performance and identifying bottlenecks.

## Manual Instrumentation

### Timing Measurement

Use `furi_get_tick()` for coarse-grained timing (1ms resolution):

```c
#include <furi.h>

void nfc_operation() {
    uint32_t start = furi_get_tick();

    // ... code to measure ...

    uint32_t elapsed = furi_get_tick() - start;
    FURI_LOG_D("PROFILE", "NFC operation took %lu ms", elapsed);
}
```

### Using Debug Helper Macros

Include `furi/core/debug.h` for convenience:

```c
#include <furi/core/debug.h>

void nfc_operation() {
    FURI_DEBUG_TIMING_START(nfc_operation);

    // ... code to measure ...

    FURI_DEBUG_TIMING_END(nfc_operation, "NFC");
}
```

Output:
```
12345 [D][PROFILE] nfc_operation took 45 ms
```

### High-Resolution Timing

For microsecond-level timing, use DWT cycle counter:

```c
#include <furi.h>

void precise_timing() {
    uint32_t start = DWT->CYCCNT;
    __DSB();

    // ... code to measure ...

    __DSB();
    uint32_t cycles = DWT->CYCCNT - start;
    uint32_t microseconds = cycles / (SystemCoreClock / 1000000);

    FURI_LOG_D("PROFILE", "Operation took %lu us (%lu cycles)", microseconds, cycles);
}
```

Note: DWT must be enabled first:

```c
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CYCCNT = 0;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
```

## FreeRTOS Task Analysis

### Task State Inspection

Use GDB to inspect task states:

```gdb
# Show all tasks
(gdb) show Task-List

# Output example:
# Task          State   Priority  Stack   Num
# -------------------------------------------
# nfc_worker    Running 5         1024    1
# gui           Blocked  3         2048    2
# cli           Blocked  2         512     3
```

### Stack Usage

Check stack high water mark:

```c
#include <FreeRTOS.h>
#include <task.h>

void check_stack_usage() {
    UBaseType_t stack_high_water_mark = uxTaskGetStackHighWaterMark(NULL);
    FURI_LOG_D("STACK", "Stack remaining: %u bytes", stack_high_water_mark * 4);
}
```

### Task CPU Time

FreeRTOS doesn't include built-in CPU time tracking. For custom implementation:

```c
#include <furi.h>

typedef struct {
    uint32_t total_cycles;
    uint32_t last_switch;
} TaskProfile;

void vApplicationSwitchOutHook(TaskHandle_t task) {
    TaskProfile* profile = pvTaskGetThreadLocalStoragePointer(task, 0);
    if(profile) {
        profile->total_cycles += DWT->CYCCNT - profile->last_switch;
    }
}

void vApplicationSwitchInHook(TaskHandle_t task) {
    TaskProfile* profile = pvTaskGetThreadLocalStoragePointer(task, 0);
    if(profile) {
        profile->last_switch = DWT->CYCCNT;
    }
}
```

## GPIO Trace

### Timing with Logic Analyzer

Configure GPIO for timing measurement:

```c
#include <furi_hal_gpio.h>

void profile_with_gpio() {
    // Configure GPIO as output
    furi_hal_gpio_init(&gpio_ext_pa4, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);

    // Start measurement
    furi_hal_gpio_write(&gpio_ext_pa4, true);

    // ... code to measure ...

    // End measurement
    furi_hal_gpio_write(&gpio_ext_pa4, false);
}
```

Use logic analyzer or oscilloscope to measure pulse width.

### Multiple Events

Use multiple GPIO pins for different events:

```c
void profile_multiple_events() {
    // Event 1 start
    furi_hal_gpio_write(&gpio_ext_pa4, true);
    // ... event 1 code ...
    furi_hal_gpio_write(&gpio_ext_pa4, false);

    // Event 2 start
    furi_hal_gpio_write(&gpio_ext_pa5, true);
    // ... event 2 code ...
    furi_hal_gpio_write(&gpio_ext_pa5, false);
}
```

## Log-Based Profiling

### Event Counting

Count events using static variables:

```c
void nfc_poll() {
    static uint32_t poll_count = 0;
    static uint32_t success_count = 0;

    poll_count++;

    if(nfc_poll_card()) {
        success_count++;
    }

    // Log statistics periodically
    if(poll_count % 100 == 0) {
        FURI_LOG_D("PROFILE", "Polls: %lu, Success: %lu (%.1f%%)",
                   poll_count, success_count,
                   (success_count * 100.0f) / poll_count);
    }
}
```

### Latency Tracking

Track operation latency:

```c
void track_latency(uint32_t latency_ms) {
    static uint32_t min_latency = UINT32_MAX;
    static uint32_t max_latency = 0;
    static uint32_t total_latency = 0;
    static uint32_t count = 0;

    if(latency_ms < min_latency) min_latency = latency_ms;
    if(latency_ms > max_latency) max_latency = latency_ms;
    total_latency += latency_ms;
    count++;

    if(count % 100 == 0) {
        FURI_LOG_D("PROFILE", "Latency: min=%lu, max=%lu, avg=%lu",
                   min_latency, max_latency, total_latency / count);
    }
}
```

## Memory Profiling

### Heap Usage

Check heap usage:

```bash
# Via CLI
> free
```

Output:
```
Heap total: 131072
Heap used: 45678
Heap free: 85394
Heap blocks: 123
```

### Allocation Tracking

Track allocations in specific code:

```c
#include <furi.h>

void track_allocations() {
    static size_t total_allocated = 0;
    static size_t allocation_count = 0;

    void* buffer = furi_alloc(1024);
    if(buffer) {
        total_allocated += 1024;
        allocation_count++;

        FURI_LOG_D("MEM", "Allocated: %zu bytes (total: %zu, count: %zu)",
                   1024, total_allocated, allocation_count);

        // ... use buffer ...

        free(buffer);
        total_allocated -= 1024;
    }
}
```

## Common Performance Issues

### Blocking in ISR

Avoid blocking operations in interrupt handlers:

```c
// BAD: Blocking in ISR
void i2c_isr() {
    while(!i2c_ready()) {  // Can cause system lockup
        // waiting...
    }
}

// GOOD: Use deferred processing
void i2c_isr() {
    furi_hal_gpio_write(&gpio_ext_pa4, true);  // Signal event
    furi_thread_flags_set(thread_handle, FLAG_I2C_READY);
}
```

### Excessive Logging

High-frequency logging can impact performance:

```c
// BAD: Logging in tight loop
for(int i = 0; i < 1000; i++) {
    FURI_LOG_D("LOOP", "Iteration %d", i);  // Very slow
}

// GOOD: Log periodically
for(int i = 0; i < 1000; i++) {
    if(i % 100 == 0) {
        FURI_LOG_D("LOOP", "Iteration %d", i);
    }
}
```

### Memory Fragmentation

Frequent allocations can cause fragmentation:

```c
// BAD: Frequent allocations
void process_data() {
    for(int i = 0; i < 100; i++) {
        void* buffer = furi_alloc(1024);
        // ... process ...
        free(buffer);
    }
}

// GOOD: Reuse buffer
void process_data() {
    void* buffer = furi_alloc(1024);
    for(int i = 0; i < 100; i++) {
        // ... process with buffer ...
    }
    free(buffer);
}
```

## Optimization Tips

### Reduce Log Level

Set log level to error or warn in production:

```bash
> sysctl log_level error
```

### Use Stack Allocation

Prefer stack allocation over heap for small buffers:

```c
// GOOD: Stack allocation
void process() {
    uint8_t buffer[256];
    // ... use buffer ...
}

// AVOID: Heap allocation for small buffers
void process() {
    uint8_t* buffer = furi_alloc(256);
    // ... use buffer ...
    free(buffer);
}
```

### Minimize Context Switches

Batch operations to reduce context switches:

```c
// GOOD: Batch processing
void process_batch() {
    for(int i = 0; i < 10; i++) {
        process_item(i);
    }
    furi_thread_flags_set(thread_handle, FLAG_DONE);
}

// AVOID: Frequent signaling
void process_item_by_item() {
    for(int i = 0; i < 10; i++) {
        process_item(i);
        furi_thread_flags_set(thread_handle, FLAG_PROGRESS);  // Excessive
    }
}
```

## Debugging Performance Issues

### Identify Slow Function

Use timing macros to identify slow functions:

```c
void suspect_function() {
    FURI_DEBUG_TIMING_START(suspect_function);

    // ... code ...

    FURI_DEBUG_TIMING_END(suspect_function, "PROFILE");
}
```

### Check Task States

Use GDB to check if tasks are blocked:

```gdb
(gdb) show Task-List
```

Look for tasks stuck in "Blocked" state unexpectedly.

### Check Interrupt Frequency

High interrupt frequency can cause performance issues:

```c
// Count interrupts
volatile uint32_t interrupt_count = 0;

void i2c_isr() {
    interrupt_count++;
    // ... handle interrupt ...
}

// Log periodically
void log_interrupt_rate() {
    static uint32_t last_count = 0;
    static uint32_t last_time = 0;

    uint32_t now = furi_get_tick();
    uint32_t elapsed = now - last_time;
    uint32_t delta = interrupt_count - last_count;

    FURI_LOG_D("IRQ", "Rate: %lu/sec", (delta * 1000) / elapsed);

    last_count = interrupt_count;
    last_time = now;
}
```

## Additional Resources

- `documentation/Debugging.md` - General debugging guide
- `furi/core/kernel.c` - Timing functions
- `furi/core/thread.h` - Thread management
- `FreeRTOS documentation` - Task profiling
