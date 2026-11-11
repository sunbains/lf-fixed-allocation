#pragma once

/**
 * Deterministic simulation for lock-free code.
 *
 * The code under test is compiled against sim::Atomic<T> instead of
 * std::atomic<T>. Every atomic access is a scheduling point: the simulation
 * decides, using nothing but a seeded PRNG, which simulated thread performs
 * the next atomic step. Simulated threads are fibers multiplexed on a single
 * OS thread, so a (program, seed) pair always produces exactly the same
 * interleaving and any failure can be replayed and shrunk.
 *
 * What this explores: all sequentially-consistent interleavings of atomic
 * steps, including arbitrarily long stalls of a thread between two steps and
 * spurious compare_exchange_weak failures.
 *
 * What it does not explore: weak memory-order effects (use TSan and the real
 * multi-threaded tests for that).
 */

#include <ucontext.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace sim {

/** SplitMix64: small, fast and good enough to drive schedules and generators. */
struct Rng {
  explicit Rng(uint64_t seed) noexcept : m_state(seed) {}

  [[nodiscard]] uint64_t next() noexcept {
    uint64_t z = (m_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  /** @return uniformly distributed value in [0, n). */
  [[nodiscard]] uint64_t below(uint64_t n) noexcept {
    assert(n > 0);
    return next() % n;
  }

  /** @return value in [lo, hi]. */
  [[nodiscard]] uint64_t range(uint64_t lo, uint64_t hi) noexcept {
    return lo + below(hi - lo + 1);
  }

  [[nodiscard]] bool chance(uint64_t numerator, uint64_t denominator) noexcept {
    return below(denominator) < numerator;
  }

  uint64_t m_state;
};

[[nodiscard]] inline uint64_t mix(uint64_t a, uint64_t b) noexcept {
  Rng rng(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
  return rng.next();
}

struct Simulation {
  enum class Outcome {
    completed,
    /* The step budget was exhausted: a livelock or an unbounded loop. */
    step_limit
  };

  struct Options {
    uint64_t m_seed{};
    uint64_t m_max_steps{200'000};
  };

  explicit Simulation(Options options) noexcept
    : m_options(options), m_rng(options.m_seed) {}

  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  /** Run all the tasks to completion as simulated threads. */
  [[nodiscard]] Outcome run(std::vector<std::function<void()>> tasks) {
    assert(s_active == nullptr);

    m_tasks = std::move(tasks);
    m_fibers.clear();
    m_fibers.resize(m_tasks.size());
    m_alive = m_tasks.size();
    m_outcome = Outcome::completed;

    choose_strategy();

    for (size_t i = 0; i < m_fibers.size(); ++i) {
      auto& fiber = m_fibers[i];

      fiber.m_stack = acquire_stack();
      getcontext(&fiber.m_context);
      fiber.m_context.uc_stack.ss_sp = fiber.m_stack.get();
      fiber.m_context.uc_stack.ss_size = STACK_SIZE;
      fiber.m_context.uc_link = nullptr;
      makecontext(&fiber.m_context, reinterpret_cast<void (*)()>(&Simulation::trampoline), 1, static_cast<int>(i));
    }

    if (m_alive > 0) {
      s_active = this;
      m_current = pick_next();
      swapcontext(&m_main, &m_fibers[static_cast<size_t>(m_current)].m_context);
      s_active = nullptr;
      m_current = -1;
    }

    /* Fibers abandoned after a step limit are never resumed; their stacks
    are simply recycled. */
    for (auto& fiber : m_fibers) {
      release_stack(std::move(fiber.m_stack));
    }
    m_tasks.clear();

    return m_outcome;
  }

  /** Scheduling point; called before every simulated atomic access. */
  static void yield_point() noexcept {
    if (s_active != nullptr && s_active->m_current >= 0) {
      s_active->schedule();
    }
  }

  /** @return true if a weak CAS that would have succeeded should fail. */
  [[nodiscard]] static bool spurious_failure() noexcept {
    return s_active != nullptr && s_active->m_current >= 0 && s_active->m_rng.chance(1, 16);
  }

  /** Logical clock used to timestamp operation invocations and responses.
  Not a scheduling point. */
  [[nodiscard]] uint64_t tick() noexcept {
    return ++m_clock;
  }

  [[nodiscard]] uint64_t steps() const noexcept {
    return m_steps;
  }

  /** Print every atomic step of the simulated threads to stderr. Meant
  for replaying a failing seed. */
  static void set_trace(bool enable) noexcept {
    s_trace = enable;
  }

  template <typename T>
  static void trace(const char* op, const void* address, T before, T after, bool changed) noexcept {
    if (s_trace && s_active != nullptr && s_active->m_current >= 0) [[unlikely]] {
      std::fprintf(stderr, "  %6llu  thread %d  %-9s %p  %#018llx%s", static_cast<unsigned long long>(s_active->m_steps),
                   s_active->m_current, op, address, static_cast<unsigned long long>(before), changed ? "" : "\n");
      if (changed) {
        std::fprintf(stderr, " -> %#018llx\n", static_cast<unsigned long long>(after));
      }
    }
  }

private:
  static constexpr size_t STACK_SIZE = 256 * 1024;

  struct Fiber {
    ucontext_t m_context{};
    std::unique_ptr<char[]> m_stack{};
    bool m_done{};
    int64_t m_priority{};
  };

  enum class Strategy {
    /* Consider a context switch at every step with a fixed probability. */
    random,
    /* PCT style: run the highest priority thread, demote the running
    thread at a few randomly chosen steps. Good at finding bugs that need
    one thread to stall for a long time at a specific point. */
    priority
  };

  static void trampoline(int index) {
    auto self = s_active;

    self->m_tasks[static_cast<size_t>(index)]();
    self->m_fibers[static_cast<size_t>(index)].m_done = true;
    --self->m_alive;

    if (self->m_alive == 0) {
      setcontext(&self->m_main);
    } else {
      self->m_current = self->pick_next();
      setcontext(&self->m_fibers[static_cast<size_t>(self->m_current)].m_context);
    }
  }

  void choose_strategy() noexcept {
    const auto n = static_cast<int64_t>(m_fibers.size());

    m_change_points.clear();
    m_low_priority = 0;

    if (m_rng.chance(3, 10)) {
      m_strategy = Strategy::priority;

      for (int64_t i = 0; i < n; ++i) {
        m_fibers[static_cast<size_t>(i)].m_priority = n + static_cast<int64_t>(m_rng.below(1000));
      }

      const auto horizon = m_rng.range(20, 600);

      for (auto i = m_rng.range(1, 5); i > 0; --i) {
        m_change_points.push_back(m_rng.range(1, horizon));
      }
    } else {
      static constexpr uint64_t PER_MILLE[] = {1000, 1000, 500, 250, 100, 30};

      m_strategy = Strategy::random;
      m_switch_per_mille = PER_MILLE[m_rng.below(std::size(PER_MILLE))];
    }
  }

  [[nodiscard]] int pick_random() noexcept {
    auto k = m_rng.below(m_alive);

    for (size_t i = 0; i < m_fibers.size(); ++i) {
      if (!m_fibers[i].m_done && k-- == 0) {
        return static_cast<int>(i);
      }
    }
    assert(false);
    return -1;
  }

  [[nodiscard]] int pick_next() noexcept {
    if (m_strategy == Strategy::random) {
      return pick_random();
    }

    int best{-1};

    for (size_t i = 0; i < m_fibers.size(); ++i) {
      if (!m_fibers[i].m_done && (best < 0 || m_fibers[i].m_priority > m_fibers[static_cast<size_t>(best)].m_priority)) {
        best = static_cast<int>(i);
      }
    }
    return best;
  }

  void schedule() noexcept {
    if (++m_steps > m_options.m_max_steps) {
      /* Abandon every fiber, they are never resumed. */
      m_outcome = Outcome::step_limit;
      setcontext(&m_main);
    }

    int next{m_current};

    if (m_strategy == Strategy::random) {
      if (m_alive > 1 && m_rng.below(1000) < m_switch_per_mille) {
        next = pick_random();
      }
    } else {
      for (auto point : m_change_points) {
        if (point == m_steps) {
          m_fibers[static_cast<size_t>(m_current)].m_priority = --m_low_priority;
        }
      }
      next = pick_next();
    }

    if (next != m_current) {
      const auto prev = m_current;

      m_current = next;
      swapcontext(&m_fibers[static_cast<size_t>(prev)].m_context, &m_fibers[static_cast<size_t>(next)].m_context);
    }
  }

  [[nodiscard]] static std::vector<std::unique_ptr<char[]>>& stack_pool() {
    static std::vector<std::unique_ptr<char[]>> pool;
    return pool;
  }

  [[nodiscard]] static std::unique_ptr<char[]> acquire_stack() {
    auto& pool = stack_pool();

    if (pool.empty()) {
      return std::unique_ptr<char[]>(new char[STACK_SIZE]);
    }

    auto stack = std::move(pool.back());
    pool.pop_back();
    return stack;
  }

  static void release_stack(std::unique_ptr<char[]> stack) {
    stack_pool().push_back(std::move(stack));
  }

  static inline Simulation* s_active{};
  static inline bool s_trace{};

  Options m_options;
  Rng m_rng;
  ucontext_t m_main{};
  std::vector<std::function<void()>> m_tasks{};
  std::vector<Fiber> m_fibers{};
  size_t m_alive{};
  int m_current{-1};
  uint64_t m_steps{};
  uint64_t m_clock{};
  Outcome m_outcome{Outcome::completed};
  Strategy m_strategy{Strategy::random};
  uint64_t m_switch_per_mille{1000};
  std::vector<uint64_t> m_change_points{};
  int64_t m_low_priority{};
};

/** Drop-in replacement for std::atomic<T> (for the subset of the interface
that the list uses) that turns every access into a scheduling point. */
template <typename T>
struct Atomic {
  constexpr Atomic() noexcept = default;
  constexpr Atomic(T value) noexcept : m_value(value) {}

  Atomic(const Atomic&) = delete;
  Atomic& operator=(const Atomic&) = delete;

  [[nodiscard]] T load(std::memory_order = std::memory_order_seq_cst) const noexcept {
    Simulation::yield_point();
    Simulation::trace("load", this, m_value, m_value, false);
    return m_value;
  }

  void store(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
    Simulation::yield_point();
    Simulation::trace("store", this, m_value, value, true);
    m_value = value;
  }

  [[nodiscard]] T exchange(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
    Simulation::yield_point();
    Simulation::trace("exchange", this, m_value, value, true);
    auto old = m_value;
    m_value = value;
    return old;
  }

  bool compare_exchange_strong(T& expected, T desired, std::memory_order = std::memory_order_seq_cst) noexcept {
    Simulation::yield_point();
    if (m_value == expected) {
      Simulation::trace("cas", this, m_value, desired, true);
      m_value = desired;
      return true;
    }
    Simulation::trace("cas-fail", this, m_value, desired, false);
    expected = m_value;
    return false;
  }

  bool compare_exchange_strong(T& expected, T desired, std::memory_order, std::memory_order) noexcept {
    return compare_exchange_strong(expected, desired);
  }

  bool compare_exchange_weak(T& expected, T desired, std::memory_order = std::memory_order_seq_cst) noexcept {
    Simulation::yield_point();
    if (m_value == expected && !Simulation::spurious_failure()) {
      Simulation::trace("cas", this, m_value, desired, true);
      m_value = desired;
      return true;
    }
    Simulation::trace("cas-fail", this, m_value, desired, false);
    expected = m_value;
    return false;
  }

  bool compare_exchange_weak(T& expected, T desired, std::memory_order, std::memory_order) noexcept {
    return compare_exchange_weak(expected, desired);
  }

  T fetch_add(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
    Simulation::yield_point();
    auto old = m_value;
    m_value = static_cast<T>(m_value + value);
    Simulation::trace("fetch_add", this, old, m_value, true);
    return old;
  }

  T fetch_sub(T value, std::memory_order = std::memory_order_seq_cst) noexcept {
    Simulation::yield_point();
    auto old = m_value;
    m_value = static_cast<T>(m_value - value);
    Simulation::trace("fetch_sub", this, old, m_value, true);
    return old;
  }

private:
  T m_value{};
};

} // namespace sim
