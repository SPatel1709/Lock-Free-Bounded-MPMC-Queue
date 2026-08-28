# Lock-Free Bounded MPMC Queue

C++ project comparing a bounded atomic MPMC ring queue against a bounded mutex-protected ring queue, to understand the behaviour of cache-lines due to various effects like false sharing, cache coherency, cache-line bouncing

## Structure

```text
lock-free-mpmc/
├── include/
│   ├── mpmc_queue.hpp
│   └── mutex_queue.hpp
├── tests/
│   ├── correctness.cpp
│   └── stress_test.cpp
├── benchmark/
│   └── benchmark.cpp
├── results/
│   ├── throughput.csv
│   └── plots/
├── CMakeLists.txt
└── README.md
```

## Queue design

`mpmcQueue_t<T, CAPACITY>` is a fixed-size ring buffer. Every slot stores the value and an atomic sequence number. Producers reserve enqueue positions with CAS; consumers reserve dequeue positions the same way. The sequence number tells whether a slot belongs to the current producer/consumer generation.

`mutexQueue_t<T, CAPACITY>` uses the same bounded ring-buffer behavior behind one `std::mutex` and serves as the baseline.

## Memory ordering

The enqueue/dequeue position counters are only used to reserve positions, so their loads and CAS operations use `memory_order_relaxed`.

A producer writes `cell->data` and then publishes the slot with a release store to `cell->sequence`. A consumer observes that sequence with an acquire load before reading the data. This creates the required producer-to-consumer happens-before relationship.

After reading a slot, the consumer releases it with another release store to the sequence. A later producer uses an acquire load of that sequence before reusing the slot.

## Correctness

The basic test verifies empty/full behavior, wraparound and FIFO behavior. The stress test runs:

- 1 producer / 1 consumer
- 4 producers / 1 consumer
- 1 producer / 4 consumers
- 4 producers / 4 consumers

Each producer emits a disjoint integer range. After the run, every generated value must have been consumed exactly once.

## Progress note

The queue uses only atomic operations and contains no mutex or blocking primitive. However, this particular per-slot sequence-number algorithm should not be claimed to be strictly lock-free under the formal progress definition: a thread can reserve a slot and be descheduled before publishing it, delaying progress at that queue position. It is non-blocking in implementation style, but not wait-free and not formally lock-free.

## Benchmark

The benchmark compares both queues for:

- 1P / 1C
- 2P / 2C
- 4P / 4C
- 8P / 8C
- 1P / 4C
- 4P / 1C

and capacities 64, 256, 1024 and 4096.

Each scenario gets one warmup run. The default is then five measured runs of 1,000,000 transferred items. One enqueue plus one dequeue counts as two operations. Raw runs are written to CSV; the console prints the median throughput.

The benchmark intentionally uses the same try-enqueue/try-dequeue API for both queues. Failed operations yield and retry.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run benchmark

From the project root:

```bash
./build/benchmark_runner
```

For a shorter run:

```bash
./build/benchmark_runner --items 200000 --runs 3
```

The generated CSV is `results/throughput.csv`.

## Interpreting results

Do not assume the atomic queue always wins. Throughput depends on core count, oversubscription, queue capacity, scheduling and contention. In particular, a benchmark with more runnable threads than available cores can strongly affect the result.

## Current scope

This base version does not add backoff strategies, CPU affinity, hazard pointers, epoch reclamation, an unbounded queue, or a separate false-sharing experiment. The enqueue/dequeue position counters are already cache-line separated so the main implementation avoids obvious state false sharing.
