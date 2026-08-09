/**
 * @file    fifo_queue.h
 * @brief   Generic thread-safe FIFO queue (Mbed OS 6 / CMSIS-RTOS v2 / RTX5).
 *
 * Provides:
 *   - IQueue<T>             - pure abstract interface; capacity-independent.
 *   - FifoQueue<T,CAPACITY> - concrete ring-buffer with static storage.
 *
 * Algorithm properties:
 *   - Zero heap allocation  : all storage is T _buf[CAPACITY] inside the object.
 *   - Mutex-guarded         : every operation acquires rtos::Mutex (with priority
 *                             inheritance); callers may block briefly for lock
 *                             acquisition. NOT safe to call from ISR context.
 *   - Push never waits for space  : when full, the OLDEST entry is evicted so the
 *                                   producer returns immediately without waiting
 *                                   for a consumer to free a slot.
 *   - Pop/peek never wait for data: return false immediately when empty; the
 *                                   consumer is never delayed waiting for items.
 *
 * Status semantics:
 *   - EMPTY      : queue holds zero items.
 *   - NORMAL     : items present, below NEAR_FULL threshold.
 *   - NEAR_FULL  : fill level >= FIFO_NEAR_FULL_PCT % of capacity.
 *   - OVERFLOWED : at least one item dropped since last clear(). STICKY.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "mbed.h"   // rtos::Mutex  (Mbed OS 6 / CMSIS-RTOS v2 / RTX5)


// -----------------------------------------------------------------------------
// QueueStatus
// -----------------------------------------------------------------------------

/**
 * @brief Fill-level and health status of a FifoQueue instance.
 *
 * Values are ordered by severity; OVERFLOWED is highest.
 * The status() accessor returns the most severe applicable state.
 * OVERFLOWED is sticky: once set it remains until clear() is called.
 */
enum class QueueStatus : uint8_t {
    EMPTY      = 0,   ///< Queue holds no items.
    NORMAL     = 1,   ///< Queue holds items, below NEAR_FULL threshold.
    NEAR_FULL  = 2,   ///< Fill level >= FIFO_NEAR_FULL_PCT % of capacity.
    OVERFLOWED = 3    ///< At least one drop has occurred since last clear(). STICKY.
};


// -----------------------------------------------------------------------------
// FIFO_NEAR_FULL_PCT  - near-full threshold (% of capacity)
// -----------------------------------------------------------------------------

/**
 * @brief Percentage of capacity at which QueueStatus::NEAR_FULL is triggered.
 *
 * Default: 75 % (status becomes NEAR_FULL when 3/4 of slots are occupied).
 * Override by defining FIFO_NEAR_FULL_PCT before including this header or in
 * config.h.
 */
#ifndef FIFO_NEAR_FULL_PCT
#  define FIFO_NEAR_FULL_PCT  75
#endif

/**
 * @brief Minimum queue fill (%) at which a push() will call INotifiable::notify().
 *
 * 0 = notify on every push (maximum responsiveness, suitable for streaming).
 * Override in config.h or before including this header.
 */
#ifndef TASK_WAKE_THRESHOLD_PCT
#  define TASK_WAKE_THRESHOLD_PCT  0
#endif


// -----------------------------------------------------------------------------
// INotifiable  - minimal wake-up interface, implemented by BaseTask
// -----------------------------------------------------------------------------

/**
 * @brief Pure interface for objects that can be woken by a one-shot signal.
 *
 * FifoQueue holds an INotifiable* (non-owning) so it can unblock the
 * consumer task immediately after a push, without including task.h and
 * creating a circular dependency.
 *
 * Implemented by BaseTask::notify().
 */
class INotifiable {
public:
    virtual ~INotifiable() = default;

    /**
     * @brief Signal the object to wake up.
     *
     * Implemented as a binary semaphore release on BaseTask.
     * Safe to call from any thread context; silently no-ops if the
     * consumer is already awake (binary semaphore is at its max count).
     */
    virtual void notify() = 0;
};


// -----------------------------------------------------------------------------
// IQueue<T>  - pure abstract FIFO interface
// -----------------------------------------------------------------------------

/**
 * @brief Capacity-independent pure abstract interface for a typed FIFO queue.
 *
 * Callers should accept IQueue<T>& so that the capacity parameter (N) of
 * the concrete FifoQueue<T,N> stays hidden from call sites.
 *
 * @tparam T  Item type. Must be copy-constructible and copy-assignable.
 *            Plain structs (POD) are ideal.
 */
template<typename T>
class IQueue {
public:
    virtual ~IQueue() = default;

    /**
     * @brief Push: inserts @p item; evicts the oldest entry if the queue is full.
     *
     * Acquires the mutex; may block briefly if another thread holds the lock.
     * Never waits for queue space: if full, the oldest item is dropped and
     * overflow_count / dropped_count are updated before the new item is written.
     * NOT safe to call from ISR context (rtos::Mutex::lock() is not IRQ-safe).
     *
     * @param item  Item to insert (copied by value).
     * @return true  Item inserted; no eviction occurred.
     * @return false Item inserted; the oldest item was evicted (overflow).
     */
    virtual bool push(const T& item) = 0;

    /**
     * @brief Pop: removes and returns the oldest item.
     *
     * Acquires the mutex; may block briefly if another thread holds the lock.
     * Never waits for data: returns false immediately when the queue is empty.
     *
     * @param[out] item  Filled with the oldest item when the queue is non-empty.
     * @return true   Item retrieved successfully; @p item is valid.
     * @return false  Queue was empty; @p item is unchanged.
     */
    virtual bool pop(T& item) = 0;

    /**
     * @brief Non-destructive peek at the oldest item. Does not remove it.
     *
     * @param[out] item  Filled with the oldest item when the queue is non-empty.
     * @return true   Queue is non-empty; @p item holds the oldest entry.
     * @return false  Queue is empty; @p item is unchanged.
     */
    virtual bool peek(T& item) const = 0;

    /**
     * @brief Reset queue to empty. Removes all items and zeros all diagnostics.
     *
     * Resets: head, tail, count, overflow_count, dropped_count, status.
     * Safe to call from any thread context.
     */
    virtual void clear() = 0;

    /** @brief Number of items currently in the queue. Thread-safe. */
    virtual size_t size() const = 0;

    /**
     * @brief Maximum number of items the queue can hold (compile-time constant).
     *
     * Does not change after construction; no mutex needed.
     */
    virtual size_t capacity() const = 0;

    /**
     * @brief True iff the queue holds no items.
     *
     * Default implementation derives from size(). Subclasses may override
     * for a more direct (single-field) read.
     */
    virtual bool isEmpty() const { return size() == 0; }

    /**
     * @brief True iff the queue is at maximum capacity.
     *
     * Default implementation derives from size() and capacity(). Subclasses
     * may override for a more direct read.
     */
    virtual bool isFull()  const { return size() == capacity(); }

    /**
     * @brief Total number of items evicted due to overflow since last clear().
     *
     * Increments by 1 for every item evicted (the drop-oldest policy evicts
     * exactly one item per overflowing push).
     */
    virtual uint32_t droppedCount() const = 0;

    /**
     * @brief Current fill-level and health status.
     *
     * OVERFLOWED is sticky: once any item is dropped, status() returns
     * OVERFLOWED until clear() is called, regardless of current fill level.
     * Use size()/capacity() for current fill; droppedCount() for exact losses.
     */
    virtual QueueStatus status() const = 0;

    /**
     * @brief Attach the consumer task so push() can wake it immediately.
     *
     * After a non-null owner is set, every push() that raises the fill level
     * above TASK_WAKE_THRESHOLD_PCT calls owner->notify().
     * Pass nullptr to detach. Default is a no-op so existing subclasses
     * need not change.
     *
     * @param owner  Non-owning pointer to the consuming task (implements
     *               INotifiable).  Must remain valid for the lifetime of
     *               the queue.
     */
    virtual void setOwner(INotifiable* owner) {}
};


// -----------------------------------------------------------------------------
// FifoQueue<T, CAPACITY>  - concrete ring-buffer implementation
// -----------------------------------------------------------------------------

/**
 * @brief Concrete thread-safe FIFO queue with static storage.
 *
 * All storage is T _buf[CAPACITY]; no heap allocation after construction.
 * Thread safety uses rtos::Mutex, which supports priority inheritance.
 * Objects with static or global lifetime are safe: Mbed OS initialises the
 * RTOS kernel before global C++ constructors run.
 * Copy construction and assignment are deleted (Mutex is not copyable);
 * pass instances by reference or pointer only.
 *
 * @tparam T        Item type. Must be copy-constructible and copy-assignable.
 * @tparam CAPACITY Maximum items. Must be >= 2.
 */
template<typename T, size_t CAPACITY>
class FifoQueue : public IQueue<T> {

    static_assert(CAPACITY >= 2,
        "FifoQueue: CAPACITY must be >= 2 (a queue of 0 or 1 is degenerate).");

public:

    // -- Construction / destruction ------------------------------------------

    FifoQueue()
        : _head(0), _tail(0), _count(0),
          _dropped_count(0),
          _status(QueueStatus::EMPTY)
    {}

    // Mutex is not copyable; prevent accidental copy of the whole queue.
    FifoQueue(const FifoQueue&)            = delete;
    FifoQueue& operator=(const FifoQueue&) = delete;


    // -- IQueue<T> implementation --------------------------------------------

    /**
     * @brief Push with drop-oldest overflow policy (mutex-guarded).
     *
     * Algorithm (with mutex held):
     *   1. If full: advance head by 1 (evict oldest), update counters.
     *   2. Write item to tail slot, advance tail, increment count.
     *   3. Update status.
     *
     * @return true   No eviction; item inserted cleanly.
     * @return false  One old item was evicted to make room; item was still inserted.
     */
    bool push(const T& item) override {
        _mutex.lock();

        bool dropped = false;

        if (_count == CAPACITY) {
            // Queue full: evict the oldest entry to make room.
            _head = (_head + 1) % CAPACITY;
            _count--;
            _dropped_count++;
            dropped = true;
        }

        _buf[_tail] = item;
        _tail       = (_tail + 1) % CAPACITY;
        _count++;

        _updateStatus();

        // Capture fill level while mutex is still held.
        const size_t countSnapshot = _count;

        _mutex.unlock();

        // Notify owner after releasing the mutex so we never call into the
        // OS while holding _mutex.
        // Condition: fill level has reached the configured wake threshold.
        // With TASK_WAKE_THRESHOLD_PCT=0, threshold==0 and countSnapshot>=0
        // is always true, so the owner is notified on every push.
        if (_owner) {
            const size_t threshold =
                (CAPACITY * static_cast<size_t>(TASK_WAKE_THRESHOLD_PCT)) / 100u;
            if (countSnapshot >= threshold) {
                _owner->notify();
            }
        }

        return !dropped;
    }

    /**
     * @brief Pop (mutex-guarded); returns false immediately if empty.
     *
     * @return true  Item retrieved; @p item is valid.
     * @return false Queue empty; @p item unchanged.
     */
    bool pop(T& item) override {
        _mutex.lock();
        const bool ok = (_count > 0);
        if (ok) {
            item  = _buf[_head];
            _head = (_head + 1) % CAPACITY;
            _count--;
            _updateStatus();
        }
        _mutex.unlock();
        return ok;
    }

    /**
     * @brief Non-destructive peek at the head (oldest item).
     *
     * @return true  Queue non-empty; @p item contains a copy of the oldest entry.
     * @return false Queue empty; @p item unchanged.
     */
    bool peek(T& item) const override {
        _mutex.lock();
        const bool ok = (_count > 0);
        if (ok) {
            item = _buf[_head];
        }
        _mutex.unlock();
        return ok;
    }

    /** @brief Reset queue and all diagnostic counters to initial state. */
    void clear() override {
        _mutex.lock();
        _head          = 0;
        _tail          = 0;
        _count         = 0;
        _dropped_count = 0;
        _status        = QueueStatus::EMPTY;
        _mutex.unlock();
    }

    /** @brief Current item count. Thread-safe. */
    size_t size() const override {
        _mutex.lock();
        const size_t s = _count;
        _mutex.unlock();
        return s;
    }

    /** @brief Compile-time capacity. No mutex needed (immutable). */
    size_t capacity() const override {
        return CAPACITY;
    }

    uint32_t droppedCount() const override {
        _mutex.lock();
        const uint32_t c = _dropped_count;
        _mutex.unlock();
        return c;
    }

    QueueStatus status() const override {
        _mutex.lock();
        const QueueStatus s = _status;
        _mutex.unlock();
        return s;
    }

    void setOwner(INotifiable* owner) override {
        _owner = owner;
    }


private:

    // -- Ring buffer state ---------------------------------------------------

    T      _buf[CAPACITY];  ///< Static storage - no heap involvement.
    size_t _head;           ///< Index of the next item to pop (oldest slot).
    size_t _tail;           ///< Index of the next free slot to write (newest+1).
    size_t _count;          ///< Current number of valid items in the buffer.

    // -- Diagnostic counter --------------------------------------------------

    uint32_t    _dropped_count;   ///< Total items evicted due to overflow.

    // -- Status --------------------------------------------------------------

    QueueStatus _status;           ///< Current/sticky status (OVERFLOWED is sticky).

    // mutable: const accessors (size, peek, status, …) must lock without const_cast.
    mutable rtos::Mutex _mutex;

    // Optional owner task — notified on push() when fill >= TASK_WAKE_THRESHOLD_PCT.
    // Null by default; set via setOwner().
    INotifiable* _owner = nullptr;


    // -- Private helpers -----------------------------------------------------

    /**
     * @brief Recompute and cache _status from current queue state.
     *
     * MUST be called with _mutex already held.
     * Precedence (highest wins): OVERFLOWED > NEAR_FULL > NORMAL > EMPTY.
     * OVERFLOWED is sticky: once _dropped_count > 0 it is never cleared here;
     * only clear() resets it.
     */
    void _updateStatus() {
        if (_dropped_count > 0) {
            // Sticky: any historical drop keeps status at OVERFLOWED.
            _status = QueueStatus::OVERFLOWED;
        } else if (_count == 0) {
            _status = QueueStatus::EMPTY;
        } else if (_count >= (CAPACITY * static_cast<size_t>(FIFO_NEAR_FULL_PCT)) / 100u) {
            _status = QueueStatus::NEAR_FULL;
        } else {
            _status = QueueStatus::NORMAL;
        }
    }
};


