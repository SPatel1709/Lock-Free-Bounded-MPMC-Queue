#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

constexpr std::size_t CACHE_LINE_SIZE = 64;

template <typename T, std::size_t CAPACITY, bool PADDED>
class MpmcQueueBase {
    static_assert(CAPACITY >= 2, "Queue capacity must be at least 2");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "size_t atomics must be lock-free on this platform");

private:
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    std::array<Cell, CAPACITY> buffer{};

    struct UnpaddedState {
        std::atomic<std::size_t> enqueuePos{0};
        std::atomic<std::size_t> dequeuePos{0};
    };

    struct PaddedState {
        alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> enqueuePos{0};
        alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> dequeuePos{0};
    };

    using State = std::conditional_t<PADDED, PaddedState, UnpaddedState>;
    State state{};

public:
    MpmcQueueBase() {
        for(std::size_t i=0;i<CAPACITY;i++)
            buffer[i].sequence.store(i, std::memory_order_relaxed);
    }

    MpmcQueueBase(const MpmcQueueBase&) = delete;
    MpmcQueueBase& operator=(const MpmcQueueBase&) = delete;

    bool enqueue(const T& value) {
        std::size_t pos=state.enqueuePos.load(std::memory_order_relaxed);
        Cell* cell=nullptr;

        while(true) {
            cell=&buffer[pos%CAPACITY];
            std::size_t sequence=cell->sequence.load(std::memory_order_acquire);
            std::intptr_t difference=static_cast<std::intptr_t>(sequence)-
                                     static_cast<std::intptr_t>(pos);

            if(difference==0) {
                if(state.enqueuePos.compare_exchange_weak(pos, pos+1,
                        std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            }
            else if(difference<0)
                return false;
            else
                pos=state.enqueuePos.load(std::memory_order_relaxed);
        }

        cell->data=value;
        cell->sequence.store(pos+1, std::memory_order_release);
        return true;
    }

    bool dequeue(T& value) {
        std::size_t pos=state.dequeuePos.load(std::memory_order_relaxed);
        Cell* cell=nullptr;

        while(true) {
            cell=&buffer[pos%CAPACITY];
            std::size_t sequence=cell->sequence.load(std::memory_order_acquire);
            std::intptr_t difference=static_cast<std::intptr_t>(sequence)-
                                     static_cast<std::intptr_t>(pos+1);

            if(difference==0) {
                if(state.dequeuePos.compare_exchange_weak(pos, pos+1,
                        std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            }
            else if(difference<0)
                return false;
            else
                pos=state.dequeuePos.load(std::memory_order_relaxed);
        }

        value=cell->data;
        cell->sequence.store(pos+CAPACITY, std::memory_order_release);
        return true;
    }
};

template <typename T, std::size_t CAPACITY>
using MpmcQueue = MpmcQueueBase<T, CAPACITY, true>;

template <typename T, std::size_t CAPACITY>
using MpmcQueueUnpadded = MpmcQueueBase<T, CAPACITY, false>;
