#include "mpmc_queue.hpp"
#include "mutex_queue.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

template <template<typename, std::size_t> class QueueType>
void run_stress(int producerCount, int consumerCount) {
    constexpr std::size_t CAPACITY = 1024;
    constexpr int ITEMS_PER_PRODUCER = 50000;

    const int totalItems=producerCount*ITEMS_PER_PRODUCER;
    QueueType<int, CAPACITY> queue;
    std::vector<std::atomic<unsigned int>> seen(totalItems);
    std::atomic<int> consumedCount{0};

    for(auto& entry: seen)
        entry.store(0, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    threads.reserve(producerCount+consumerCount);

    for(int producerId=0;producerId<producerCount;producerId++) {
        threads.emplace_back([&, producerId]() {
            int start=producerId*ITEMS_PER_PRODUCER;
            int end=start+ITEMS_PER_PRODUCER;

            for(int value=start;value<end;value++) {
                while(!queue.enqueue(value))
                    std::this_thread::yield();
            }
        });
    }

    for(int consumerId=0;consumerId<consumerCount;consumerId++) {
        threads.emplace_back([&]() {
            int value=0;
            while(consumedCount.load(std::memory_order_relaxed)<totalItems) {
                if(queue.dequeue(value)) {
                    assert(value>=0 && value<totalItems);
                    seen[value].fetch_add(1, std::memory_order_relaxed);
                    consumedCount.fetch_add(1, std::memory_order_relaxed);
                }
                else
                    std::this_thread::yield();
            }
        });
    }

    for(auto& thread: threads)
        thread.join();

    assert(consumedCount.load()==totalItems);
    for(const auto& entry: seen)
        assert(entry.load()==1);
}

template <template<typename, std::size_t> class QueueType>
void run_all() {
    run_stress<QueueType>(1, 1);
    run_stress<QueueType>(4, 1);
    run_stress<QueueType>(1, 4);
    run_stress<QueueType>(4, 4);
}

int main() {
    std::cout << "testing padded MPMC queue...\n";
    run_all<MpmcQueue>();

    std::cout << "testing unpadded MPMC queue...\n";
    run_all<MpmcQueueUnpadded>();

    std::cout << "testing mutex queue...\n";
    run_all<MutexQueue>();

    std::cout << "stress tests passed\n";
}
