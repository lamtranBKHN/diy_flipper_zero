# Flipper Zero Codebase Debugging Implementation Prompt

## Objective
To systematically debug the Flipper Zero codebase by following the comprehensive analysis plan we've created, examining each component line-by-line to ensure thorough understanding and verification of the codebase functionality.

## Implementation Steps

### 1. Environment Setup
- Ensure the Flipper Zero development environment is properly configured
- Verify all necessary tools are installed (ARM toolchain, Python dependencies)
- Confirm access to the device or emulator for testing

### 2. Phase 1: System Initialization Analysis

#### 2.1 Main Application Entry Point Analysis
- Examine `targets/f7/src/main.c` line-by-line
- Verify `furi_init()` function implementation
- Confirm `furi_hal_init_early()` execution flow
- Validate boot mode detection and handling
- Check thread initialization process

#### 2.2 FURI Core Framework Analysis
- Review `furi/core/thread.c` for thread management implementation
- Analyze `furi/core/memmgr.c` for memory management practices
- Examine `furi/core/event_loop.c` for event handling mechanisms
- Inspect `furi/core/timer.c` for timer system functionality

### 3. Phase 2: Hardware Abstraction Layer Debugging

#### 3.1 GPIO Configuration Analysis
- Examine `furi_hal_gpio.c` for pin configuration implementation
- Verify interrupt handling in GPIO module
- Confirm pull-up/pull-down configuration procedures
- Validate EXTI line handling

#### 3.2 Resource Management Review
- Analyze `furi_hal_resources.c` for resource mapping
- Verify button handling via PCF8574
- Check SPI/I2C interface configuration

### 4. Phase 3: Core Functionality Modules Analysis

#### 4.1 Unit Test Framework
- Examine `applications/debug/unit_tests/` for test runner implementation
- Validate plugin loading and execution procedures
- Confirm memory leak detection mechanisms
- Review performance timing analysis implementation

### 5. Phase 4: Detailed Component Analysis

#### 5.1 Memory Management Debugging
- Analyze `furi/core/memmgr.c` for memory allocation tracking
- Verify heap usage monitoring
- Check memory leak detection in application context

#### 5.2 Threading System Debugging
- Examine `furi/core/thread.c` for thread creation and management
- Verify stack overflow detection mechanisms
- Confirm context switching analysis
- Validate priority management

#### 5.3 Hardware Interface Debugging
- Review `furi_hal/gpio.c` for pin configuration verification
- Examine interrupt handling analysis
- Confirm hardware register configuration
- Validate clock domain management

### 6. Phase 5: Communication and Storage Testing

#### 6.1 USB and Communication Debugging
- Test USB communication protocols in `furi_hal/usb.c`
- Verify interface testing with external devices
- Confirm power management functionality

#### 6.2 Storage and File System Debugging
- Test SD card interface
- Validate file operation implementation
- Confirm directory management

### 7. Verification and Testing

#### 7.1 Execute Unit Tests
- Run unit tests for each component
- Verify integration points between modules
- Confirm communication protocol validation
- Validate error handling completeness

#### 7.2 Performance Monitoring
- Track memory usage throughout execution
- Analyze CPU utilization patterns
- Measure thread execution timing
- Evaluate interrupt response times
- Profile power consumption

### 8. Documentation and Reporting

#### 8.1 Debug Session Output
- Generate memory usage reports (current/peak usage)
- Document performance metrics (function execution times)
- Analyze error handling and exceptions
- Verify component interaction status
- Confirm hardware interface validation

## Implementation Commands

### Build System Commands
```bash
# Build with debug symbols
./fbt DEBUG=1

# GDB debugging
./fbt debug

# Black Magic Probe debugging
./fbt blackmagic

# Code linting
./fbt lint_all

# Code formatting
./fbt format_all
```

### Hardware Testing Commands
```bash
# Flash firmware
./fbt flash

# USB flashing
./fbt flash_usb_full

# Update package creation
./fbt updater_package

# Core2 firmware packaging
./fbt copro_dist
```

## Expected Outcomes

1. Complete line-by-line analysis of the Flipper Zero codebase
2. Identification of any bugs or issues in the implementation
3. Performance optimization recommendations
4. Verification of all hardware interfaces
5. Complete testing of all core functionality modules
6. Comprehensive documentation of findings

## Success Criteria

1. All core system functions have been analyzed and verified
2. Hardware interfaces have been tested and validated
3. Unit tests pass successfully
4. Memory management is efficient and leak-free
5. Thread safety mechanisms are properly implemented
6. Performance metrics meet expected standards
7. All integration points function correctly
8. Error handling is comprehensive and robust

## Next Steps

1. Begin Phase 1: System Initialization Analysis
2. Proceed through each phase systematically
3. Document findings at each step
4. Verify all components against success criteria
5. Generate final comprehensive report