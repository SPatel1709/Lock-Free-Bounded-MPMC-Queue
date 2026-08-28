#include "mpmc_queue.hpp"
#include "mutex_queue.hpp"

#include <cassert>
#include <iostream>

template <template<typename, std::size_t> class QueueType>
void test_queue() {
    constexpr std::size_t CAPACITY = 4;
    QueueType<int, CAPACITY> queue;
    int value=-1;

    assert(!queue.dequeue(value));
    assert(queue.enqueue(10));
    assert(queue.enqueue(20));
    assert(queue.enqueue(30));
    assert(queue.enqueue(40));
    assert(!queue.enqueue(50));

    assert(queue.dequeue(value) && value==10);
    assert(queue.dequeue(value) && value==20);
    assert(queue.enqueue(50));
    assert(queue.enqueue(60));
    assert(queue.dequeue(value) && value==30);
    assert(queue.dequeue(value) && value==40);
    assert(queue.dequeue(value) && value==50);
    assert(queue.dequeue(value) && value==60);
    assert(!queue.dequeue(value));
}

int main() {
    test_queue<MpmcQueue>();
    test_queue<MpmcQueueUnpadded>();
    test_queue<MutexQueue>();
    std::cout << "correctness tests passed\n";
}
