#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <optional>

namespace ut {

struct Iterator_invalidated : public std::runtime_error {
  explicit Iterator_invalidated(const char* msg) : std::runtime_error(msg) {}
};

struct Node {
  using Link_type = uint16_t;
  using Version_type = uint16_t;
  static constexpr auto NULL_PTR = std::numeric_limits<Link_type>::max();
  static constexpr auto NULL_LINK = std::numeric_limits<uint64_t>::max();
  static constexpr uint32_t MAX_RETRIES = 100;

  Node() = default;
  Node(Node&& rhs) noexcept : m_links(rhs.m_links.load(std::memory_order_relaxed)) {}
  Node(Link_type next, Link_type prev, Version_type version) noexcept;

  Node& operator=(Node&& rhs) noexcept {
    m_links.store(rhs.m_links.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  bool is_null() const noexcept {
    return m_links.load(std::memory_order_relaxed) == NULL_LINK;
  }

  void invalidate() noexcept {
    m_links.store(NULL_LINK, std::memory_order_relaxed);
  }

  /** Atomic links storage: version (16-bit) | next (16-bit) | prev_version (16-bit) | prev (16-bit) */
  std::atomic<uint64_t> m_links{NULL_LINK};
};

inline constexpr uint64_t pack_links(Node::Link_type next, Node::Link_type prev, Node::Version_type next_version, Node::Version_type prev_version) noexcept {
  return (static_cast<uint64_t>(next_version) << 48) |
         (static_cast<uint64_t>(next) << 32) |
         (static_cast<uint64_t>(prev_version) << 16) |
         static_cast<uint64_t>(prev);
}

struct Link_pack {
  Node::Link_type next;
  Node::Link_type prev;
  Node::Version_type next_version;
  Node::Version_type prev_version;
};

inline Link_pack unpack_links(uint64_t links) noexcept {
  return {
    static_cast<Node::Link_type>((links >> 32) & 0xFFFF),
    static_cast<Node::Link_type>(links & 0xFFFF),
    static_cast<Node::Version_type>((links >> 48) & 0xFFFF),
    static_cast<Node::Version_type>((links >> 16) & 0xFFFF)
  };
}

template<typename T, auto N, bool IsConst = false>
struct List_iterator {
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = std::conditional_t<IsConst, const T*, T*>;
  using reference = std::conditional_t<IsConst, const T&, T&>;
  using iterator_category = std::bidirectional_iterator_tag;

  using result_type = typename std::invoke_result_t<decltype(N), T>;
  using node_type = typename std::remove_reference<result_type>::type;
  using node_pointer = std::conditional_t<IsConst, const node_type*, node_type*>;

  List_iterator() = default;

  template<bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
  List_iterator(const List_iterator<T, N, WasConst>& rhs) noexcept
    : m_base(rhs.m_base),
      m_prev(rhs.m_prev),
      m_current(rhs.m_current) {}

  List_iterator(pointer base, node_pointer current, node_pointer prev) noexcept
    : m_base(base),
      m_prev(prev),
      m_current(current) {}

  [[nodiscard]] inline pointer to_item(node_pointer node) const noexcept {
    if (!node) return nullptr;
    auto ptr = reinterpret_cast<const std::byte*>(node);
    const auto offset = ptr - reinterpret_cast<const std::byte*>(m_base);
    return &const_cast<pointer>(m_base)[offset / sizeof(T)];
  }

  [[nodiscard]] inline node_pointer to_node(typename node_type::Link_type link) const noexcept {
    if (link == node_type::NULL_PTR) return nullptr;
    auto base = const_cast<T*>(m_base);
    auto& node = (base[link].*N)();
    return const_cast<node_pointer>(&node);
  }

  [[nodiscard]] inline bool operator==(const List_iterator& rhs) const noexcept {
    return m_current == rhs.m_current;
  }

  [[nodiscard]] inline bool operator!=(const List_iterator& rhs) const noexcept {
    return !(*this == rhs);
  }

  [[nodiscard]] inline reference operator*() const noexcept {
    return *operator->();
  }

  [[nodiscard]] inline pointer operator->() const noexcept {
    return to_item(m_current);
  }

  List_iterator& operator++() {
    if (!m_current){
      return *this;
    }

    uint32_t retries{};
    auto current_links = unpack_links(m_current->m_links.load(std::memory_order_acquire));
    node_pointer next{to_node(current_links.next)};

    /* Validate current node hasn't been removed */
    if (to_node(current_links.prev) != m_prev) [[unlikely]] {
      while (m_current != nullptr && to_node(current_links.prev) != m_prev && retries++ < node_type::MAX_RETRIES) [[likely]] {
        m_current = to_node(current_links.next);
        if (m_current != nullptr) [[likely]] {
          current_links = unpack_links(m_current->m_links.load(std::memory_order_acquire));
          m_prev = to_node(current_links.prev);
        }
      }

      if (retries >= node_type::MAX_RETRIES) {
        throw Iterator_invalidated("Iterator invalidated by concurrent modifications");
      }
    }

    m_prev = m_current;
    m_current = next;
    return *this;
  }

  List_iterator operator++(int) noexcept {
    List_iterator tmp = *this;
    ++(*this);
    return tmp;
  }

  List_iterator& operator--() {
    if (!m_prev) return *this;

    uint32_t retries = 0;
    auto prev_links = unpack_links(m_prev->m_links.load(std::memory_order_acquire));
    auto prev = to_node(prev_links.prev);

    /* Validate prev node hasn't been removed */
    if (to_node(prev_links.next) != m_current) [[unlikely]] {
      while (m_prev != nullptr && to_node(prev_links.next) != m_current && retries++ < node_type::MAX_RETRIES) [[likely]] {
        m_prev = to_node(prev_links.prev);
        if (m_prev != nullptr) [[likely]] {
          prev_links = unpack_links(m_prev->m_links.load(std::memory_order_acquire));
          m_current = to_node(prev_links.next);
        }
      }

      if (retries >= node_type::MAX_RETRIES) {
        throw Iterator_invalidated("Iterator invalidated by concurrent modifications");
      }
    }

    m_current = m_prev;
    m_prev = prev;
    return *this;
  }

  List_iterator operator--(int) noexcept {
    List_iterator tmp = *this;
    --(*this);
    return tmp;
  }

  pointer m_base{};
  node_pointer m_prev{};
  node_pointer m_current{};
};

template <typename T, auto N>
struct List {
  using iterator = List_iterator<T, N, false>;
  using const_iterator = List_iterator<T, N, true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  using value_type = T;
  using node_type = std::remove_reference_t<typename std::invoke_result_t<decltype(N), T>>;
  using node_pointer = node_type*;
  using item_type = T;
  using item_pointer = item_type*;
  using item_reference = item_type&;

  List(item_pointer base, item_pointer end) noexcept : m_bounds(base, end) {
    assert(base <= end);
    assert(base != nullptr);
  }

  [[nodiscard]] static item_pointer to_item(const item_pointer base, const node_type& node) noexcept {
    auto ptr = reinterpret_cast<const std::byte*>(&node);
    const auto offset{ptr - reinterpret_cast<const std::byte*>(base)};
    return &const_cast<item_pointer>(base)[offset / sizeof(T)];
  }

  /* Utility methods */
  [[nodiscard]] static node_pointer to_node(const item_pointer base, typename node_type::Link_type link) noexcept {
    if (link == node_type::NULL_PTR) [[unlikely]] {
      return nullptr;
    } else [[likely]] {
      auto& node = (base[link].*N)();
      return const_cast<node_pointer>(&node);
    }
  }

  [[nodiscard]] node_pointer to_node(typename node_type::Link_type link) const noexcept {
    if (link == node_type::NULL_PTR) [[unlikely]] {
      return nullptr;
    } else [[likely]] {
      return to_node(const_cast<item_pointer>(m_bounds.first), link);
    }
  }

  [[nodiscard]] typename node_type::Link_type to_link(const node_type& node) const noexcept {
    auto ptr = reinterpret_cast<const std::byte*>(&node);
    return ((ptr - reinterpret_cast<const std::byte*>(m_bounds.first)) / sizeof(T));
  }
  [[nodiscard]] item_pointer to_item(const node_type& node) const noexcept {
    return to_item(const_cast<item_pointer>(m_bounds.first), node);
  }

  [[nodiscard]] item_pointer to_item(typename node_type::Link_type link) const noexcept {
    assert(link != node_type::NULL_PTR);
    return &const_cast<item_pointer>(m_bounds.first)[link];
  }

  item_pointer remove(item_reference item) noexcept {
    uint32_t retries{};
    auto& node = (item.*N)();

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      auto node_links = node.m_links.load(std::memory_order_acquire);
      auto link_data = unpack_links(node_links);

      auto prev_node = to_node(link_data.prev);
      auto next_node = to_node(link_data.next);
      auto node_link = to_link(node);

      /* Handle head removal */
      if (link_data.prev == Node::NULL_PTR && !m_head.compare_exchange_strong(node_link, link_data.next, std::memory_order_acq_rel)) [[unlikely]] {
        continue;
      }

      /* Handle tail removal */
      if (link_data.next == Node::NULL_PTR && !m_tail.compare_exchange_strong(node_link, link_data.prev, std::memory_order_acq_rel)) [[unlikely]] {
        continue;
      }

      /* Mark node as removed */
      if (!node.m_links.compare_exchange_strong(node_links, Node::NULL_LINK, std::memory_order_acq_rel)) [[unlikely]] {
        continue;
      }

      /* Update adjacent nodes */
      bool success{true};

      if (prev_node != nullptr) [[likely]] {
        uint32_t prev_retries{};
        uint64_t prev_links;
        Link_pack prev_link_data;

        do {
          if (prev_retries++ >= Node::MAX_RETRIES) [[unlikely]] {
            success = false;
            break;
          }

          prev_link_data = unpack_links(prev_links = prev_node->m_links.load(std::memory_order_acquire));

          if (prev_link_data.next != to_link(node)) [[unlikely]] {
            success = false;
            break;
          }

        } while (!prev_node->m_links.compare_exchange_weak(prev_links,
                  pack_links(link_data.next, prev_link_data.prev,
                            prev_link_data.next_version + 1, prev_link_data.prev_version),
                  std::memory_order_acq_rel));
      }

      if (next_node != nullptr && success) [[likely]] {
        uint32_t next_retries{};
        uint64_t next_links;
        Link_pack next_link_data;

        do {
          if (next_retries++ >= Node::MAX_RETRIES) [[unlikely]] {
            success = false;
            break;
          }

          next_link_data = unpack_links(next_links = next_node->m_links.load(std::memory_order_acquire));

          if (next_link_data.prev != to_link(node)) [[unlikely]] {
            success = false;
            break;
          }

        } while (!next_node->m_links.compare_exchange_weak(next_links,
                  pack_links(next_link_data.next, link_data.prev,
                            next_link_data.next_version, next_link_data.prev_version + 1),
                  std::memory_order_acq_rel));
      }

      if (success) {
        node.invalidate();
        return to_item(node);
      }

      /* If we failed to update adjacent nodes, try to restore the node */
      node.m_links.store(node_links, std::memory_order_release);
    }

    /* Failed to remove after max retries */
    return nullptr;
  }

  bool push_front(item_reference item) noexcept {
    uint32_t retries{};
    auto& node = (item.*N)();

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      typename node_type::Link_type old_head_link = m_head.load(std::memory_order_acquire);
      auto new_node_link = to_link(node);

      node.m_links.store(pack_links(old_head_link, Node::NULL_PTR, 0, 0), std::memory_order_relaxed);

      if (m_head.compare_exchange_strong(old_head_link, new_node_link, std::memory_order_acq_rel)) [[likely]] {

        if (old_head_link != Node::NULL_PTR) [[likely]] {
          uint32_t head_retries{};
          uint64_t old_head_links;
          auto old_head = to_node(old_head_link);
          Link_pack old_head_data;

          do {
            if (head_retries++ >= Node::MAX_RETRIES) {
              /* Failed to update old head, try to restore state */
              m_head.store(old_head_link, std::memory_order_release);
              node.invalidate();
              return false;
            }

            old_head_data = unpack_links(old_head_links = old_head->m_links.load(std::memory_order_acquire));

          } while (!old_head->m_links.compare_exchange_weak(old_head_links,
                    pack_links(old_head_data.next, new_node_link,
                              old_head_data.next_version, old_head_data.prev_version + 1),
                    std::memory_order_acq_rel));
        }

        typename node_type::Link_type expected_tail = Node::NULL_PTR;
        m_tail.compare_exchange_strong(expected_tail, new_node_link, std::memory_order_acq_rel);

        return true;
      }
    }

    node.invalidate();
    return false;
  }

  bool push_back(item_reference item) noexcept {
    uint32_t retries{};
    auto& node = (item.*N)();

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      typename node_type::Link_type old_tail_link = m_tail.load(std::memory_order_acquire);
      auto new_node_link = to_link(node);

      node.m_links.store(pack_links(Node::NULL_PTR, old_tail_link, 0, 0), std::memory_order_relaxed);

      if (m_tail.compare_exchange_strong(old_tail_link, new_node_link, std::memory_order_acq_rel)) [[likely]] {

        if (old_tail_link != Node::NULL_PTR) [[likely]] {
          uint64_t old_tail_links;
          uint32_t tail_retries{};
          auto old_tail = to_node(old_tail_link);
          Link_pack old_tail_data;

          do {
            if (tail_retries++ >= Node::MAX_RETRIES) {
              m_tail.store(old_tail_link, std::memory_order_release);
              node.invalidate();
              return false;
            }

            old_tail_data = unpack_links(old_tail_links = old_tail->m_links.load(std::memory_order_acquire));

          } while (!old_tail->m_links.compare_exchange_weak(old_tail_links,
                    pack_links(new_node_link, old_tail_data.prev,
                              old_tail_data.next_version + 1, old_tail_data.prev_version),
                    std::memory_order_acq_rel));
        }

        typename node_type::Link_type expected_head = Node::NULL_PTR;
        m_head.compare_exchange_strong(expected_head, new_node_link, std::memory_order_acq_rel);

        return true;
      }
    }

    node.invalidate();
    return false;
  }

  bool insert_after(item_reference item, item_reference new_item) noexcept {
    auto& node = (item.*N)();

    if (node.is_null()) {
      return false;
    }

    auto& new_node = (new_item.*N)();
    uint32_t retries{};

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      uint64_t node_links = node.m_links.load(std::memory_order_acquire);
      auto link_data = unpack_links(node_links);
      auto new_node_link = to_link(new_node);

      if (node_links == Node::NULL_LINK) [[unlikely]] {
        /* Node was removed */
        new_node.invalidate();
        return false;
      }

      /* Set new node's links */
      new_node.m_links.store(pack_links(link_data.next, to_link(node), 0, 0), std::memory_order_relaxed);

      if (node.m_links.compare_exchange_strong(node_links,
            pack_links(new_node_link, link_data.prev,
                      link_data.next_version + 1, link_data.prev_version),
            std::memory_order_acq_rel)) [[likely]] {

        /* Update next node's prev link if it exists */
        if (link_data.next != Node::NULL_PTR) {
          uint64_t next_links;
          uint32_t next_retries{};
          auto next_node = to_node(link_data.next);
          Link_pack next_link_data;

          do {
            if (next_retries++ >= Node::MAX_RETRIES) {
              /* Restore original node links */
              node.m_links.store(node_links, std::memory_order_release);
              new_node.invalidate();
              return false;
            }

            next_link_data = unpack_links(next_links = next_node->m_links.load(std::memory_order_acquire));

            if (next_link_data.prev != to_link(node)) [[unlikely]] {
              /* Next node was modified, restore and retry */
              node.m_links.store(node_links, std::memory_order_release);
              break;
            }

          } while (!next_node->m_links.compare_exchange_weak(next_links,
                    pack_links(next_link_data.next, new_node_link,
                              next_link_data.next_version, next_link_data.prev_version + 1),
                    std::memory_order_acq_rel));
        } else {
          /* Update tail if necessary */
          auto expected_tail = to_link(node);
          m_tail.compare_exchange_strong(expected_tail, new_node_link, std::memory_order_acq_rel);
        }

        return true;
      }
    }

    new_node.invalidate();
    return false;
  }

  bool insert_before(item_reference item, item_reference new_item) noexcept {
    uint32_t retries{};
    auto& node = (item.*N)();
    auto& new_node = (new_item.*N)();

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      uint64_t node_links = node.m_links.load(std::memory_order_acquire);
      auto link_data = unpack_links(node_links);
      auto new_node_link = to_link(new_node);

      if (node_links == Node::NULL_LINK) [[unlikely]] {
        /* Node was removed */
        new_node.invalidate();
        return false;
      }

      /* Set new node's links */
      new_node.m_links.store(pack_links(to_link(node), link_data.prev, 0, 0), std::memory_order_relaxed);

      if (node.m_links.compare_exchange_strong(node_links,
            pack_links(link_data.next, new_node_link,
                      link_data.next_version, link_data.prev_version + 1),
            std::memory_order_acq_rel)) [[likely]] {

        /* Update prev node's next link if it exists */
        if (link_data.prev != Node::NULL_PTR) [[likely]] {
          uint64_t prev_links;
          uint32_t prev_retries{};
          auto prev_node = to_node(link_data.prev);
          Link_pack prev_link_data;

          do {
            if (prev_retries++ >= Node::MAX_RETRIES) {
              /* Restore original node links */
              node.m_links.store(node_links, std::memory_order_release);
              new_node.invalidate();
              return false;
            }

            prev_link_data = unpack_links(prev_links = prev_node->m_links.load(std::memory_order_acquire));

            if (prev_link_data.next != to_link(node)) [[unlikely]] {
              /* Prev node was modified, restore and retry */
              node.m_links.store(node_links, std::memory_order_release);
              break;
            }

          } while (!prev_node->m_links.compare_exchange_weak(prev_links,
                    pack_links(new_node_link, prev_link_data.prev,
                              prev_link_data.next_version + 1, prev_link_data.prev_version),
                    std::memory_order_acq_rel));
        } else {
          /* Update head if necessary */
          auto expected_head = to_link(node);
          m_head.compare_exchange_strong(expected_head, new_node_link, std::memory_order_acq_rel);
        }

        return true;
      }
    }

    new_node.invalidate();
    return false;
  }

  template <typename Predicate>
  [[nodiscard]] item_pointer find(Predicate predicate) noexcept {
    uint32_t retries{};
    typename node_type::Link_type current = m_head.load(std::memory_order_acquire);

    while (current != Node::NULL_PTR && retries++ < Node::MAX_RETRIES) [[likely]] {
      auto node = to_node(current);
      auto item = to_item(*node);

      if (predicate(item)) [[unlikely]] {
        /* Verify node is still in the list */
        if (!node->is_null()) {
          return item;
        }
      }

      auto links = node->m_links.load(std::memory_order_acquire);

      if (links == Node::NULL_LINK) [[unlikely]] {
        /* Node was removed, try to recover from head */
        current = m_head.load(std::memory_order_acquire);
        retries++;
        continue;
      }

      current = unpack_links(links).next;
    }

    return nullptr;
  }

  [[nodiscard]] item_pointer pop_front() noexcept {
    if (auto node = m_head.load(std::memory_order_acquire); node != Node::NULL_PTR) [[unlikely]] {
      return remove(*to_item(node));
    } else {
      return nullptr;
    }
  }

  [[nodiscard]] item_pointer pop_back() noexcept {
    if (auto node = m_tail.load(std::memory_order_acquire); node != Node::NULL_PTR) [[unlikely]] {
      return remove(*to_item(node));
    } else {
      return nullptr;
    }
  }

  [[nodiscard]] iterator begin() noexcept {
    const auto head = m_head.load(std::memory_order_acquire);

    if (head != Node::NULL_PTR) [[likely]] {
      auto node = to_node(head);
      return iterator(m_bounds.first, node, nullptr);
    }
    return end();
  }

  [[nodiscard]] const_iterator begin() const noexcept {
    const auto head = m_head.load(std::memory_order_acquire);

    if (head != Node::NULL_PTR) [[likely]] {
      auto node = to_node(head);
      return const_iterator(m_bounds.first, node, nullptr);
    }
    return end();
  }

  [[nodiscard]] iterator end() noexcept {
    const auto tail = m_tail.load(std::memory_order_acquire);
    auto node = tail != Node::NULL_PTR ? to_node(tail) : nullptr;
    return iterator(m_bounds.first, nullptr, node);
  }

  [[nodiscard]] const_iterator end() const noexcept {
    const auto tail = m_tail.load(std::memory_order_acquire);
    auto node = tail != Node::NULL_PTR ? to_node(tail) : nullptr;
    return const_iterator(m_bounds.first, nullptr, node);
  }

  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator(end());
  }

  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator(begin());
  }

  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }

#if UT_DEBUG
  /* Validation helper */
  bool validate_node_links(const node_type& node) const noexcept {
    auto links = node.m_links.load(std::memory_order_acquire);

    if (links == Node::NULL_LINK) [[likely]] {
      return true;  // Removed nodes are valid
    }

    auto link_data = unpack_links(links);

    /* Check next pointer consistency */
    if (link_data.next != Node::NULL_PTR) [[likely]] {
      auto next_node = to_node(link_data.next);

      if (!next_node || next_node->is_null()) {
        return false;
      }

      auto next_links = next_node->m_links.load(std::memory_order_acquire);

      if (next_links == Node::NULL_LINK) [[unlikely]] {
        return false;
      }

      auto next_link_data = unpack_links(next_links);

      if (next_link_data.prev != to_link(node)) [[unlikely]] {
        return false;
      }
    }

    /* Check prev pointer consistency */
    if (link_data.prev != Node::NULL_PTR) [[likely]] {
      auto prev_node = to_node(link_data.prev);

      if (!prev_node || prev_node->is_null()) [[unlikely]] {
        return false;
      }

      auto prev_links = prev_node->m_links.load(std::memory_order_acquire);

      if (prev_links == Node::NULL_LINK) [[unlikely]] {
        return false;
      }

      auto prev_link_data = unpack_links(prev_links);

      if (prev_link_data.next != to_link(node)) [[unlikely]] {
        return false;
      }
    }

    return true;
  }
#endif // UT_DEBUG

private:
  std::pair<item_pointer, item_pointer> m_bounds{};
  std::atomic<typename node_type::Link_type> m_head{node_type::NULL_PTR};
  std::atomic<typename node_type::Link_type> m_tail{node_type::NULL_PTR};
};

} // namespace ut
