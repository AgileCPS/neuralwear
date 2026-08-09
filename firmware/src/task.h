/**
 * @file    task.h
 * @brief   Abstract task interface and base implementations for RTOS-based
 *          producer-consumer patterns.
 *
 * ARCHITECTURE:
 *   This file defines the foundation for the task-based data flow architecture
 *   described in docs/firmware_architecture.md.
 *
 * DESIGN PATTERN:
 *   - BaseTask          : Abstract base for all RTOS tasks
 *   - IProducer<T>      : Interface for tasks that produce data
 *   - IConsumer<T>      : Interface for tasks that consume data
 *   - ProducerTask<T>   : Base implementation for pure producer tasks
 *   - ConsumerTask<T>   : Base implementation for pure consumer tasks
 *   - ConsumerProducerTask<TIn,TOut> : Base for hybrid tasks (transform pipeline)
 *
 * USAGE:
 *   Concrete tasks inherit from ProducerTask, ConsumerTask, or
 *   ConsumerProducerTask and implement the pure virtual run() method.
 *
 * THREAD SAFETY:
 *   - All FIFOs are thread-safe (IQueue<T> uses rtos::Mutex internally)
 *   - Subscription lists are NOT thread-safe (subscribe during setup only)
 *   - State transitions (start/stop) are NOT thread-safe (call from main thread)
 *
 * MEMORY MODEL:
 *   - Tasks own their thread objects
 *   - Consumers own their incoming FIFO
 *   - Producers hold non-owning pointers to subscriber queues
 */

#pragma once

#include <vector>
#include "mbed.h"
#include "fifo_queue.h"
#include "config.h"

#ifdef DEBUG_ENABLE
// mbed_boot.h: mbed_heap_start (uint8_t*) and mbed_heap_size (uint32_t)
// always present — no special compile flags required.
#include "mbed_boot.h"
extern "C" void* _sbrk(int incr);  // heap-break query
// Shared mutex: every task must hold this while calling Serial.print() to
// prevent character-level interleaving on the USB CDC driver (not thread-safe).
extern rtos::Mutex gSerialMutex;

// Best-effort debug output: never block RTOS tasks on a congested USB CDC link.
// Returns false when the mutex is busy or the TX buffer lacks room; callers
// should treat that as a dropped log line, not as an error.
bool debugTryPrint(const char* msg);
#endif


// =============================================================================
// BaseTask - Abstract base class for all RTOS tasks
// =============================================================================

/**
 * @brief Abstract base class providing common RTOS thread management.
 *
 * All concrete tasks derive from this and implement the pure virtual run()
 * method, which is executed in the spawned thread.
 *
 * LIFECYCLE:
 *   1. Construct task (allocates thread object but does NOT start it)
 *   2. Call start() to spawn the thread and begin execution
 *   3. Call stop() to signal termination and join the thread
 *
 * IMPORTANT:
 *   - start() must be called AFTER all subscriptions are configured
 *   - stop() blocks until the thread terminates
 *   - Do NOT call start() or stop() from ISR context
 */
class BaseTask : public INotifiable {
protected:
    rtos::Thread*  _thread;        ///< Owned thread object (heap-allocated)
    bool           _stopRequested; ///< Thread termination flag (volatile)
    bool           _isRunning;     ///< True if thread has been started

    /**
     * @brief Pure virtual run method - main thread loop.
     *
     * Concrete tasks implement their core logic here.
     * The method should check _stopRequested periodically and return cleanly
     * when stop() is called.
     *
     * Example:
     * @code
     * void MyTask::run() {
     *     while (!_stopRequested) {
     *         // Do work...
     *         rtos::ThisThread::sleep_for(10ms);
     *     }
     * }
     * @endcode
     */
    virtual void run() = 0;

    /**
     * @brief Static thread entry point (trampoline to instance method).
     * @param arg  Pointer to the BaseTask instance (this).
     */
    static void threadEntry(void* arg);

    /**
     * @brief Block until notified or timeout expires.
     *
     * Concrete run() loops call this in their idle branch instead of a plain
     * sleep_for().  When a producer calls notify() the semaphore is released
     * and this returns immediately, allowing the task to re-check its queues
     * without waiting for the full timeout.
     *
     * @param timeout_ms  Maximum time to wait in milliseconds (default: 1).
     */
    void sleepUntilNotified(uint32_t timeout_ms = 1);

    /**
     * @brief Print a one-line health report to Serial (DEBUG_ENABLE only).
     *
     * Reports for the calling thread:
     *   - Allocated stack size (from osThreadGetStackSize — always valid).
     *   - Peak-free stack: N/A — osThreadGetStackSpace() always returns 0 with
     *     the pre-built Arduino Mbed core (OS_STACK_WATERMARK disabled in libmbed.a).
     *   - Heap claimed / total: computed from _sbrk(0) and mbed_heap_start/size
     *     (mbed_boot.h). This is a system-wide figure, not per-task.
     *
     * Call once per report interval inside the concrete run() loop:
     * @code
     *   static uint32_t lastReport = 0;
     *   if (millis() - lastReport >= 5000) {
     *       lastReport = millis();
     *       reportHealth("STREAMUX");
     *   }
     * @endcode
     *
     * Compiled out entirely when DEBUG_ENABLE is not defined.
     *
     * @param taskName  Short label printed in the output line (e.g. "UART", "EEG").
     */
    void reportHealth(const char* taskName);

public:
    /**
     * @brief Construct a BaseTask with the specified priority and stack size.
     *
     * Does NOT start the thread - call start() after construction.
     *
     * @param priority   RTOS priority (e.g., osPriorityRealtime, osPriorityNormal)
     * @param stackSize  Stack size in bytes (e.g., 2048)
     */
    BaseTask(osPriority priority, uint32_t stackSize);

    /**
     * @brief Virtual destructor - stops the thread if still running.
     *
     * Calls stop() if the thread is active, then releases resources.
     */
    virtual ~BaseTask();

    /**
     * @brief Start the task thread.
     *
     * Spawns the thread and begins execution of run().
     * Do NOT call more than once without an intervening stop().
     *
     * IMPORTANT: Call this AFTER all subscriptions are configured.
     */
    void start();

    /**
     * @brief Stop the task thread.
     *
     * Sets the _stopRequested flag and blocks until the thread terminates.
     * Safe to call multiple times (idempotent).
     */
    void stop();

    /**
     * @brief Check if the task thread is currently running.
     * @return true if start() has been called and stop() has not completed.
     */
    bool isRunning() const { return _isRunning; }

    /**
     * @brief Wake the task immediately.
     *
     * Implements INotifiable. Called by FifoQueue::push() (via INotifiable*)
     * when data is enqueued above TASK_WAKE_THRESHOLD_PCT, unblocking the
     * sleepUntilNotified() wait in the run() loop.
     *
     * Uses a binary semaphore: if the task is already awake the release()
     * silently no-ops (count capped at 1).  Safe to call from any thread.
     */
    void notify() override;

private:
    osPriority       _priority;   ///< Thread priority (stored for thread creation)
    uint32_t         _stackSize;  ///< Stack size in bytes
    rtos::Semaphore  _wakeSem;    ///< Binary semaphore used by sleepUntilNotified()
};


// =============================================================================
// IProducer<T> - Interface for tasks that produce data
// =============================================================================

/**
 * @brief Interface for tasks that produce data items of type T.
 *
 * Producers maintain a list of subscriber queues and distribute data to all
 * subscribers when new data is available (fan-out pattern).
 *
 * @tparam T  Data item type (e.g., ADS1299_4_Sample)
 */
template<typename T>
class IProducer {
public:
    virtual ~IProducer() = default;

    /**
     * @brief Subscribe a consumer queue to receive data from this producer.
     *
     * The subscriber queue is added to the internal subscription list.
     * When the producer generates new data, it will be pushed to all
     * subscribed queues.
     *
     * THREAD SAFETY: NOT thread-safe. Call during setup() only, before start().
     *
     * @param queue  Non-owning pointer to the consumer's incoming FIFO.
     *               The queue must remain valid for the lifetime of the producer.
     */
    virtual void subscribe(IQueue<T>* queue) = 0;
};


// =============================================================================
// IConsumer<T> - Interface for tasks that consume data
// =============================================================================

/**
 * @brief Interface for tasks that consume data items of type T.
 *
 * Consumers own an incoming FIFO queue and expose it via getQueue() so that
 * producers can subscribe to it.
 *
 * @tparam T  Data item type (e.g., ADS1299_4_Sample)
 */
template<typename T>
class IConsumer {
public:
    virtual ~IConsumer() = default;

    /**
     * @brief Get a non-owning pointer to this consumer's incoming queue.
     *
     * Producers call this method to obtain the queue reference for subscription.
     *
     * @return Non-owning pointer to the consumer's incoming FIFO.
     */
    virtual IQueue<T>* getQueue() = 0;
};


// =============================================================================
// ProducerTask<T> - Base implementation for pure producer tasks
// =============================================================================

/**
 * @brief Base class for tasks that produce data but do not consume.
 *
 * Concrete producers inherit from this class and implement run().
 * When new data is produced, call distribute() to fan out to all subscribers.
 *
 * EXAMPLE:
 * @code
 * class AcquisitionTask : public ProducerTask<ADS1299_4_Sample> {
 * protected:
 *     void run() override {
 *         while (!_stopRequested) {
 *             _semaphore.acquire();  // Wait for DRDY ISR
 *             ADS1299_4_Sample sample = _ads.readSample();
 *             distribute(sample);    // Fan out to subscribers
 *         }
 *     }
 * public:
 *     AcquisitionTask() : ProducerTask(osPriorityRealtime, 2048) {}
 * };
 * @endcode
 *
 * @tparam T  Output data item type
 */
template<typename T>
class ProducerTask : public BaseTask, public IProducer<T> {
protected:
    std::vector<IQueue<T>*>  _subscribers;  ///< List of subscriber queues

    /**
     * @brief Distribute a data item to all subscribed consumers.
     *
     * Pushes the item to every queue in _subscribers.
     * If a queue is full, the oldest item is evicted (drop-oldest policy).
     *
     * @param item  Data item to distribute (copied to each queue)
     */
    void distribute(const T& item);

public:
    /**
     * @brief Construct a ProducerTask with specified priority and stack size.
     * @param priority   RTOS priority
     * @param stackSize  Stack size in bytes
     */
    ProducerTask(osPriority priority, uint32_t stackSize);

    /**
     * @brief Subscribe a consumer queue to receive data from this producer.
     * @param queue  Non-owning pointer to consumer's incoming FIFO
     */
    void subscribe(IQueue<T>* queue) override;
};


// =============================================================================
// ConsumerTask<T> - Base implementation for pure consumer tasks
// =============================================================================

/**
 * @brief Base class for tasks that consume data but do not produce.
 *
 * Concrete consumers inherit from this class and implement run().
 * The incoming queue is accessed via _incomingQueue.
 *
 * EXAMPLE:
 * @code
 * class StreamingTask : public ConsumerTask<ADS1299_4_Sample> {
 * protected:
 *     void run() override {
 *         ADS1299_4_Sample sample;
 *         while (!_stopRequested) {
 *             if (_incomingQueue->pop(sample)) {
 *                 formatAndSendToSerial(sample);
 *             } else {
 *                 rtos::ThisThread::sleep_for(1ms);
 *             }
 *         }
 *     }
 * public:
 *     StreamingTask() : ConsumerTask(osPriorityNormal, 2048, 64) {}
 * };
 * @endcode
 *
 * @tparam T  Input data item type
 */
template<typename T>
class ConsumerTask : public BaseTask, public IConsumer<T> {
protected:
    FifoQueue<T, FIFO_DEPTH_STREAMING>*  _incomingQueue;  ///< Owned incoming FIFO

public:
    /**
     * @brief Construct a ConsumerTask with specified priority, stack, and queue depth.
     *
     * NOTE: The queue depth is a template parameter of FifoQueue, so this
     *       constructor uses FIFO_DEPTH_STREAMING from config.h.
     *       For custom depths, use a template parameter or override in subclass.
     *
     * @param priority   RTOS priority
     * @param stackSize  Stack size in bytes
     */
    ConsumerTask(osPriority priority, uint32_t stackSize);

    /**
     * @brief Destructor - releases the owned incoming queue.
     */
    virtual ~ConsumerTask();

    /**
     * @brief Get a non-owning pointer to this consumer's incoming queue.
     * @return Pointer to the incoming FIFO (producers subscribe to this)
     */
    IQueue<T>* getQueue() override;
};


// =============================================================================
// ConsumerProducerTask<TIn, TOut> - Base for hybrid tasks (transform pipeline)
// =============================================================================

/**
 * @brief Base class for tasks that both consume and produce data (pipeline stage).
 *
 * Hybrid tasks read from an incoming queue, process the data, and distribute
 * results to downstream consumers. This implements a data transformation pipeline.
 *
 * EXAMPLE:
 * @code
 * class MLProcessorTask : public ConsumerProducerTask<ADS1299_4_Sample, MLOutput> {
 * protected:
 *     void run() override {
 *         ADS1299_4_Sample sample;
 *         while (!_stopRequested) {
 *             if (_incomingQueue->pop(sample)) {
 *                 MLOutput result = processWithML(sample);
 *                 distribute(result);  // Send to downstream consumers
 *             } else {
 *                 rtos::ThisThread::sleep_for(1ms);
 *             }
 *         }
 *     }
 * public:
 *     MLProcessorTask() : ConsumerProducerTask(osPriorityNormal, 4096) {}
 * };
 * @endcode
 *
 * @tparam TIn   Input data item type (consumed from upstream)
 * @tparam TOut  Output data item type (produced to downstream)
 */
template<typename TIn, typename TOut>
class ConsumerProducerTask : public BaseTask, public IConsumer<TIn>, public IProducer<TOut> {
protected:
    FifoQueue<TIn, FIFO_DEPTH_STREAMING>*  _incomingQueue;  ///< Owned incoming FIFO
    std::vector<IQueue<TOut>*>              _subscribers;    ///< Downstream queues

    /**
     * @brief Distribute a data item to all subscribed downstream consumers.
     * @param item  Transformed output item
     */
    void distribute(const TOut& item);

public:
    /**
     * @brief Construct a ConsumerProducerTask with specified priority and stack size.
     * @param priority   RTOS priority
     * @param stackSize  Stack size in bytes
     */
    ConsumerProducerTask(osPriority priority, uint32_t stackSize);

    /**
     * @brief Destructor - releases the owned incoming queue.
     */
    virtual ~ConsumerProducerTask();

    /**
     * @brief Get a non-owning pointer to this task's incoming queue (IConsumer interface).
     * @return Pointer to the incoming FIFO (upstream producers subscribe to this)
     */
    IQueue<TIn>* getQueue() override;

    /**
     * @brief Subscribe a downstream consumer queue (IProducer interface).
     * @param queue  Non-owning pointer to downstream consumer's incoming FIFO
     */
    void subscribe(IQueue<TOut>* queue) override;
};


// =============================================================================
// Template Implementations
// =============================================================================

// --- ProducerTask<T> Implementation ------------------------------------------

template<typename T>
ProducerTask<T>::ProducerTask(osPriority priority, uint32_t stackSize)
    : BaseTask(priority, stackSize)
{
    _subscribers.reserve(4);  // Pre-allocate space for typical subscriber count
}

template<typename T>
void ProducerTask<T>::subscribe(IQueue<T>* queue) {
    if (queue != nullptr) {
        _subscribers.push_back(queue);
    }
}

template<typename T>
void ProducerTask<T>::distribute(const T& item) {
    for (auto* queue : _subscribers) {
        queue->push(item);  // Mutex-guarded; never waits for space (drop-oldest on full)
    }
}


// --- ConsumerTask<T> Implementation ------------------------------------------

template<typename T>
ConsumerTask<T>::ConsumerTask(osPriority priority, uint32_t stackSize)
    : BaseTask(priority, stackSize)
{
    _incomingQueue = new FifoQueue<T, FIFO_DEPTH_STREAMING>();
}

template<typename T>
ConsumerTask<T>::~ConsumerTask() {
    delete _incomingQueue;
}

template<typename T>
IQueue<T>* ConsumerTask<T>::getQueue() {
    return _incomingQueue;
}


// --- ConsumerProducerTask<TIn, TOut> Implementation -------------------------

template<typename TIn, typename TOut>
ConsumerProducerTask<TIn, TOut>::ConsumerProducerTask(osPriority priority, uint32_t stackSize)
    : BaseTask(priority, stackSize)
{
    _incomingQueue = new FifoQueue<TIn, FIFO_DEPTH_STREAMING>();
    _subscribers.reserve(4);
}

template<typename TIn, typename TOut>
ConsumerProducerTask<TIn, TOut>::~ConsumerProducerTask() {
    delete _incomingQueue;
}

template<typename TIn, typename TOut>
IQueue<TIn>* ConsumerProducerTask<TIn, TOut>::getQueue() {
    return _incomingQueue;
}

template<typename TIn, typename TOut>
void ConsumerProducerTask<TIn, TOut>::subscribe(IQueue<TOut>* queue) {
    if (queue != nullptr) {
        _subscribers.push_back(queue);
    }
}

template<typename TIn, typename TOut>
void ConsumerProducerTask<TIn, TOut>::distribute(const TOut& item) {
    for (auto* queue : _subscribers) {
        queue->push(item);  // Mutex-guarded; never waits for space (drop-oldest on full)
    }
}
