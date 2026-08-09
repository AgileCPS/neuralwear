/**
 * @file    task.cpp
 * @brief   Implementation of BaseTask — abstract RTOS task base class.
 */

#include "task.h"

#ifdef DEBUG_ENABLE
#include <cstdio>
#include <cstring>
#endif

#ifdef DEBUG_ENABLE
// Defined here (one translation unit); declared extern in task.h.
rtos::Mutex gSerialMutex;

bool debugTryPrint(const char* msg) {
    if (msg == nullptr) {
        return false;
    }

    if (!gSerialMutex.trylock()) {
        return false;
    }

    size_t len = strlen(msg);
    if (len == 0) {
        gSerialMutex.unlock();
        return true;
    }

    if (Serial.availableForWrite() < (int)len) {
        gSerialMutex.unlock();
        return false;
    }

    size_t written = Serial.write(reinterpret_cast<const uint8_t*>(msg), len);
    gSerialMutex.unlock();
    return written == len;
}
#endif


// ═════════════════════════════════════════════════════════════════════════════
// BaseTask Implementation
// ═════════════════════════════════════════════════════════════════════════════

BaseTask::BaseTask(osPriority priority, uint32_t stackSize)
    : _thread(nullptr),
      _stopRequested(false),
      _isRunning(false),
      _wakeSem(0, 1),   // binary: initial count=0, max count=1
      _priority(priority),
      _stackSize(stackSize)
{
    // Thread is NOT created here — start() will spawn it
}


BaseTask::~BaseTask() {
    // Ensure thread is stopped before destruction
    if (_isRunning) {
        stop();
    }

    // Clean up thread object if it was allocated
    if (_thread != nullptr) {
        delete _thread;
        _thread = nullptr;
    }
}


void BaseTask::start() {
    // Guard against double-start
    if (_isRunning) {
        return;  // Already running; ignore
    }

    // Reset the stop flag
    _stopRequested = false;

    // Allocate and configure the thread
    _thread = new rtos::Thread(_priority, _stackSize);

    // Start the thread with our static entry point
    _thread->start(mbed::callback(BaseTask::threadEntry, this));

    // Mark as running
    _isRunning = true;
}


void BaseTask::stop() {
    // Guard against double-stop
    if (!_isRunning) {
        return;  // Not running; ignore
    }

    // Signal the thread to stop
    _stopRequested = true;

    // Block until the thread terminates
    if (_thread != nullptr) {
        _thread->join();
    }

    // Mark as stopped
    _isRunning = false;
}


void BaseTask::threadEntry(void* arg) {
    // Cast the argument back to BaseTask instance
    BaseTask* task = static_cast<BaseTask*>(arg);

    // Call the instance's pure virtual run() method
    task->run();

    // run() has returned — thread exits naturally
}


void BaseTask::notify() {
    // Release the binary semaphore to wake sleepUntilNotified().
    // If the semaphore is already at max count (task is awake), release()
    // returns osErrorResource — ignored intentionally.
    _wakeSem.release();
}


void BaseTask::sleepUntilNotified(uint32_t timeout_ms) {
    // Block until notify() fires or the timeout elapses.
    // try_acquire_for() drains one token; if none arrives within timeout_ms
    // it returns false and the loop continues (periodic poll safety net).
    _wakeSem.try_acquire_for(std::chrono::milliseconds(timeout_ms));
}


void BaseTask::reportHealth(const char* taskName) {
#ifdef DEBUG_ENABLE
    // --- Stack ---
    // osThreadGetStackSize() is always valid (reads creation record).
    // osThreadGetStackSpace() requires OS_STACK_WATERMARK=1 in libmbed.a;
    // the pre-built Arduino Mbed core has it disabled → always returns 0.
    // We report only the allocated size; label peak-free honestly as N/A.
    uint32_t stackAllocated = osThreadGetStackSize(osThreadGetId());

    // --- Heap (system-wide) ---
    // _sbrk(0) returns the current heap break — the end of the dlmalloc arena.
    // heapClaimed = bytes from heap base to break (includes live + freed-but-retained).
    // heapFree    = arena bytes beyond the break (not yet touched by any allocation).
    uint32_t heapClaimed = (uint32_t)((char*)_sbrk(0) - (char*)mbed_heap_start);
    uint32_t heapFree    = (heapClaimed <= mbed_heap_size)
                               ? (mbed_heap_size - heapClaimed) : 0;

    char dbgBuf[160];
    snprintf(dbgBuf, sizeof(dbgBuf),
        "[%s HEALTH] Stack: %lu B alloc, peak-free=N/A | HeapFree: %lu B (claimed: %lu/%lu B)\r\n",
        taskName,
        (unsigned long)stackAllocated,
        (unsigned long)heapFree,
        (unsigned long)heapClaimed,
        (unsigned long)mbed_heap_size);
    debugTryPrint(dbgBuf);
#else
    (void)taskName;
#endif
}
