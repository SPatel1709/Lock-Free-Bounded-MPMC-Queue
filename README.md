# Bounded MPMC Concurrent Queue

A compact C++20 learning project for studying bounded multi-producer/multi-consumer queues, C++ atomics, memory ordering, cache-line effects, contention, and thread placement.

The project compares three bounded queues:

- `MpmcQueue<T, CAPACITY>` — CAS-based MPMC ring buffer with per-slot sequence counters and cache-line-separated hot counters.
- `MpmcQueueUnpadded<T, CAPACITY>` — the same MPMC algorithm, but with enqueue/dequeue counters adjacent in memory.
- `MutexQueue<T, CAPACITY>` — a bounded ring buffer protected by `std::mutex`.

The padded and unpadded queues intentionally use the same queue algorithm. Their layout difference makes false sharing measurable without changing the synchronization protocol.

---


## 1. Queue design

Each MPMC cell contains:

```text
data
atomic sequence
```

The queue also maintains monotonically increasing logical enqueue and dequeue positions.

A producer:

1. reads the current logical enqueue position,
2. checks whether the corresponding physical cell belongs to the required generation,
3. claims the logical position with CAS,
4. writes the payload,
5. publishes the cell with a release store to its sequence counter.

A consumer performs the corresponding operation on the dequeue side and returns the cell to the next producer generation after reading it.

### Why sequence counters?

The ring buffer reuses physical cells. For capacity 4, physical slot 0 is used by logical positions:

```text
0, 4, 8, 12, ...
```

A boolean `free/full` flag cannot identify which reuse generation a thread is observing. The sequence counter carries both slot state and generation information.

For slot 0, a typical lifecycle is:

```text
0 -> 1 -> 4 -> 5 -> 8 -> 9 -> ...
```

### Memory ordering

The enqueue/dequeue position atomics are used for position ownership, so their loads/CAS operations use relaxed ordering.

The per-cell sequence counter performs the actual payload hand-off:

```text
producer writes data
        |
release store to sequence
        |
        | synchronizes-with
        v
consumer acquire-loads sequence
        |
consumer reads data
```

The same acquire/release pattern is used when the consumer releases the cell for reuse by a future producer.

This allows `data` itself to remain non-atomic while still being safely handed between owners.

---

## 2. Cache-line / false-sharing experiment

In the unpadded queue, the producer and consumer position counters can occupy the same cache line:

```text
enqueuePos | dequeuePos
```

Producers repeatedly write `enqueuePos` while consumers repeatedly write `dequeuePos`. Even though they are independent variables, placing them on the same cache line can cause unnecessary cache-coherence traffic.

The padded implementation places these hot counters on separate cache lines.

Padding does **not** remove true contention:

- producers still contend with other producers on `enqueuePos`,
- consumers still contend with other consumers on `dequeuePos`.

---

## 3. Correctness validation

The correctness tests cover:

- empty queue behavior,
- full queue behavior,
- FIFO behavior,
- ring-buffer wraparound.

Stress tests cover:

- 1 producer / 1 consumer,
- 4 producers / 1 consumer,
- 1 producer / 4 consumers,
- 4 producers / 4 consumers.

Each producer generates a disjoint integer range. After the run, the test verifies that every produced value appears exactly once in the consumed output.

The stress test was also run under ThreadSanitizer during development with no reported data race.

---

## 4. Benchmark methodology

Queue variants:

```text
mpmc_padded
mpmc_unpadded
mutex
```

Workloads:

```text
1P / 1C
2P / 2C
4P / 4C
8P / 8C
1P / 4C
4P / 1C
```

Capacities:

```text
64
256
1024
4096
```

For each queue/capacity/workload combination:

- one warmup is performed,
- 1,000,000 elements are transferred,
- five measured throughput runs are recorded,
- throughput counts enqueue + dequeue operations,
- a separate sampled latency pass reports p50, p95 and p99 latency.

There are 24 workload/capacity configurations for each queue and 360 throughput rows for each placement mode:

```text
3 queues x 4 capacities x 6 workloads x 5 measured runs = 360
```

Latency is measured separately so timestamp calls do not distort the throughput runs. Roughly every 128th enqueue/dequeue is sampled.

---

## 5. Test machine

Latest complete experiment:

```text
CPU:          Intel Core i7-1360P
Logical CPUs: 16
CPU cores:    12
Sockets:      1
NUMA nodes:   1
L1d:          448 KiB total, 12 instances
L2:           9 MiB total, 6 instances
L3:           18 MiB
Architecture: x86-64
```

The processor is a hybrid Intel design. Linux `perf` exposes separate `cpu_core` and `cpu_atom` PMU domains, so logical CPU numbering should not be interpreted as a uniform set of identical physical cores.

---

# Results

Unless otherwise stated, throughput numbers below use the median of the five runs for each capacity and then summarize across capacities.

## 6. Scheduler-controlled throughput

Median throughput across capacities, in million operations/second:

| Workload | Padded MPMC | Unpadded MPMC | Mutex |
|---|---:|---:|---:|
| 1P / 1C | 94.00 | 38.98 | 8.75 |
| 2P / 2C | 17.66 | 13.85 | 11.81 |
| 4P / 4C | 13.25 | 10.30 | 7.32 |
| 8P / 8C | 10.17 | 5.92 | 5.18 |
| 1P / 4C | 15.03 | 13.18 | 4.53 |
| 4P / 1C | 16.80 | 15.10 | 4.33 |

Across all 24 scheduler-controlled workload/capacity configurations:

- padded MPMC achieved a **2.82x median throughput speedup over the mutex queue**,
- padded MPMC achieved a **1.34x median throughput speedup over the unpadded MPMC queue**.

### Observation: contention limits scaling

Throughput falls substantially as the number of simultaneous producers and consumers increases.

For the padded queue:

```text
1P/1C : 94.00 Mops/s
4P/4C : 13.25 Mops/s
8P/8C : 10.17 Mops/s
```

More threads do not imply proportionally greater throughput because producers contend on the enqueue-position cache line and consumers contend on the dequeue-position cache line. CAS retries and cache-line ownership transfers become increasingly important.

The high 1P/1C number should therefore not be interpreted as the expected throughput under contention; it is primarily the low-contention case.

---

## 7. False-sharing result

The padding experiment becomes especially visible under heavy balanced contention.

At 8P / 8C:

```text
padded   : 10.17 Mops/s
unpadded :  5.92 Mops/s
```

The padded queue was about **1.73x faster** than the unpadded queue across the tested capacities for this workload.

This supports the expected false-sharing explanation: keeping producer and consumer hot counters on separate cache lines reduces unnecessary producer-consumer coherence traffic.

Padding is not universally beneficial in every individual run or asymmetric workload. It removes one source of false sharing; it does not remove true contention or scheduler/topology effects.

---

## 8. Tail latency

Scheduler-controlled median p99 latency across capacities:

| Workload | Padded MPMC | Unpadded MPMC | Mutex |
|---|---:|---:|---:|
| 1P / 1C | 0.076 us | 0.135 us | 0.759 us |
| 2P / 2C | 1.225 us | 1.514 us | 5.363 us |
| 4P / 4C | 2.828 us | 4.311 us | 13.677 us |
| 8P / 8C | 11.217 us | 20.582 us | 30.333 us |
| 1P / 4C | 1.721 us | 2.168 us | 13.247 us |
| 4P / 1C | 3.350 us | 2.510 us | 15.569 us |

Across all 24 scheduler-controlled configurations, padded MPMC had roughly **5.22x lower median p99 latency than the mutex baseline**.

At 4P / 4C specifically:

- padded: **2.83 us**
- unpadded: **4.31 us**
- mutex: **13.68 us**

So padding reduced p99 by about **34% versus the unpadded queue**, while the padded queue reduced p99 by about **79% versus the mutex baseline**.

At 8P / 8C the difference was also clear:

```text
padded   : 11.22 us
unpadded : 20.58 us
mutex    : 30.33 us
```

### Observation: tail latency grows faster than median behavior

Heavy contention increases waiting/retry time, so p99 grows much more sharply than the low-contention latency. This is why throughput alone is not enough to describe concurrent-queue behavior.

The 4P / 1C result is also a useful counterexample: the unpadded queue had lower p99 than the padded queue in this asymmetric workload. Padding removes false sharing but does not guarantee a win for every scheduler placement and workload.

---

# CPU Affinity Experiment

## 9. Placement modes

Two placement modes were compared.

### Scheduler controlled

Linux chooses where each worker runs and may migrate workers.

### Pinned

`--pin` maps worker `i` to:

```text
logical CPU = i % hardware_concurrency
```

This is intentionally a simple affinity policy. It is **not topology-aware**: it does not explicitly separate SMT siblings, P-cores and E-cores.

---

## 10. Padded queue: scheduler vs pinned

Median throughput across capacities:

| Workload | Scheduler | Pinned | Pinned / Scheduler |
|---|---:|---:|---:|
| 1P / 1C | 94.00 Mops/s | 101.89 Mops/s | 1.08x |
| 2P / 2C | 17.66 Mops/s | 44.19 Mops/s | 2.50x |
| 4P / 4C | 13.25 Mops/s | 12.39 Mops/s | 0.94x |
| 8P / 8C | 10.17 Mops/s | 7.56 Mops/s | 0.74x |
| 1P / 4C | 15.03 Mops/s | 17.80 Mops/s | 1.18x |
| 4P / 1C | 16.80 Mops/s | 23.68 Mops/s | 1.41x |

Across all queue variants, capacities and workloads, pinning improved median throughput in **48 of 72** exact configurations. The median pinned/scheduler throughput ratio was about **1.20x**.

However, the saturated 8P / 8C workload became slower: the median ratio across queue variants and capacities was about **0.73x**.

### Observation: affinity is not automatically an optimization

The result is more useful than a simple "pinning is faster" conclusion.

Affinity can:

- reduce migration,
- preserve cache locality,
- make thread placement more deterministic,

but a poor fixed placement can also:

- place competing workers on SMT siblings,
- force work onto different classes of hybrid cores,
- prevent the scheduler from moving a thread to a better CPU,
- interact badly with a saturated workload.

On this i7-1360P, simple logical-CPU pinning helped several moderate workloads but hurt 8P / 8C. A topology-aware policy would be required before treating affinity as a general optimization.

---

## 11. Affinity and latency

Across all 72 exact configurations:

- median p50 pinned/scheduler ratio: **0.73**
- median p95 pinned/scheduler ratio: **0.91**
- median p99 pinned/scheduler ratio: **1.04**
- only **32 of 72** configurations improved p99 latency.

So fixed affinity generally helped central latency more than tail latency in this experiment.

For the padded queue, examples were mixed:

```text
2P/2C p99: 1.225 us -> 0.777 us   (better)
4P/4C p99: 2.828 us -> 11.444 us  (worse)
8P/8C p99: 11.217 us -> 17.569 us (worse)
```

This reinforces the same conclusion: thread placement matters, but naive pinning does not guarantee lower tail latency.

---

## 12. `perf stat` observations

The complete scheduler-controlled benchmark took:

```text
96.32 s
```

The complete pinned benchmark took:

```text
87.04 s
```

or about **9.6% less wall-clock time**.

Aggregating the `cpu_core` and `cpu_atom` counters reported by `perf`, the pinned run recorded approximately:

- **11.3% fewer instructions**
- **32.6% fewer cache references**
- **28.3% fewer cache misses**
- roughly the same total cycle count

These counters are useful as directional evidence, not as a microarchitectural proof. The CPU is hybrid, the PMU events were split across `cpu_core` and `cpu_atom`, and some events were multiplexed.

The context-switch and CPU-migration counters in this run were reported as zero, so they were **not used to draw conclusions** about scheduler migration.

---

# Main observations

1. **Atomics are not free.**  
   Increasing producer/consumer count introduces CAS contention and cache-line ownership traffic, so throughput does not scale linearly with thread count.

2. **False sharing is measurable.**  
   Separating producer and consumer hot counters improved the 8P/8C throughput by about 1.73x relative to the otherwise-identical unpadded implementation.

3. **Tail latency exposes contention more clearly than throughput alone.**  
   p99 increased sharply under heavy contention; at 4P/4C the padded queue reduced p99 from 13.68 us for the mutex queue to 2.83 us.

4. **Padding is an optimization, not a guarantee.**  
   Some asymmetric/topology-sensitive cases favored the unpadded layout. Removing one source of coherence traffic does not remove all contention.

5. **CPU affinity is workload- and topology-dependent.**  
   Simple pinning improved many configurations and shortened the complete experiment, but it degraded the saturated 8P/8C case and did not consistently improve p99.

6. **A mutex baseline remains important.**  
   Without it, it would be difficult to tell whether the additional atomic/memory-ordering complexity actually produces useful performance behavior.

---

# Building and running

Requirements:

- CMake
- C++20 compiler
- pthread support
- Ninja is optional
- Linux `perf` is optional; when found by CMake it is used for the full experiment tests

Recommended Release build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Linux this runs:

```text
test_correctness
test_stress
test_full_scheduler
test_full_pinned
```

The full benchmark tests are serial so the two experiment modes do not interfere with each other.

Results are written to:

```text
build/results/
├── lscpu.txt
├── throughput_scheduler.csv
├── latency_scheduler.csv
├── perf_scheduler.txt
├── throughput_pinned.csv
├── latency_pinned.csv
└── perf_pinned.txt
```

The default experiment size is controlled in CMake:

```text
EXPERIMENT_ITEMS = 1000000
EXPERIMENT_RUNS  = 5
```

They can be changed during configuration, for example:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DEXPERIMENT_ITEMS=200000 \
  -DEXPERIMENT_RUNS=3
```

---

# CSV formats

Throughput:

```text
queue,affinity,capacity,producers,consumers,items,run,seconds,throughput_ops_per_sec
```

Latency:

```text
queue,affinity,capacity,producers,consumers,items,samples,p50_ns,p95_ns,p99_ns
```

For throughput comparisons, use the median of the five measured runs rather than selecting the best run.

---

# Project structure

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
├── CMakeLists.txt
└── README.md
```

---

# Scope

This project deliberately stays bounded and fixed-storage.

It does not attempt to add:

- an unbounded linked queue,
- hazard-pointer or epoch reclamation,
- RCU,
- custom allocators,
- wait-free guarantees.

Keeping the storage fixed avoids dynamic-memory reclamation and lets the project remain focused on:

```text
atomics
CAS
acquire/release ordering
per-slot generations
contention
cache lines
false sharing
CPU placement
benchmarking
```

The goal is to understand these mechanisms well rather than broaden the project into a general lock-free programming library.
