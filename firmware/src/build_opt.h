// build_opt.h
// Extra compiler flags injected by the Arduino Mbed core at build time.
// Every non-comment line here is appended verbatim to every gcc invocation
// for sketch source files.
//
// IMPORTANT NOTE — pre-compiled library limitation:
//   The Arduino Mbed Nicla core ships a pre-compiled libmbed.a. Flags set
//   here affect only the sketch source files, NOT the pre-compiled library.
//   Therefore:
//
//   MBED_HEAP_STATS_ENABLED=1  — has NO effect here.
//     mbed_stats_heap_get() is compiled out of libmbed.a; the symbol does not
//     exist in the library. Adding the flag only in build_opt.h would cause
//     the header to declare the function but the linker would fail to find it.
//     Alternative used: _sbrk(0) + mbed_heap_start/mbed_heap_size (mbed_boot.h)
//
//   OS_STACK_WATERMARK=1  — has NO effect here.
//     RTX5 osThreadGetStackSpace() returns 0U unconditionally when watermarking
//     is compiled out of libmbed.a. Stack peak monitoring is not available
//     with the pre-built core; only allocated stack size can be reported.
//
// Add actual sketch-level flags below if needed in the future.

// ── USB CDC TX buffer size ─────────────────────────────────────────────────
// The Arduino Mbed Nicla core compiles USBCDC.cpp from source (not in
// libmbed.a), so this flag is effective here.
//
// Default USB_TX_SIZE is 256 B.  At 1 kSPS with 4 ch (≈16 kB/s) that gives
// only ~16 ms of headroom before Serial.write() blocks waiting for the host.
// Python OS-scheduler jitter can easily exceed 16 ms, risking a WDT reset.
//
// 2048 B → ≈128 ms headroom; enough to survive normal host scheduling pauses
// without any blocking on the EEG TX path.
-DUSB_TX_SIZE=2048
