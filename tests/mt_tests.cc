#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <algorithm>

#include "ut/lock_free_list.h"

struct Test_item {
  Test_item()  {
    assert(m_node.is_null());
  }
  explicit Test_item(int value) : m_value(value) {}

  ut::Node& node() noexcept { return m_node; }

  int m_value{};
  ut::Node m_node{};
};

class Multi_threaded_list_test : public ::testing::Test {
protected:
  static constexpr size_t BUFFER_SIZE = 100000;
  
  void SetUp() override {
    m_buffer = std::make_unique<Test_item[]>(BUFFER_SIZE);
    m_list = std::make_unique<ut::List<Test_item, &Test_item::node>>(
      m_buffer.get(), 
      m_buffer.get() + BUFFER_SIZE
    );
  }

  std::unique_ptr<Test_item[]> m_buffer;
  std::unique_ptr<ut::List<Test_item, &Test_item::node>> m_list;
};

TEST_F(Multi_threaded_list_test, concurrent_push_front) {
  constexpr size_t NUM_THREADS = 8;
  constexpr size_t ITEMS_PER_THREAD = 1000;
  std::atomic<size_t> next_index{0};
  
  std::vector<std::thread> threads;
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (size_t i = 0; i < ITEMS_PER_THREAD; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[index] = Test_item(static_cast<int>(index));
        m_list->push_front(m_buffer[index]);
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }

  // Verify all elements are present
  std::vector<bool> found(NUM_THREADS * ITEMS_PER_THREAD, false);
  size_t count = 0;
  
  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, NUM_THREADS * ITEMS_PER_THREAD);
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }
  
  EXPECT_EQ(count, NUM_THREADS * ITEMS_PER_THREAD);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
}

TEST_F(Multi_threaded_list_test, concurrent_push_back) {
  constexpr size_t NUM_THREADS = 8;
  constexpr size_t ITEMS_PER_THREAD = 1000;
  std::atomic<size_t> next_index{0};
  
  std::vector<std::thread> threads;
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (size_t i = 0; i < ITEMS_PER_THREAD; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[index] = Test_item(static_cast<int>(index));
        m_list->push_back(m_buffer[index]);
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }

  // Verify all elements are present
  std::vector<bool> found(NUM_THREADS * ITEMS_PER_THREAD, false);
  size_t count = 0;
  
  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, NUM_THREADS * ITEMS_PER_THREAD);
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }
  
  EXPECT_EQ(count, NUM_THREADS * ITEMS_PER_THREAD);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
}

TEST_F(Multi_threaded_list_test, mixed_push_front_and_back) {
  constexpr size_t NUM_THREADS = 8;
  constexpr size_t ITEMS_PER_THREAD = 1000;
  std::atomic<size_t> next_index{0};
  
  std::vector<std::thread> threads;
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&, t]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> dis(0, 1);
      
      for (size_t i = 0; i < ITEMS_PER_THREAD; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[index] = Test_item(static_cast<int>(index));
        
        if (dis(gen)) {
          m_list->push_front(m_buffer[index]);
        } else {
          m_list->push_back(m_buffer[index]);
        }
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }

  // Verify all elements are present
  std::vector<bool> found(NUM_THREADS * ITEMS_PER_THREAD, false);
  size_t count = 0;
  
  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, NUM_THREADS * ITEMS_PER_THREAD);
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }
  
  EXPECT_EQ(count, NUM_THREADS * ITEMS_PER_THREAD);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
}

TEST_F(Multi_threaded_list_test, concurrent_inserts) {
  constexpr size_t NUM_THREADS = 8;
  std::vector<std::vector<size_t>> base_indices(NUM_THREADS);

  // First create a base list
  constexpr size_t BASE_SIZE = 8;
  for (size_t i = 0; i < BASE_SIZE; ++i) {
    m_buffer[i] = Test_item(static_cast<int>(i * 2)); // Create gaps for insertions
    m_list->push_back(m_buffer[i]);
    base_indices[0].push_back(m_buffer[i].m_value);
  }

  constexpr size_t ITEMS_PER_THREAD = 1;
  std::atomic<size_t> next_index{BASE_SIZE};
  
  std::atomic<size_t> insert_count{BASE_SIZE};

  std::vector<std::thread> threads;

  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&, t]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> base_index_dis(0, BASE_SIZE - 1);
      
      for (size_t i = 0; i < ITEMS_PER_THREAD; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        size_t base_index = base_index_dis(gen);
        
        m_buffer[index] = Test_item(static_cast<int>(index));

        EXPECT_TRUE(m_list->insert_after(m_buffer[base_index], m_buffer[index]));
        insert_count.fetch_add(1, std::memory_order_relaxed);
        base_indices[t].push_back(m_buffer[index].m_value);
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }

  std::vector<size_t> base_indices_sorted;
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    base_indices_sorted.insert(base_indices_sorted.end(), base_indices[t].begin(), base_indices[t].end());
  }

  std::sort(base_indices_sorted.begin(), base_indices_sorted.end());
  ASSERT_EQ(base_indices_sorted.size(), insert_count.load(std::memory_order_relaxed));

  // Check that all inserted elements are present
  for (auto index : base_indices_sorted) {
    EXPECT_TRUE(m_list->find([index](const Test_item* item) {
      return item->m_value == (int)index;
    }));
  }
}

TEST_F(Multi_threaded_list_test, concurrent_mixed_operations) {
  constexpr size_t NUM_THREADS = 8;
  constexpr size_t OPERATIONS_PER_THREAD = 1000;
  std::atomic<size_t> next_index{0};
  
  // Initialize with some data
  for (size_t i = 0; i < 100; ++i) {
    m_buffer[i] = Test_item(static_cast<int>(i));
    m_list->push_back(m_buffer[i]);
  }
  next_index.store(100);

  std::vector<std::thread> threads;
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> op_dis(0, 3); // 4 operations
      
      for (size_t i = 0; i < OPERATIONS_PER_THREAD; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[index] = Test_item(static_cast<int>(index));
        
        switch (op_dis(gen)) {
          case 0:
            m_list->push_front(m_buffer[index]);
            break;
          case 1:
            m_list->push_back(m_buffer[index]);
            break;
          case 2: {
            // Find random existing item for insert_after
            auto target = m_list->find([&gen](const Test_item* item) {
              static thread_local int target_val = std::uniform_int_distribution<>(0, 99)(gen);
              return item->m_value == target_val;
            });
            if (target) {
              m_list->insert_after(*target, m_buffer[index]);
            } else {
              m_list->push_back(m_buffer[index]);
            }
            break;
          }
          case 3: {
            // Find random existing item for insert_before
            auto target = m_list->find([&gen](const Test_item* item) {
              static thread_local int target_val = std::uniform_int_distribution<>(0, 99)(gen);
              return item->m_value == target_val;
            });
            if (target) {
              m_list->insert_before(*target, m_buffer[index]);
            } else {
              m_list->push_front(m_buffer[index]);
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

  // Verify final state
  std::vector<bool> found(next_index.load(), false);
  size_t count = 0;
  
  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, next_index.load());
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }
  
  EXPECT_EQ(count, 100 + NUM_THREADS * OPERATIONS_PER_THREAD);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
}

TEST_F(Multi_threaded_list_test, concurrent_iterators) {
  // First populate the list
  constexpr size_t INITIAL_ITEMS = 1000;
  for (size_t i = 0; i < INITIAL_ITEMS; ++i) {
    m_buffer[i] = Test_item(static_cast<int>(i));
    m_list->push_back(m_buffer[i]);
  }

  constexpr size_t NUM_READER_THREADS = 4;
  constexpr size_t NUM_WRITER_THREADS = 4;
  std::atomic<bool> stop{false};
  std::atomic<size_t> next_index{INITIAL_ITEMS};
  std::atomic<size_t> iterations_completed{0};
  
  // Create reader threads that continuously iterate
  std::vector<std::thread> reader_threads;
  for (size_t t = 0; t < NUM_READER_THREADS; ++t) {
    reader_threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        size_t count = 0;
        for (const auto& item : *m_list) {
          (void)item; // Prevent unused variable warning
          count++;
        }
        iterations_completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Create writer threads that modify the list
  std::vector<std::thread> writer_threads;
  for (size_t t = 0; t < NUM_WRITER_THREADS; ++t) {
    writer_threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> op_dis(0, 1);
      
      for (size_t i = 0; i < 1000; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[index] = Test_item(static_cast<int>(index));
        
        if (op_dis(gen)) {
          m_list->push_front(m_buffer[index]);
        } else {
          m_list->push_back(m_buffer[index]);
        }
      }
    });
  }

  // Let writers complete
  for (auto& thread : writer_threads) {
    thread.join();
  }

  // Let readers run a bit longer then stop them
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_release);
  
  for (auto& thread : reader_threads) {
    thread.join();
  }

  // Verify final state
  std::vector<bool> found(next_index.load(), false);
  size_t count = 0;
  
  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, next_index.load());
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }
  
  EXPECT_EQ(count, INITIAL_ITEMS + NUM_WRITER_THREADS * 1000);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
  
  // Verify that iterations completed successfully
  EXPECT_GT(iterations_completed.load(), 0UL);
  std::cout << "Completed " << iterations_completed.load() << " iterations during concurrent modifications\n";
}

TEST_F(Multi_threaded_list_test, concurrent_find_and_modify) {
  // Initialize with some data
  constexpr size_t INITIAL_ITEMS = 1000;
  for (size_t i = 0; i < INITIAL_ITEMS; ++i) {
    m_buffer[i] = Test_item(static_cast<int>(i));
    m_list->push_back(m_buffer[i]);
  }

  constexpr size_t NUM_FINDER_THREADS = 4;
  constexpr size_t NUM_MODIFIER_THREADS = 4;
  constexpr size_t OPERATIONS_PER_THREAD = 1000;
  std::atomic<size_t> next_index{INITIAL_ITEMS};
  std::atomic<size_t> finds_completed{0};
  std::atomic<bool> stop{false};

  // Create finder threads
  std::vector<std::thread> finder_threads;
  for (size_t t = 0; t < NUM_FINDER_THREADS; ++t) {
    finder_threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> value_dis(0, INITIAL_ITEMS - 1);

      while (!stop.load(std::memory_order_relaxed)) {
        int target_value = value_dis(gen);
        auto result = m_list->find([target_value](const Test_item* item) {
          return item->m_value == target_value;
        });
        
        if (result) {
          finds_completed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // Create modifier threads
  std::vector<std::thread> modifier_threads;
  for (size_t t = 0; t < NUM_MODIFIER_THREADS; ++t) {
    modifier_threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> op_dis(0, 3);
      std::uniform_int_distribution<> pos_dis(0, INITIAL_ITEMS - 1);

      for (size_t i = 0; i < OPERATIONS_PER_THREAD; ++i) {
        size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[index] = Test_item(static_cast<int>(index));

        int target_pos = pos_dis(gen);
        auto target = m_list->find([target_pos](const Test_item* item) {
          return item->m_value == target_pos;
        });

        switch (op_dis(gen)) {
          case 0:
            m_list->push_front(m_buffer[index]);
            break;
          case 1:
            m_list->push_back(m_buffer[index]);
            break;
          case 2:
            if (target) {
              m_list->insert_after(*target, m_buffer[index]);
            } else {
              m_list->push_back(m_buffer[index]);
            }
            break;
          case 3:
            if (target) {
              m_list->insert_before(*target, m_buffer[index]);
            } else {
              m_list->push_front(m_buffer[index]);
            }
            break;
        }
      }
    });
  }

  // Let modifiers complete
  for (auto& thread : modifier_threads) {
    thread.join();
  }

  // Let finders run a bit longer then stop them
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_release);

  for (auto& thread : finder_threads) {
    thread.join();
  }

  // Verify final state
  std::vector<bool> found(next_index.load(), false);
  size_t count = 0;

  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, next_index.load());
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }

  EXPECT_EQ(count, INITIAL_ITEMS + NUM_MODIFIER_THREADS * OPERATIONS_PER_THREAD);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
  EXPECT_GT(finds_completed.load(), 0UL);
  
  std::cout << "Completed " << finds_completed.load() << " successful finds during concurrent modifications\n";
}

TEST_F(Multi_threaded_list_test, stress_test) {
  constexpr size_t NUM_THREADS = 8;
  constexpr size_t OPERATIONS_PER_THREAD = 10000;
  std::atomic<size_t> next_index{0};
  std::atomic<size_t> operations_completed{0};

  std::vector<std::thread> threads;
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> op_dis(0, 9); // 10 different operations

      for (size_t i = 0; i < OPERATIONS_PER_THREAD; ++i) {
        int op = op_dis(gen);

        if (op <= 3) { // 40% new insertions
          size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
          m_buffer[index] = Test_item(static_cast<int>(index));

          switch (op) {
            case 0:
              m_list->push_front(m_buffer[index]);
              break;
            case 1:
              m_list->push_back(m_buffer[index]);
              break;
            case 2:
            case 3: {
              // Find random existing item
              auto target = m_list->find([&gen](const Test_item* item) {
                static thread_local int target_val =
                  std::uniform_int_distribution<>(0, 1000)(gen);
                return item->m_value == target_val;
              });

              if (target) {
                if (op == 2) {
                  m_list->insert_after(*target, m_buffer[index]);
                } else {
                  m_list->insert_before(*target, m_buffer[index]);
                }
              } else {
                m_list->push_back(m_buffer[index]);
              }
              break;
            }
          }
        } else { // 60% traversal and find operations
          // Traverse or find operations
          if (op < 7) {
            // Full traversal
            size_t count = 0;
            for (const auto& item : *m_list) {
              (void)item;
              count++;
            }
          } else {
            // Find operation
            static thread_local int target_val = std::uniform_int_distribution<>(0, 1000)(gen);
            auto &ref = target_val;
            (void) m_list->find([&ref](const Test_item* item) {
              return item->m_value == ref;
            });
          }
        }

        operations_completed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Join all threads
  for (auto& thread : threads) {
    thread.join();
  }

  // Verify final state
  std::vector<bool> found(next_index.load(), false);
  size_t count = 0;

  for (const auto& item : *m_list) {
    ASSERT_LT(item.m_value, next_index.load());
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }

  std::cout << "Completed " << operations_completed.load() << " operations in stress test\n";
  std::cout << "Final list size: " << count << " nodes\n";
  std::cout << "Total unique values: " << next_index.load() << "\n";
}

// Test that reproduces the bug in remove() where partial adjacent node updates
// can leave the list in an inconsistent state (forward and backward traversals differ)
TEST_F(Multi_threaded_list_test, concurrent_remove_inconsistency_bug) {
  constexpr size_t NUM_ITERATIONS = 100;
  constexpr size_t LIST_SIZE = 10;

  for (size_t iter = 0; iter < NUM_ITERATIONS; ++iter) {
    // Reset list for each iteration - create fresh instances
    m_buffer = std::make_unique<Test_item[]>(BUFFER_SIZE);
    m_list = std::make_unique<ut::List<Test_item, &Test_item::node>>(
      m_buffer.get(),
      m_buffer.get() + BUFFER_SIZE
    );

    // Create a list: [0] <-> [1] <-> [2] <-> ... <-> [LIST_SIZE-1]
    for (size_t i = 0; i < LIST_SIZE; ++i) {
      m_buffer[i] = Test_item(static_cast<int>(i));
      ASSERT_TRUE(m_list->push_back(m_buffer[i]));
    }

    std::atomic<bool> start{false};
    std::atomic<size_t> next_insert_index{LIST_SIZE};

    // Thread 1: Removes middle nodes
    std::thread remover([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      // Try to remove nodes in the middle (indices 3, 4, 5, 6)
      for (size_t i = 3; i <= 6; ++i) {
        m_list->remove(m_buffer[i]);
      }
    });

    // Thread 2: Concurrently inserts before nodes that Thread 1 might be updating
    std::thread inserter([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      // Insert before nodes 4, 5, 6, 7 (these are the "next" nodes when removing 3, 4, 5, 6)
      for (size_t i = 4; i <= 7; ++i) {
        size_t idx = next_insert_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[idx] = Test_item(static_cast<int>(idx));
        m_list->insert_before(m_buffer[i], m_buffer[idx]);
      }
    });

    // Start both threads simultaneously
    start.store(true, std::memory_order_release);

    remover.join();
    inserter.join();

    // Verify list consistency: forward and backward traversal should find the same nodes
    std::vector<int> forward_values;
    std::vector<int> backward_values;

    // Safe forward iteration
    try {
      for (const auto& item : *m_list) {
        forward_values.push_back(item.m_value);
        if (forward_values.size() > BUFFER_SIZE) {
          FAIL() << "Infinite loop detected in forward iteration";
        }
      }
    } catch (const ut::Iterator_invalidated& e) {
      // Iterator was invalidated, skip this iteration
      continue;
    }

    // Safe backward iteration
    try {
      for (auto it = m_list->rbegin(); it != m_list->rend(); ++it) {
        backward_values.push_back(it->m_value);
        if (backward_values.size() > BUFFER_SIZE) {
          FAIL() << "Infinite loop detected in backward iteration";
        }
      }
    } catch (const ut::Iterator_invalidated& e) {
      // Iterator was invalidated, skip this iteration
      continue;
    }

    // Reverse backward_values to compare with forward
    std::reverse(backward_values.begin(), backward_values.end());

    // The bug manifests as forward and backward traversals finding different nodes
    ASSERT_EQ(forward_values.size(), backward_values.size())
        << "Iteration " << iter << ": Forward traversal found " << forward_values.size()
        << " nodes, backward found " << backward_values.size() << " nodes";

    ASSERT_EQ(forward_values, backward_values)
        << "Iteration " << iter << ": Forward and backward traversals found different nodes!";

    // Also verify size() matches actual count
    ASSERT_EQ(m_list->size(), forward_values.size())
        << "Iteration " << iter << ": size() = " << m_list->size()
        << " but actual count = " << forward_values.size();
  }

  std::cout << "Completed " << NUM_ITERATIONS << " iterations of concurrent remove test\n";
}

// More aggressive test: concurrent removes and inserts on overlapping nodes
TEST_F(Multi_threaded_list_test, concurrent_remove_and_insert_stress) {
  constexpr size_t NUM_THREADS = 4;
  constexpr size_t OPERATIONS_PER_THREAD = 500;
  constexpr size_t INITIAL_SIZE = 100;

  // Initialize list
  for (size_t i = 0; i < INITIAL_SIZE; ++i) {
    m_buffer[i] = Test_item(static_cast<int>(i));
    m_list->push_back(m_buffer[i]);
  }

  std::atomic<size_t> next_index{INITIAL_SIZE};
  std::atomic<bool> stop{false};
  std::atomic<size_t> successful_removes{0};
  std::atomic<size_t> successful_inserts{0};

  std::vector<std::thread> threads;

  // Remover threads
  for (size_t t = 0; t < NUM_THREADS / 2; ++t) {
    threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> dis(0, INITIAL_SIZE - 1);

      for (size_t i = 0; i < OPERATIONS_PER_THREAD && !stop.load(std::memory_order_relaxed); ++i) {
        int target = dis(gen);
        if (m_list->remove(m_buffer[target])) {
          successful_removes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // Inserter threads
  for (size_t t = 0; t < NUM_THREADS / 2; ++t) {
    threads.emplace_back([&]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> target_dis(0, INITIAL_SIZE - 1);
      std::uniform_int_distribution<> op_dis(0, 1);

      for (size_t i = 0; i < OPERATIONS_PER_THREAD && !stop.load(std::memory_order_relaxed); ++i) {
        size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
        m_buffer[idx] = Test_item(static_cast<int>(idx));

        int target = target_dis(gen);
        bool success = false;

        if (op_dis(gen) == 0) {
          success = m_list->insert_before(m_buffer[target], m_buffer[idx]);
        } else {
          success = m_list->insert_after(m_buffer[target], m_buffer[idx]);
        }

        if (!success) {
          // Target was removed, fall back to push_back
          m_list->push_back(m_buffer[idx]);
        }
        successful_inserts.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Verify list consistency
  std::vector<int> forward_values;
  std::vector<int> backward_values;

  for (const auto& item : *m_list) {
    forward_values.push_back(item.m_value);
  }

  for (auto it = m_list->rbegin(); it != m_list->rend(); ++it) {
    backward_values.push_back(it->m_value);
  }

  std::reverse(backward_values.begin(), backward_values.end());

  std::cout << "Successful removes: " << successful_removes.load() << "\n";
  std::cout << "Successful inserts: " << successful_inserts.load() << "\n";
  std::cout << "Forward traversal count: " << forward_values.size() << "\n";
  std::cout << "Backward traversal count: " << backward_values.size() << "\n";
  std::cout << "Reported size(): " << m_list->size() << "\n";

  // Check for inconsistency - this is the bug!
  EXPECT_EQ(forward_values.size(), backward_values.size())
      << "BUG DETECTED: Forward and backward traversals have different counts!";

  EXPECT_EQ(forward_values, backward_values)
      << "BUG DETECTED: Forward and backward traversals found different nodes!";
}

