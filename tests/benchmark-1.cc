#include <benchmark/benchmark.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <thread>
#include <vector>
#include <random>
#include "ut/lock_free_list.h"

namespace {

// Hardware performance counter wrapper
struct Perf_counter {
  explicit Perf_counter(perf_event_attr&& attr) {
    auto perf_event_attr = std::move(attr);
    m_fd = syscall(__NR_perf_event_open, &perf_event_attr, 0, -1, -1, 0);
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
    uint64_t count;
    ::read(m_fd, &count, sizeof(count));
    return count;
  }

private:
  int m_fd;
};

// Performance counter setup helper
perf_event_attr setup_counter(uint32_t type, uint64_t config) {
  perf_event_attr attr{};
  attr.type = type;
  attr.size = sizeof(perf_event_attr);
  attr.config = config;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  return attr;
}

struct Test_item {
  Test_item() = default;
  explicit Test_item(int value) : m_value(value) {}

  ut::Node& node() noexcept { return m_node; }

  int m_value{};
  ut::Node m_node{};
};

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

    auto cache_misses_attr = setup_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
    auto branch_misses_attr = setup_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
    // Setup performance counters
    m_cache_misses = std::make_unique<Perf_counter>(std::move(cache_misses_attr));
    m_branch_misses = std::make_unique<Perf_counter>(std::move(branch_misses_attr));
  }

  std::unique_ptr<Test_item[]> m_buffer;
  std::unique_ptr<ut::List<Test_item, &Test_item::node>> m_list;
  std::unique_ptr<Perf_counter> m_cache_misses;
  std::unique_ptr<Perf_counter> m_branch_misses;
};

} // anonymous namespace

// Single-threaded sequential operations benchmark
BENCHMARK_F(List_benchmark, Sequential_operations)(benchmark::State& state) {
  size_t index = 0;
  
  for (auto _ : state) {
    state.PauseTiming();
    index = 0;
    m_list = std::make_unique<ut::List<Test_item, &Test_item::node>>(
      m_buffer.get(),
      m_buffer.get() + BUFFER_SIZE
    );
    state.ResumeTiming();

    m_cache_misses->start();
    m_branch_misses->start();

    // Perform operations
    for (size_t i = 0; i < 1000; ++i) {
      m_buffer[index] = Test_item(static_cast<int>(i));
      m_list->push_back(m_buffer[index++]);
    }

    for (size_t i = 0; i < 500; ++i) {
      m_buffer[index] = Test_item(static_cast<int>(i));
      m_list->push_front(m_buffer[index++]);
    }

    auto it = m_list->begin();
    for (size_t i = 0; i < 250 && it != m_list->end(); ++i, ++it) {
      m_buffer[index] = Test_item(static_cast<int>(i));
      m_list->insert_after(*it, m_buffer[index++]);
    }

    m_cache_misses->stop();
    m_branch_misses->stop();
  }

  state.counters["CacheMisses"] = 
    benchmark::Counter(m_cache_misses->read(), benchmark::Counter::kAvgIterations);
  
  state.counters["BranchMisses"] = 
    benchmark::Counter(m_branch_misses->read(), benchmark::Counter::kAvgIterations);
}

// Contention benchmark - many threads operating on the same area

BENCHMARK_MAIN();