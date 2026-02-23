#include <benchmark/benchmark.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <thread>
#include <vector>
#include <random>
#include "ut/lock_free_list.h"

namespace benchmark_utils {

struct Test_item {
  Test_item() = default;
  explicit Test_item(int value) : m_value(value) {}

  ut::Node& node() noexcept { return m_node; }

  int m_value{};
  ut::Node m_node{};
};

class Perf_counter {
public:
  explicit Perf_counter(uint64_t config) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(perf_event_attr);
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    m_fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    if (m_fd < 0) {
      throw std::runtime_error("Failed to create perf counter");
    }
  }

  ~Perf_counter() {
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  void start() {
    ioctl(m_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(m_fd, PERF_EVENT_IOC_ENABLE, 0);
  }

  void stop() {
    ioctl(m_fd, PERF_EVENT_IOC_DISABLE, 0);
  }

  uint64_t read() {
    uint64_t count{};
    if (::read(m_fd, &count, sizeof(count)) != sizeof(count)) {
      return 0;
    }
    return count;
  }

private:
  int m_fd;
};

} // namespace benchmark_utils

using namespace benchmark_utils;

// Benchmark fixture
class List_benchmark : public benchmark::Fixture {
protected:
  static constexpr size_t BUFFER_SIZE = 1000000;

  void SetUp(const benchmark::State&) override {
    m_buffer = std::make_unique<Test_item[]>(BUFFER_SIZE);
    m_list = std::make_unique<ut::List<Test_item, &Test_item::node>>(
      m_buffer.get(),
      m_buffer.get() + BUFFER_SIZE
    );

    m_cache_misses = std::make_unique<Perf_counter>(PERF_COUNT_HW_CACHE_MISSES);
    m_branch_misses = std::make_unique<Perf_counter>(PERF_COUNT_HW_BRANCH_MISSES);
  }

  void TearDown(const benchmark::State&) override {
    m_buffer.reset();
    m_list.reset();
    m_cache_misses.reset();
    m_branch_misses.reset();
  }

  std::unique_ptr<Test_item[]> m_buffer;
  std::unique_ptr<ut::List<Test_item, &Test_item::node>> m_list;
  std::unique_ptr<Perf_counter> m_cache_misses;
  std::unique_ptr<Perf_counter> m_branch_misses;
};

BENCHMARK_DEFINE_F(List_benchmark, Mixed_workload)(benchmark::State& state) {
  const int num_threads = static_cast<int>(state.range(0));
  const size_t ops_per_thread = 10000 / num_threads;

  for (auto _ : state) {
    state.PauseTiming();
    // Reset list
    m_list = std::make_unique<ut::List<Test_item, &Test_item::node>>(
      m_buffer.get(),
      m_buffer.get() + BUFFER_SIZE
    );
    std::atomic<size_t> next_index{0};
    std::vector<std::thread> threads;
    state.ResumeTiming();

    m_cache_misses->start();
    m_branch_misses->start();

    // Launch worker threads
    for (int t = 0; t < num_threads; ++t) {
      threads.emplace_back([&, t]() {
        std::mt19937 gen(t); // Deterministic seed per thread
        std::uniform_int_distribution<> op_dis(0, 4);
        std::uniform_int_distribution<> val_dis(0, 999);

        for (size_t i = 0; i < ops_per_thread; ++i) {
          size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
          if (index >= BUFFER_SIZE) break;

          m_buffer[index] = Test_item(val_dis(gen));
          
          switch (op_dis(gen)) {
            case 0: // push_back - may fail under contention
              if (!m_list->push_back(m_buffer[index])) { /* contention failure */ }
              break;

            case 1: // push_front - may fail under contention
              if (!m_list->push_front(m_buffer[index])) { /* contention failure */ }
              break;

            case 2: { // insert_after with find
              auto target = m_list->find([val = val_dis(gen)](const Test_item* item) {
                return item->m_value == val;
              });
              if (target) {
                if (!m_list->insert_after(*target, m_buffer[index])) { /* contention failure */ }
              }
              break;
            }

            case 3: { // remove with find
              auto target = m_list->find([val = val_dis(gen)](const Test_item* item) {
                return item->m_value == val;
              });
              if (target) {
                if (m_list->remove(*target) == nullptr) { /* contention failure */ }
              }
              break;
            }
            
            case 4: { // traversal
              size_t count = 0;
              for (const auto& item : *m_list) {
                (void)item;
                if (++count >= 100) break;
              }
              break;
            }
          }
        }
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    m_cache_misses->stop();
    m_branch_misses->stop();
  }

  state.counters["CacheMisses/Thread"] = 
    benchmark::Counter(m_cache_misses->read() / num_threads,
                      benchmark::Counter::kAvgIterations);
  
  state.counters["BranchMisses/Thread"] = 
    benchmark::Counter(m_branch_misses->read() / num_threads,
                      benchmark::Counter::kAvgIterations);

  state.SetItemsProcessed(state.iterations() * num_threads * ops_per_thread);
}

BENCHMARK_DEFINE_F(List_benchmark, High_contention)(benchmark::State& state) {
  const int num_threads = static_cast<int>(state.range(0));
  const size_t ops_per_thread = 10000 / num_threads;

  for (auto _ : state) {
    state.PauseTiming();
    // Reset list and initialize with contended data
    m_list = std::make_unique<ut::List<Test_item, &Test_item::node>>(
      m_buffer.get(),
      m_buffer.get() + BUFFER_SIZE
    );

    // Initialize with small set of data for high contention
    for (size_t i = 0; i < 10; ++i) {
      m_buffer[i] = Test_item(static_cast<int>(i));
      if (!m_list->push_back(m_buffer[i])) {
        state.SkipWithError("Initialization push_back failed");
        return;
      }
    }

    std::atomic<size_t> next_index{10}; // Start after initial elements
    std::vector<std::thread> threads;
    state.ResumeTiming();

    m_cache_misses->start();
    m_branch_misses->start();

    // Launch worker threads
    for (int t = 0; t < num_threads; ++t) {
      threads.emplace_back([&, t]() {
        std::mt19937 gen(t);
        std::uniform_int_distribution<> op_dis(0, 2);
        std::uniform_int_distribution<> pos_dis(0, 9); // Target initial elements

        for (size_t i = 0; i < ops_per_thread; ++i) {
          size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
          if (index >= BUFFER_SIZE) break;

          m_buffer[index] = Test_item(pos_dis(gen));
          
          // Find one of the initial elements
          auto target = m_list->find([val = pos_dis(gen)](const Test_item* item) {
            return item->m_value == val;
          });

          if (target) {
            switch (op_dis(gen)) {
              case 0:
                if (!m_list->insert_after(*target, m_buffer[index])) { /* contention failure */ }
                break;
              case 1:
                if (!m_list->insert_before(*target, m_buffer[index])) { /* contention failure */ }
                break;
              case 2:
                if (m_list->remove(*target) == nullptr) { /* contention failure */ }
                break;
            }
          }
        }
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    m_cache_misses->stop();
    m_branch_misses->stop();
  }

  state.counters["CacheMisses/Thread"] = 
    benchmark::Counter(m_cache_misses->read() / num_threads,
                      benchmark::Counter::kAvgIterations);
  
  state.counters["BranchMisses/Thread"] = 
    benchmark::Counter(m_branch_misses->read() / num_threads,
                      benchmark::Counter::kAvgIterations);

  state.SetItemsProcessed(state.iterations() * num_threads * ops_per_thread);
}

// Register benchmarks
BENCHMARK_REGISTER_F(List_benchmark, Mixed_workload)
  ->RangeMultiplier(2)
  ->Range(1, 32)
  ->UseRealTime()
  ->Unit(benchmark::kMillisecond);

BENCHMARK_REGISTER_F(List_benchmark, High_contention)
  ->RangeMultiplier(2)
  ->Range(1, 32)
  ->UseRealTime()
  ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();

