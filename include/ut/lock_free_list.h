#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <immintrin.h>

namespace ut {

struct Iterator_invalidated : public std::runtime_error {
  explicit Iterator_invalidated(const char* msg) : std::runtime_error(msg) {}
};

struct Node {
  using Link_type = uint32_t;
  static constexpr Link_type NULL_PTR = std::numeric_limits<Link_type>::max();

  Node() = default;

  Node(Node&& rhs) noexcept
    : m_links(rhs.m_links.load(std::memory_order_relaxed)) {}

  Node(Link_type next, Link_type prev) noexcept;

  Node& operator=(Node&& rhs) noexcept {
    m_links.store(rhs.m_links.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  /** Atomic links storage, both next and prev links are stored in a single 64-bit integer */
  std::atomic<uint64_t> m_links{std::numeric_limits<uint64_t>::max()};
};

inline constexpr uint64_t pack_links(Node::Link_type next, Node::Link_type prev) noexcept {
  return (static_cast<uint64_t>(next) << 32) | prev;
}

inline std::pair<Node::Link_type, Node::Link_type> unpack_links(uint64_t links) noexcept {
  return {static_cast<Node::Link_type>(links >> 32), static_cast<Node::Link_type>(links & 0xFFFFFFFF)};
}

template <typename T>
const T* from_link(const T* base, Node::Link_type link) noexcept {
  return reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(base) + link);
}

template <typename T>
T* from_link(T* base, Node::Link_type link) noexcept {
  return const_cast<T*>(from_link(base, link));
}

template <typename T>
Node::Link_type to_link(T* base, T* ptr) noexcept {
  const auto ptr_addr = reinterpret_cast<std::byte*>(ptr);
  const auto base_addr = reinterpret_cast<std::byte*>(base);

  assert(ptr_addr >= base_addr);
  assert((ptr_addr - base_addr) % sizeof(T) == 0);
  assert((ptr_addr - base_addr) < std::numeric_limits<Node::Link_type>::max());

  return static_cast<Node::Link_type>(ptr_addr - base_addr);
}

inline Node::Node(Link_type next, Link_type prev) noexcept
  : m_links(pack_links(next, prev)) {}

template<typename T, auto N>
class List;

template<typename T, auto N, bool IsConst = false>
struct List_iterator {
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = T;
  using result_type = typename std::invoke_result_t<decltype(N), T>;
  using node_type = typename std::remove_reference<result_type>::type;
  using difference_type = std::ptrdiff_t;
  using pointer = std::conditional_t<IsConst, const T*, T*>;
  using reference = std::conditional_t<IsConst, const T&, T&>;
  using node_pointer = std::conditional_t<IsConst, const node_type*, node_type*>;

  /* Default constructor for end iterator */
  List_iterator() noexcept
    : m_current(),
      m_base() {}

  /* Allow conversion from non-const to const iterator */
  template<bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
  List_iterator(const List_iterator<T, N, WasConst>& rhs) noexcept
    : m_current(rhs.current()),
      m_base(rhs.base()) {}

  List_iterator(pointer base, node_pointer current) noexcept
    : m_base(base),
      m_current(current) {}

  node_pointer to_node(node_type::Link_type link) const noexcept {
    return List<T, N>::to_node(m_base, link);
  }

  pointer to_item(node_pointer node) const noexcept {
    return List<T, N>::to_item(m_base, *const_cast<node_type*>(node));
  }

  bool operator==(const List_iterator& rhs) const noexcept {
    return m_current == rhs.m_current;
  }

  bool operator!=(const List_iterator& rhs) const noexcept {
    return !(*this == rhs);
  }

  reference operator*() const noexcept {
    return *operator->();
  }

  pointer operator->() const noexcept {
    return to_item(m_current);
  }

  List_iterator& operator++() noexcept {
    if (m_current) {
      auto [next, _] = unpack_links(m_current->m_links.load(std::memory_order_acquire));

      m_current = next != node_type::NULL_PTR ? to_node(next) : nullptr;
    }
    return *this;
  }

  List_iterator operator++(int) noexcept {
    List_iterator tmp = *this;
    ++(*this);
    return tmp;
  }

  List_iterator& operator--() noexcept {
    if (m_current) {
      auto [next, prev] = unpack_links(m_current->m_links.load(std::memory_order_acquire));

      m_current = prev != node_type::NULL_PTR ? to_node(prev) : nullptr;
    }
    return *this;
  }

  List_iterator operator--(int) noexcept {
    List_iterator tmp = *this;
    --(*this);
    return tmp;
  }

  pointer m_base;
  node_pointer m_current;
};

template <typename T, auto N>
struct List {
  using value_type = T;
  using result_type = typename std::invoke_result_t<decltype(N), T>;
  using node_type = typename std::remove_reference<result_type>::type;
  using node_pointer = node_type*;
  using iterator = List_iterator<T, N, false>;
  using const_iterator = List_iterator<T, N, true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  List(T* base, T* end) noexcept
    : m_bounds(base, end) {
    assert(base <= end);
    assert(base != nullptr);
  }

  /**
   * @brief Get the node pointer from the link
   * 
   * @param[in] link Node's link (offset)
   * 
   * @return Pointer to the node in the containing type
   */
  static node_pointer to_node(const T *base, node_type::Link_type link) noexcept {
    return &(const_cast<T*>(base)[link].*N)();
  }

  static node_pointer to_node(T *base, node_type::Link_type link) noexcept {
    return &(base[link].*N)();
  }

  static T* to_item(const T* base, const node_type& node) noexcept {
    auto ptr = reinterpret_cast<const std::byte*>(&node);
    const auto offset{ptr - reinterpret_cast<const std::byte*>(base)};

    return &const_cast<T*>(base)[(offset / sizeof(T)) * sizeof(T)];
  }

  /** Get node's link (offset) from the base pointer and pointer to the node
   * 
   * @param[in] node Pointer to the node
   * 
   * @return Node's link (offset)
   */
  node_type::Link_type to_link(const node_type& node) noexcept {
    auto ptr = reinterpret_cast<const std::byte*>(&node);
    return ((ptr - reinterpret_cast<const std::byte*>(m_bounds.first)) / sizeof(T)) * sizeof(T);
  }

  const T* to_item(node_type::Link_type link) const noexcept {
    return &m_bounds.first[link];
  }

  T* to_item(node_type::Link_type link) noexcept {
    return &m_bounds.first[link];
  }

  node_pointer to_node(node_type::Link_type link) const noexcept {
    return to_node(m_bounds.first, link);
  }

  node_pointer to_node(node_type::Link_type link) noexcept {
    return to_node(m_bounds.first, link);
  }

  void push_front(node_type& node) noexcept {
    assert(m_bounds.first <= &node && &node < m_bounds.second);

    typename node_type::Link_type old_head_link;
    const auto new_node_link = node_type::to_link(to_item(node));

    do {
      old_head_link = m_head.load(std::memory_order_acquire);

      /* Set the new node's links */
      node.m_links.store(pack_links(old_head_link, node_type::NULL_PTR), std::memory_order_relaxed);

    } while (!m_head.compare_exchange_weak(old_head_link, new_node_link, std::memory_order_release));

    /* Update the old head's `prev` link, if it exists */
    if (old_head_link != node_type::NULL_PTR) {
      node_pointer old_head = to_node(old_head_link);
      const auto old_head_link = old_head->m_links.load(std::memory_order_acquire);
      const auto new_head_link = pack_links(node_type::unpack_links(old_head_link).first, new_node_link);

      old_head->m_links.compare_exchange_strong(old_head_link, new_head_link, std::memory_order_release);
    }

    /* Update tail if the list was empty */
    if (m_tail.load(std::memory_order_acquire) == node_type::NULL_PTR) {
      m_tail.store(new_node_link, std::memory_order_release);
    }
  }

  void push_back(node_type& node) noexcept {
    typename node_type::Link_type old_tail_link;
    const auto new_node_link = to_link(node);

    do {
      old_tail_link = m_tail.load(std::memory_order_acquire);

      /* Set the new node's links */
      node.m_links.store(old_tail_link, std::memory_order_relaxed);

    } while (!m_tail.compare_exchange_weak(old_tail_link, new_node_link, std::memory_order_release));

    /* Update the old tail's `next` link, if it exists */
    if (old_tail_link != node_type::NULL_PTR) {
      node_pointer old_tail = to_node(old_tail_link);
      auto old_tail_link = old_tail->m_links.load(std::memory_order_acquire);
      auto new_tail_link = pack_links(new_node_link, unpack_links(old_tail_link).second);

      old_tail->m_links.compare_exchange_strong(old_tail_link, new_tail_link, std::memory_order_release);
    }

    /* Update head if the list was empty */
      if (m_head.load(std::memory_order_acquire) == node_type::NULL_PTR) {
      m_head.store(new_node_link, std::memory_order_release);
    }
  }

  void insert_after(node_type& node, node_type& new_node) noexcept {
    const auto node_link = to_link(node);
    const auto new_node_link = to_link(new_node);

    uint64_t updated_node_link;
    typename node_type::Link_type next_link;

    do {
      node_link = node.m_links.load(std::memory_order_acquire);
      next_link = node_type::unpack_links(node_link).first;

      /* Set the new node's links */
      new_node.m_links.store(pack_links(next_link, node_link), std::memory_order_relaxed);

      /* Update node's next to point to the new node */
      updated_node_link = pack_links(new_node_link, node_link);
    } while (!node.m_links.compare_exchange_weak(node_link, updated_node_link, std::memory_order_release));

    /* Update the next node's prev link, if it exists */
    if (next_link != node_type::NULL_PTR) {
      uint64_t next_link;
      uint64_t updated_next_link;
      auto next_node = from_link(m_bounds.first, next_link);

      do {
        next_link = next_node->m_links.load(std::memory_order_acquire);
        updated_next_link = pack_links(next_link, new_node_link);
      } while (!next_node->m_links.compare_exchange_weak(next_link, updated_next_link, std::memory_order_release));
    }

    /* Update tail if needed */
    if (m_tail.load(std::memory_order_acquire) == next_link) {
      m_tail.store(new_node_link, std::memory_order_release);
    }
  }

  void insert_before(node_type& node, node_type& new_node) noexcept {
    const auto node_link = to_link(node);
    const auto new_node_link = to_link(new_node);

    uint64_t updated_node_link;
    typename node_type::Link_type prev_link;

    do {
      node_link = node.m_links.load(std::memory_order_acquire);
      prev_link = unpack_links(node_link).second;

      /* Set the new node's links */
      new_node.m_links.store(pack_links(node_link, prev_link), std::memory_order_relaxed);

      /* Update target's prev to point to the new node */
      updated_node_link = pack_links(node_link, new_node_link);
    } while (!node.m_links.compare_exchange_weak(node_link, updated_node_link, std::memory_order_release));

    /* Update the previous node's next link, if it exists */
    if (prev_link != node_type::NULL_PTR) {
      uint64_t prev_link;
      uint64_t updated_prev_link;
      const auto prev_node = node_type::from_link(m_bounds.first, prev_link);

      do {
        prev_link = prev_node->m_links.load(std::memory_order_acquire);
        updated_prev_link = pack_links(new_node_link, prev_link);
      } while (!prev_node->m_links.compare_exchange_weak(prev_link, updated_prev_link, std::memory_order_release));
    }

    /* Update head if needed */
    if (m_head.load(std::memory_order_acquire) == prev_link) {
      m_head.store(new_node_link, std::memory_order_release);
    }
  }

  template <typename Predicate>
  T *find(Predicate predicate) noexcept {
    auto curr_link = m_head.load(std::memory_order_acquire);
    auto node_link = node_type::unpack_links(curr_link).first;

    while (node_link != node_type::NULL_PTR) {
      auto node = node_type::to_node(node_link);

      /* Prefetch the next node for better performance */
      _mm_prefetch(reinterpret_cast<const char*>(&node->m_links), _MM_HINT_T0);

      if (Predicate(to_item(node))) {
        return to_item(node);
      }

      curr_link = node->m_links.load(std::memory_order_acquire).first;
      node_link = node_type::unpack_links(curr_link);
    }

    return nullptr;
  }

  iterator begin() noexcept {
    auto head = m_head.load(std::memory_order_acquire);
    return head != node_type::NULL_PTR ? iterator(m_bounds.first, to_node(head)) : end();
  }

  const_iterator begin() const noexcept {
    auto head = m_head.load(std::memory_order_acquire);
    return head != node_type::NULL_PTR ? const_iterator(m_bounds.first, to_node(head)) : end();
  }

  const_iterator cbegin() const noexcept {
    return begin();
  }

  iterator end() noexcept {
    return iterator(m_bounds.first, nullptr);
  }

  const_iterator end() const noexcept {
    return const_iterator(m_bounds.first, nullptr);
  }

  const_iterator cend() const noexcept {
    return end();
  }

  reverse_iterator rbegin() noexcept {
    return reverse_iterator(end());
  }

  const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  const_reverse_iterator crbegin() const noexcept {
    return rbegin();
  }

  reverse_iterator rend() noexcept {
    return reverse_iterator(begin());
  }

  const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }

  const_reverse_iterator crend() const noexcept {
    return rend();
  }

  std::pair<T*, T*> m_bounds{};
  std::atomic<typename node_type::Link_type> m_head{node_type::NULL_PTR};
  std::atomic<typename node_type::Link_type> m_tail{node_type::NULL_PTR};
};

} // namespace ut

