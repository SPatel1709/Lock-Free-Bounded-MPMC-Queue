#include "mpmc_queue.hpp"
#include "mutex_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

struct BenchmarkResult {
    double seconds=0.0;
    double throughput=0.0;
};

struct LatencyResult {
    long long p50=0;
    long long p95=0;
    long long p99=0;
    std::size_t samples=0;
};

constexpr std::size_t LATENCY_SAMPLE_INTERVAL = 128;
bool g_pinThreads=false;

void maybe_pin_thread(int workerIndex) {
    if(!g_pinThreads)
        return;

#ifdef __linux__
    unsigned int cpuCount=std::thread::hardware_concurrency();
    if(cpuCount==0)
        return;

    int cpu=workerIndex%static_cast<int>(cpuCount);
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(cpu, &cpuSet);

    if(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet)!=0) {
        static std::atomic<bool> warned{false};
        if(!warned.exchange(true))
            std::cerr << "warning: could not set CPU affinity\n";
    }
#else
    (void)workerIndex;
#endif
}

const char* get_affinity_name() {
    return g_pinThreads ? "pinned" : "scheduler";
}

long long get_percentile(std::vector<long long>& values, double percentile) {
    if(values.empty())
        return 0;

    std::sort(values.begin(), values.end());
    std::size_t index=static_cast<std::size_t>(percentile*(values.size()-1));
    return values[index];
}

template <template<typename, std::size_t> class QueueType, std::size_t CAPACITY>
BenchmarkResult run_once(int producerCount, int consumerCount, std::size_t totalItems) {
    QueueType<std::size_t, CAPACITY> queue;
    std::atomic<bool> startFlag{false};
    std::atomic<std::size_t> consumedCount{0};
    std::atomic<unsigned long long> checksum{0};
    std::vector<std::thread> threads;
    threads.reserve(producerCount+consumerCount);

    for(int producerId=0;producerId<producerCount;producerId++) {
        threads.emplace_back([&, producerId]() {
            maybe_pin_thread(producerId);

            std::size_t base=totalItems/static_cast<std::size_t>(producerCount);
            std::size_t extra=totalItems%static_cast<std::size_t>(producerCount);
            std::size_t begin=static_cast<std::size_t>(producerId)*base+
                              std::min<std::size_t>(producerId, extra);
            std::size_t itemCount=base+(static_cast<std::size_t>(producerId)<extra ? 1 : 0);

            while(!startFlag.load(std::memory_order_acquire))
                std::this_thread::yield();

            for(std::size_t i=0;i<itemCount;i++) {
                std::size_t value=begin+i;
                while(!queue.enqueue(value))
                    std::this_thread::yield();
            }
        });
    }

    for(int consumerId=0;consumerId<consumerCount;consumerId++) {
        threads.emplace_back([&, consumerId]() {
            maybe_pin_thread(producerCount+consumerId);

            unsigned long long localChecksum=0;
            std::size_t value=0;

            while(!startFlag.load(std::memory_order_acquire))
                std::this_thread::yield();

            while(consumedCount.load(std::memory_order_relaxed)<totalItems) {
                if(queue.dequeue(value)) {
                    localChecksum+=value;
                    consumedCount.fetch_add(1, std::memory_order_relaxed);
                }
                else
                    std::this_thread::yield();
            }

            checksum.fetch_add(localChecksum, std::memory_order_relaxed);
        });
    }

    auto beginTime=std::chrono::steady_clock::now();
    startFlag.store(true, std::memory_order_release);

    for(auto& thread: threads)
        thread.join();

    auto endTime=std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed=endTime-beginTime;

    const unsigned long long expectedChecksum=
        static_cast<unsigned long long>(totalItems)*(totalItems-1)/2;
    if(checksum.load()!=expectedChecksum) {
        std::cerr << "benchmark checksum failed\n";
        std::exit(1);
    }

    double operations=2.0*static_cast<double>(totalItems);
    return {elapsed.count(), operations/elapsed.count()};
}

template <template<typename, std::size_t> class QueueType, std::size_t CAPACITY>
LatencyResult run_latency_once(int producerCount, int consumerCount, std::size_t totalItems) {
    QueueType<std::size_t, CAPACITY> queue;
    std::atomic<bool> startFlag{false};
    std::atomic<std::size_t> consumedCount{0};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> samples(
        static_cast<std::size_t>(producerCount+consumerCount));
    threads.reserve(producerCount+consumerCount);

    for(int producerId=0;producerId<producerCount;producerId++) {
        threads.emplace_back([&, producerId]() {
            maybe_pin_thread(producerId);

            auto& localSamples=samples[static_cast<std::size_t>(producerId)];
            std::size_t base=totalItems/static_cast<std::size_t>(producerCount);
            std::size_t extra=totalItems%static_cast<std::size_t>(producerCount);
            std::size_t begin=static_cast<std::size_t>(producerId)*base+
                              std::min<std::size_t>(producerId, extra);
            std::size_t itemCount=base+(static_cast<std::size_t>(producerId)<extra ? 1 : 0);
            localSamples.reserve(itemCount/LATENCY_SAMPLE_INTERVAL+1);

            while(!startFlag.load(std::memory_order_acquire))
                std::this_thread::yield();

            for(std::size_t i=0;i<itemCount;i++) {
                std::size_t value=begin+i;
                bool sample=(i%LATENCY_SAMPLE_INTERVAL==0);
                auto beginTime=std::chrono::steady_clock::time_point{};

                if(sample)
                    beginTime=std::chrono::steady_clock::now();

                while(!queue.enqueue(value))
                    std::this_thread::yield();

                if(sample) {
                    auto endTime=std::chrono::steady_clock::now();
                    localSamples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        endTime-beginTime).count());
                }
            }
        });
    }

    for(int consumerId=0;consumerId<consumerCount;consumerId++) {
        int threadIndex=producerCount+consumerId;
        threads.emplace_back([&, threadIndex]() {
            maybe_pin_thread(threadIndex);

            auto& localSamples=samples[static_cast<std::size_t>(threadIndex)];
            std::size_t localCount=0;
            std::size_t value=0;
            localSamples.reserve(totalItems/static_cast<std::size_t>(consumerCount)/
                                 LATENCY_SAMPLE_INTERVAL+1);

            while(!startFlag.load(std::memory_order_acquire))
                std::this_thread::yield();

            while(consumedCount.load(std::memory_order_relaxed)<totalItems) {
                bool sample=(localCount%LATENCY_SAMPLE_INTERVAL==0);
                auto beginTime=std::chrono::steady_clock::time_point{};

                if(sample)
                    beginTime=std::chrono::steady_clock::now();

                bool success=false;
                while(consumedCount.load(std::memory_order_relaxed)<totalItems) {
                    if(queue.dequeue(value)) {
                        success=true;
                        break;
                    }
                    std::this_thread::yield();
                }

                if(!success)
                    break;

                if(sample) {
                    auto endTime=std::chrono::steady_clock::now();
                    localSamples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        endTime-beginTime).count());
                }

                localCount++;
                consumedCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    startFlag.store(true, std::memory_order_release);
    for(auto& thread: threads)
        thread.join();

    std::vector<long long> allSamples;
    for(auto& localSamples: samples)
        allSamples.insert(allSamples.end(), localSamples.begin(), localSamples.end());

    std::size_t sampleCount=allSamples.size();
    long long p50=get_percentile(allSamples, 0.50);
    long long p95=get_percentile(allSamples, 0.95);
    long long p99=get_percentile(allSamples, 0.99);
    return {p50, p95, p99, sampleCount};
}

template <template<typename, std::size_t> class QueueType, std::size_t CAPACITY>
void run_scenario(const std::string& queueName, int producerCount, int consumerCount,
                  std::size_t totalItems, int runCount, std::ofstream& throughputOutput,
                  std::ofstream& latencyOutput) {
    run_once<QueueType, CAPACITY>(producerCount, consumerCount,
                                 std::min<std::size_t>(totalItems, 50000));

    std::vector<double> throughputs;
    throughputs.reserve(runCount);

    for(int run=1;run<=runCount;run++) {
        BenchmarkResult result=run_once<QueueType, CAPACITY>(
            producerCount, consumerCount, totalItems);
        throughputs.push_back(result.throughput);

        throughputOutput << queueName << ',' << get_affinity_name() << ',' << CAPACITY << ','
                         << producerCount << ',' << consumerCount << ',' << totalItems << ',' << run << ','
                         << std::fixed << std::setprecision(6) << result.seconds << ','
                         << std::setprecision(2) << result.throughput << '\n';
    }

    LatencyResult latency=run_latency_once<QueueType, CAPACITY>(
        producerCount, consumerCount, totalItems);
    latencyOutput << queueName << ',' << get_affinity_name() << ',' << CAPACITY << ','
                  << producerCount << ',' << consumerCount << ',' << totalItems << ',' << latency.samples << ','
                  << latency.p50 << ',' << latency.p95 << ',' << latency.p99 << '\n';

    std::sort(throughputs.begin(), throughputs.end());
    double median=throughputs[throughputs.size()/2];
    std::cout << std::setw(14) << queueName
              << " cap=" << std::setw(4) << CAPACITY
              << "  " << producerCount << "P/" << consumerCount << "C"
              << "  median=" << std::fixed << std::setprecision(2)
              << median/1000000.0 << " M ops/s"
              << "  p99=" << latency.p99 << " ns\n";
}

template <std::size_t CAPACITY>
void run_capacity(std::size_t totalItems, int runCount, std::ofstream& throughputOutput,
                  std::ofstream& latencyOutput) {
    constexpr int SCENARIOS[][2]={{1,1},{2,2},{4,4},{8,8},{1,4},{4,1}};

    for(const auto& scenario: SCENARIOS) {
        run_scenario<MpmcQueue, CAPACITY>("mpmc_padded", scenario[0], scenario[1],
                                          totalItems, runCount, throughputOutput, latencyOutput);
        run_scenario<MpmcQueueUnpadded, CAPACITY>("mpmc_unpadded", scenario[0], scenario[1],
                                                  totalItems, runCount, throughputOutput, latencyOutput);
        run_scenario<MutexQueue, CAPACITY>("mutex", scenario[0], scenario[1],
                                           totalItems, runCount, throughputOutput, latencyOutput);
    }
}

int main(int argc, char** argv) {
    constexpr std::size_t DEFAULT_ITEMS = 1000000;
    constexpr int DEFAULT_RUNS = 5;

    std::size_t totalItems=DEFAULT_ITEMS;
    int runCount=DEFAULT_RUNS;
    std::string throughputPath="results/throughput.csv";
    std::string latencyPath="results/latency.csv";

    for(int i=1;i<argc;i++) {
        std::string argument=argv[i];
        if(argument=="--items" && i+1<argc)
            totalItems=std::stoull(argv[++i]);
        else if(argument=="--runs" && i+1<argc)
            runCount=std::stoi(argv[++i]);
        else if(argument=="--output" && i+1<argc)
            throughputPath=argv[++i];
        else if(argument=="--latency-output" && i+1<argc)
            latencyPath=argv[++i];
        else if(argument=="--pin")
            g_pinThreads=true;
    }

#ifndef __linux__
    if(g_pinThreads) {
        std::cerr << "--pin is currently supported only on Linux\n";
        return 1;
    }
#endif

    if(totalItems<2 || runCount<1) {
        std::cerr << "items must be >= 2 and runs must be >= 1\n";
        return 1;
    }

    std::filesystem::path throughputFile(throughputPath);
    std::filesystem::path latencyFile(latencyPath);
    if(throughputFile.has_parent_path())
        std::filesystem::create_directories(throughputFile.parent_path());
    if(latencyFile.has_parent_path())
        std::filesystem::create_directories(latencyFile.parent_path());

    std::ofstream throughputOutput(throughputPath);
    std::ofstream latencyOutput(latencyPath);
    if(!throughputOutput || !latencyOutput) {
        std::cerr << "could not open output files\n";
        return 1;
    }

    throughputOutput << "queue,affinity,capacity,producers,consumers,items,run,seconds,throughput_ops_per_sec\n";
    latencyOutput << "queue,affinity,capacity,producers,consumers,items,samples,p50_ns,p95_ns,p99_ns\n";

    std::cout << "items=" << totalItems << ", measured runs=" << runCount
              << ", affinity=" << get_affinity_name()
              << " (one warmup per scenario, separate sampled latency pass)\n\n";

#ifdef __linux__
    if(g_pinThreads)
        std::cout << "pinning worker i to logical CPU i % "
                  << std::thread::hardware_concurrency() << "\n\n";
#endif

    run_capacity<64>(totalItems, runCount, throughputOutput, latencyOutput);
    run_capacity<256>(totalItems, runCount, throughputOutput, latencyOutput);
    run_capacity<1024>(totalItems, runCount, throughputOutput, latencyOutput);
    run_capacity<4096>(totalItems, runCount, throughputOutput, latencyOutput);

    std::cout << "\nThroughput CSV written to " << throughputPath
              << "\nLatency CSV written to " << latencyPath << '\n';
}
