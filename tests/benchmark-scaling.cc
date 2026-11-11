/**
 * Thread scaling benchmark.
 *
 *   benchmark-scaling [workload] [items] [seconds] [max threads]
 *
 * Runs the workload with 1, 2, 4, ... threads for a fixed time and reports
 * the total throughput. Workloads:
 *
 *   middle  move a random item behind another random item
 *           (remove + insert_after). Threads work on different parts of
 *           the list, this is the workload that can scale.
 *   before  same with insert_before, which depends on the prev hints.
 *   lru     move a random item to the front (remove + push_front). All
 *           the threads meet at the head.
 *   queue   pop_front + push_back. All the threads meet at the head and
 *           at the tail.
 *
 * For reference, every workload can also be run on a plain doubly linked
 * list protected by a mutex: prefix the workload with "mutex-".
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ut/lock_free_list.h"

namespace {

struct Item {
  ut::Node& node() noexcept { return m_node; }

  ut::Node m_node{};
  int m_value{};
};

using Lock_free_list = ut::List<Item, &Item::node>;

/** The reference: an index linked list with one big lock. */
struct Mutex_list {
  static constexpr uint32_t NIL = ~0U;

  struct Links {
    uint32_t m_next{NIL};
    uint32_t m_prev{NIL};
    bool m_linked{};
  };

  Mutex_list(Item* base, Item* end) : m_base(base), m_links(static_cast<size_t>(end - base)) {}

  uint32_t index(const Item& item) const { return static_cast<uint32_t>(&item - m_base); }

  void link(uint32_t i, uint32_t prev, uint32_t next) {
    m_links[i] = {next, prev, true};
    (prev == NIL ? m_head : m_links[prev].m_next) = i;
    (next == NIL ? m_tail : m_links[next].m_prev) = i;
  }

  void unlink(uint32_t i) {
    const auto [next, prev, linked] = m_links[i];
    (prev == NIL ? m_head : m_links[prev].m_next) = next;
    (next == NIL ? m_tail : m_links[next].m_prev) = prev;
    m_links[i].m_linked = false;
  }

  bool push_front(Item& item) {
    std::lock_guard guard(m_mutex);
    const auto i = index(item);
    if (m_links[i].m_linked) return false;
    link(i, NIL, m_head);
    return true;
  }

  bool push_back(Item& item) {
    std::lock_guard guard(m_mutex);
    const auto i = index(item);
    if (m_links[i].m_linked) return false;
    link(i, m_tail, NIL);
    return true;
  }

  bool insert_after(Item& item, Item& new_item) {
    std::lock_guard guard(m_mutex);
    const auto p = index(item);
    const auto i = index(new_item);
    if (!m_links[p].m_linked || m_links[i].m_linked) return false;
    link(i, p, m_links[p].m_next);
    return true;
  }

  bool insert_before(Item& item, Item& new_item) {
    std::lock_guard guard(m_mutex);
    const auto n = index(item);
    const auto i = index(new_item);
    if (!m_links[n].m_linked || m_links[i].m_linked) return false;
    link(i, m_links[n].m_prev, n);
    return true;
  }

  Item* remove(Item& item) {
    std::lock_guard guard(m_mutex);
    const auto i = index(item);
    if (!m_links[i].m_linked) return nullptr;
    unlink(i);
    return &item;
  }

  Item* pop_front() {
    std::lock_guard guard(m_mutex);
    if (m_head == NIL) return nullptr;
    const auto i = m_head;
    unlink(i);
    return m_base + i;
  }

  std::mutex m_mutex;
  Item* m_base;
  std::vector<Links> m_links;
  uint32_t m_head{NIL};
  uint32_t m_tail{NIL};
};

template <typename List>
void run(const std::string& workload, size_t n_items, double seconds, int max_threads) {
  for (int threads = 1; threads <= max_threads; threads *= 2) {
    auto buffer = std::make_unique<Item[]>(n_items);
    List list(buffer.get(), buffer.get() + n_items);

    for (size_t i = 0; i < n_items; ++i) {
      if (!list.push_back(buffer[i])) {
        std::abort();
      }
    }

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total{0};
    std::vector<std::thread> pool;

    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        std::mt19937_64 gen(static_cast<uint64_t>(t) + 1);
        uint64_t ops{};

        while (!go.load(std::memory_order_acquire)) {
        }

        while (!stop.load(std::memory_order_relaxed)) {
          auto& item = buffer[gen() % n_items];

          if (workload == "middle") {
            if (list.remove(item) != nullptr) {
              while (!list.insert_after(buffer[gen() % n_items], item)) {
              }
              ops += 2;
            }
          } else if (workload == "before") {
            if (list.remove(item) != nullptr) {
              while (!list.insert_before(buffer[gen() % n_items], item)) {
              }
              ops += 2;
            }
          } else if (workload == "lru") {
            if (list.remove(item) != nullptr) {
              if (!list.push_front(item)) {
                std::abort();
              }
              ops += 2;
            }
          } else {
            if (auto ptr = list.pop_front()) {
              if (!list.push_back(*ptr)) {
                std::abort();
              }
              ops += 2;
            }
          }
        }

        total.fetch_add(ops);
      });
    }

    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true);

    for (auto& thread : pool) {
      thread.join();
    }

    const auto rate = static_cast<double>(total.load()) / seconds / 1e6;

    std::printf("  threads=%3d  %8.2f M ops/s  %6.2f per thread\n", threads, rate, rate / threads);
  }
}

} // namespace

int main(int argc, char** argv) {
  std::string workload = argc > 1 ? argv[1] : "middle";
  const size_t n_items = argc > 2 ? std::strtoul(argv[2], nullptr, 0) : 65536;
  const double seconds = argc > 3 ? std::atof(argv[3]) : 1.0;
  const int max_threads = argc > 4 ? std::atoi(argv[4]) : 64;
  const bool reference = workload.rfind("mutex-", 0) == 0;

  if (reference) {
    workload = workload.substr(6);
  }

  if (workload != "middle" && workload != "before" && workload != "lru" && workload != "queue") {
    std::fprintf(stderr, "usage: %s [mutex-]middle|before|lru|queue [items] [seconds] [max threads]\n", argv[0]);
    return 1;
  }

  std::printf("%s%s, %zu items\n", reference ? "mutex-" : "", workload.c_str(), n_items);

  if (reference) {
    run<Mutex_list>(workload, n_items, seconds, max_threads);
  } else {
    run<Lock_free_list>(workload, n_items, seconds, max_threads);
  }
  return 0;
}
