#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "ut/lock_free_list.h"

struct Test_item {
  Test_item() = default;
  explicit Test_item(int value) : m_value(value) {}

  ut::Node& node() noexcept { return m_node; }

  int m_value{};
  ut::Node m_node{};
};

class List_test : public ::testing::Test {
protected:
  static constexpr size_t BUFFER_SIZE = 1000;
  
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

TEST_F(List_test, empty_list_iterators) {
  EXPECT_EQ(m_list->begin(), m_list->end());
  EXPECT_EQ(m_list->rbegin(), m_list->rend());
}

TEST_F(List_test, push_front_single_element) {
  m_buffer[0] = Test_item(42);
  m_list->push_front(m_buffer[0]);
  
  auto it = m_list->begin();
  EXPECT_NE(it, m_list->end());
  EXPECT_EQ(it->m_value, 42);
  ++it;
  EXPECT_EQ(it, m_list->end());
}

TEST_F(List_test, push_back_single_element) {
  m_buffer[0] = Test_item(42);
  m_list->push_back(m_buffer[0]);
  
  auto it = m_list->begin();
  EXPECT_NE(it, m_list->end());
  EXPECT_EQ(it->m_value, 42);
  ++it;
  EXPECT_EQ(it, m_list->end());
}

TEST_F(List_test, multiple_push_front) {
  std::vector<int> values = {1, 2, 3, 4, 5};
  
  for (size_t i = 0; i < values.size(); ++i) {
    m_buffer[i] = Test_item(values[i]);
    m_list->push_front(m_buffer[i]);
  }
  
  std::vector<int> expected = {5, 4, 3, 2, 1};
  std::vector<int> actual;
  
  for (const auto& item : *m_list) {
    actual.push_back(item.m_value);
  }
  
  EXPECT_EQ(actual, expected);
}

TEST_F(List_test, multiple_push_back) {
  std::vector<int> values = {1, 2, 3, 4, 5};
  
  for (size_t i = 0; i < values.size(); ++i) {
    m_buffer[i] = Test_item(values[i]);
    m_list->push_back(m_buffer[i]);
  }
  
  std::vector<int> actual;
  for (const auto& item : *m_list) {
    actual.push_back(item.m_value);
  }
  
  EXPECT_EQ(actual, values);
}

TEST_F(List_test, reverse_iteration) {
  std::vector<int> values = {1, 2, 3, 4, 5};
  
  for (size_t i = 0; i < values.size(); ++i) {
    m_buffer[i] = Test_item(values[i]);
    m_list->push_back(m_buffer[i]);
  }
  
  std::vector<int> expected = {5, 4, 3, 2, 1};
  std::vector<int> actual;
  
  for (auto it = m_list->rbegin(); it != m_list->rend(); ++it) {
    actual.push_back(it->m_value);
  }
  
  EXPECT_EQ(actual, expected);
}

TEST_F(List_test, insert_after) {
  // Create initial list: 1 -> 2 -> 4
  m_buffer[0] = Test_item(1);
  m_buffer[1] = Test_item(2);
  m_buffer[2] = Test_item(4);
  
  m_list->push_back(m_buffer[0]);
  m_list->push_back(m_buffer[1]);
  m_list->push_back(m_buffer[2]);
  
  // Insert 3 after 2
  m_buffer[3] = Test_item(3);
  m_list->insert_after(m_buffer[1], m_buffer[3]);
  
  std::vector<int> expected = {1, 2, 3, 4};
  std::vector<int> actual;
  
  for (const auto& item : *m_list) {
    actual.push_back(item.m_value);
  }
  
  EXPECT_EQ(actual, expected);
}

TEST_F(List_test, insert_before) {
  // Create initial list: 1 -> 2 -> 4
  m_buffer[0] = Test_item(1);
  m_buffer[1] = Test_item(2);
  m_buffer[2] = Test_item(4);
  
  m_list->push_back(m_buffer[0]);
  m_list->push_back(m_buffer[1]);
  m_list->push_back(m_buffer[2]);
  
  // Insert 3 before 4
  m_buffer[3] = Test_item(3);
  m_list->insert_before(m_buffer[2], m_buffer[3]);
  
  std::vector<int> expected = {1, 2, 3, 4};
  std::vector<int> actual;
  
  for (const auto& item : *m_list) {
    actual.push_back(item.m_value);
  }
  
  EXPECT_EQ(actual, expected);
}

TEST_F(List_test, find_existing_element) {
  std::vector<int> values = {1, 2, 3, 4, 5};
  
  for (size_t i = 0; i < values.size(); ++i) {
    m_buffer[i] = Test_item(values[i]);
    m_list->push_back(m_buffer[i]);
  }
  
  auto result = m_list->find([](const Test_item* item) {
    return item->m_value == 3;
  });
  
  EXPECT_NE(result, nullptr);
  EXPECT_EQ(result->m_value, 3);
}

TEST_F(List_test, find_non_existing_element) {
  std::vector<int> values = {1, 2, 3, 4, 5};
  
  for (size_t i = 0; i < values.size(); ++i) {
    m_buffer[i] = Test_item(values[i]);
    m_list->push_back(m_buffer[i]);
  }
  
  auto result = m_list->find([](const Test_item* item) {
    return item->m_value == 42;
  });
  
  EXPECT_EQ(result, nullptr);
}

TEST_F(List_test, concurrent_push_back) {
  constexpr size_t NUM_THREADS = 4;
  constexpr size_t ITEMS_PER_THREAD = 100;
  
  std::vector<std::thread> threads;
  
  for (size_t t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([this, t]() {
      for (size_t i = 0; i < ITEMS_PER_THREAD; ++i) {
        size_t index = t * ITEMS_PER_THREAD + i;
        m_buffer[index] = Test_item(static_cast<int>(index));
        m_list->push_back(m_buffer[index]);
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  // Verify all elements are present (order may vary)
  std::vector<bool> found(NUM_THREADS * ITEMS_PER_THREAD, false);
  size_t count = 0;
  
  for (const auto& item : *m_list) {
    EXPECT_LT(item.m_value, NUM_THREADS * ITEMS_PER_THREAD);
    EXPECT_FALSE(found[item.m_value]) << "Duplicate value found: " << item.m_value;
    found[item.m_value] = true;
    count++;
  }
  
  EXPECT_EQ(count, NUM_THREADS * ITEMS_PER_THREAD);
  EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool v) { return v; }));
}

TEST_F(List_test, size_tracking) {
  // Test initial size
  EXPECT_EQ(m_list->size(), 0);

  // Test push_front increments size
  m_buffer[0] = Test_item(1);
  m_list->push_front(m_buffer[0]);
  EXPECT_EQ(m_list->size(), 1);

  // Test push_back increments size
  m_buffer[1] = Test_item(2);
  m_list->push_back(m_buffer[1]);
  EXPECT_EQ(m_list->size(), 2);

  // Test insert_after increments size
  m_buffer[2] = Test_item(3);
  m_list->insert_after(m_buffer[0], m_buffer[2]);
  EXPECT_EQ(m_list->size(), 3);

  // Test insert_before increments size
  m_buffer[3] = Test_item(4);
  m_list->insert_before(m_buffer[1], m_buffer[3]);
  EXPECT_EQ(m_list->size(), 4);

  // Test remove decrements size
  m_list->remove(m_buffer[2]);
  EXPECT_EQ(m_list->size(), 3);

  // Test pop_front decrements size
  auto* popped1 = m_list->pop_front();
  EXPECT_NE(popped1, nullptr);
  EXPECT_EQ(m_list->size(), 2);

  // Test pop_back decrements size
  auto* popped2 = m_list->pop_back();
  EXPECT_NE(popped2, nullptr);
  EXPECT_EQ(m_list->size(), 1);

  // Remove last element
  auto* popped3 = m_list->pop_front();
  EXPECT_NE(popped3, nullptr);
  EXPECT_EQ(m_list->size(), 0);
}

