#pragma once

#include <array>
#include <cstddef>
#include <mutex>

template <typename T, std::size_t CAPACITY>
class MutexQueue {
    static_assert(CAPACITY >= 2, "Queue capacity must be at least 2");

private:
    std::array<T, CAPACITY> buffer{};
    std::size_t head=0;
    std::size_t tail=0;
    std::size_t count=0;
    std::mutex mutexLock;

public:
    MutexQueue()=default;
    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    bool enqueue(const T& value) {
        std::lock_guard<std::mutex> lock(mutexLock);
        if(count==CAPACITY)
            return false;

        buffer[tail]=value;
        tail=(tail+1)%CAPACITY;
        count++;
        return true;
    }

    bool dequeue(T& value) {
        std::lock_guard<std::mutex> lock(mutexLock);
        if(count==0)
            return false;

        value=buffer[head];
        head=(head+1)%CAPACITY;
        count--;
        return true;
    }
};
