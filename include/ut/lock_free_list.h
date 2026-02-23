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
  using Link_type = uint32_t;
  using Version_type = uint8_t;

  // Configuration: Total bits for version counters (2 bits per version)
  static constexpr uint32_t VERSION_BITS_PER_LINK = 2;
  static constexpr uint32_t TOTAL_VERSION_BITS = VERSION_BITS_PER_LINK * 2;  // next_version + prev_version

  // Derived constants
  static constexpr uint32_t LINK_BITS = (64 - TOTAL_VERSION_BITS) / 2;  // 30 bits per link
  static constexpr auto VERSION_MASK = (1u << VERSION_BITS_PER_LINK) - 1;  // 0x3 for 2 bits
  static constexpr auto NULL_PTR = (1u << LINK_BITS) - 1;  // Max value for link bits
  static constexpr auto DELETING_MARK = NULL_PTR - 1;  // Marks node as being deleted
  static constexpr auto NULL_LINK = std::numeric_limits<uint64_t>::max();
  static constexpr uint32_t MAX_RETRIES = 100;

  Node() = default;
  Node(Node&& rhs) noexcept : m_links(rhs.m_links.load(std::memory_order_relaxed)) {}
  Node(Link_type next, Link_type prev, Version_type version) noexcept;

  Node& operator=(Node&& rhs) noexcept {
    m_links.store(rhs.m_links.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  [[nodiscard]] bool is_null() const noexcept {
    return m_links.load(std::memory_order_relaxed) == NULL_LINK;
  }

  [[nodiscard]] bool is_deleting() const noexcept {
    auto links = m_links.load(std::memory_order_acquire);
    if (links == NULL_LINK) return false;
    auto next = static_cast<Link_type>((links >> (VERSION_BITS_PER_LINK + LINK_BITS + VERSION_BITS_PER_LINK)) & ((1u << LINK_BITS) - 1));
    return next == DELETING_MARK;
  }

  [[nodiscard]] bool is_removed_or_deleting() const noexcept {
    auto links = m_links.load(std::memory_order_acquire);
    if (links == NULL_LINK) return true;
    auto next = static_cast<Link_type>((links >> (VERSION_BITS_PER_LINK + LINK_BITS + VERSION_BITS_PER_LINK)) & ((1u << LINK_BITS) - 1));
    return next == DELETING_MARK;
  }

  void invalidate() noexcept {
    m_links.store(NULL_LINK, std::memory_order_relaxed);
  }

  /** Atomic links storage layout (64 bits total):
   *  - next link (LINK_BITS)
   *  - next_version (VERSION_BITS_PER_LINK)
   *  - prev link (LINK_BITS)
   *  - prev_version (VERSION_BITS_PER_LINK)
   */
  std::atomic<uint64_t> m_links{NULL_LINK};
};

[[nodiscard]] inline constexpr uint64_t pack_links(Node::Link_type next, Node::Link_type prev, Node::Version_type next_version, Node::Version_type prev_version) noexcept {
  constexpr uint32_t LINK_MASK = (1u << Node::LINK_BITS) - 1;
  constexpr uint32_t PREV_VERSION_BITS = Node::VERSION_BITS_PER_LINK;
  constexpr uint32_t PREV_LINK_SHIFT = PREV_VERSION_BITS;
  constexpr uint32_t NEXT_VERSION_SHIFT = PREV_VERSION_BITS + Node::LINK_BITS;
  constexpr uint32_t NEXT_LINK_SHIFT = PREV_VERSION_BITS + Node::LINK_BITS + Node::VERSION_BITS_PER_LINK;

  return (static_cast<uint64_t>(next & LINK_MASK) << NEXT_LINK_SHIFT) |
         (static_cast<uint64_t>(next_version & Node::VERSION_MASK) << NEXT_VERSION_SHIFT) |
         (static_cast<uint64_t>(prev & LINK_MASK) << PREV_LINK_SHIFT) |
         static_cast<uint64_t>(prev_version & Node::VERSION_MASK);
}

struct Link_pack {
  Node::Link_type next;
  Node::Link_type prev;
  Node::Version_type next_version;
  Node::Version_type prev_version;

  [[nodiscard]] bool is_deleting() const noexcept {
    return next == Node::DELETING_MARK;
  }
};

[[nodiscard]] inline Link_pack unpack_links(uint64_t links) noexcept {
  constexpr uint32_t LINK_MASK = (1u << Node::LINK_BITS) - 1;
  constexpr uint32_t PREV_VERSION_BITS = Node::VERSION_BITS_PER_LINK;
  constexpr uint32_t PREV_LINK_SHIFT = PREV_VERSION_BITS;
  constexpr uint32_t NEXT_VERSION_SHIFT = PREV_VERSION_BITS + Node::LINK_BITS;
  constexpr uint32_t NEXT_LINK_SHIFT = PREV_VERSION_BITS + Node::LINK_BITS + Node::VERSION_BITS_PER_LINK;

  return {
    static_cast<Node::Link_type>((links >> NEXT_LINK_SHIFT) & LINK_MASK),
    static_cast<Node::Link_type>((links >> PREV_LINK_SHIFT) & LINK_MASK),
    static_cast<Node::Version_type>((links >> NEXT_VERSION_SHIFT) & Node::VERSION_MASK),
    static_cast<Node::Version_type>(links & Node::VERSION_MASK)
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
    if (link == node_type::NULL_PTR || link == node_type::DELETING_MARK) return nullptr;
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
    uint64_t raw_links = m_current->m_links.load(std::memory_order_acquire);

    /* Handle deleted or deleting nodes */
    if (raw_links == node_type::NULL_LINK) [[unlikely]] {
      /* Current node was fully removed - move to end */
      m_prev = m_current;
      m_current = nullptr;
      return *this;
    }

    auto current_links = unpack_links(raw_links);
    node_pointer next{to_node(current_links.next)};

    /* Validate current node hasn't been removed */
    if (to_node(current_links.prev) != m_prev) [[unlikely]] {
      while (m_current != nullptr && to_node(current_links.prev) != m_prev && retries++ < node_type::MAX_RETRIES) [[likely]] {
        m_current = to_node(current_links.next);
        if (m_current != nullptr) [[likely]] {
          raw_links = m_current->m_links.load(std::memory_order_acquire);
          if (raw_links == node_type::NULL_LINK) [[unlikely]] {
            m_prev = m_current;
            m_current = nullptr;
            return *this;
          }
          current_links = unpack_links(raw_links);
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

  [[nodiscard]] List_iterator operator++(int) noexcept {
    List_iterator tmp = *this;
    ++(*this);
    return tmp;
  }

  List_iterator& operator--() {
    if (!m_prev) return *this;

    uint32_t retries = 0;
    uint64_t raw_links = m_prev->m_links.load(std::memory_order_acquire);

    /* Handle deleted nodes */
    if (raw_links == node_type::NULL_LINK) [[unlikely]] {
      m_prev = nullptr;
      return *this;
    }

    auto prev_links = unpack_links(raw_links);

    /* Handle node being deleted - move past it */
    while (prev_links.is_deleting() && m_prev != nullptr && retries++ < node_type::MAX_RETRIES) [[unlikely]] {
      m_prev = to_node(prev_links.prev);
      if (!m_prev) return *this;
      raw_links = m_prev->m_links.load(std::memory_order_acquire);
      if (raw_links == node_type::NULL_LINK) {
        m_prev = nullptr;
        return *this;
      }
      prev_links = unpack_links(raw_links);
    }

    if (retries >= node_type::MAX_RETRIES) {
      throw Iterator_invalidated("Iterator invalidated by concurrent modifications");
    }

    auto prev = to_node(prev_links.prev);

    /* Cycle detection: if prev equals current m_prev, we'd loop forever */
    if (prev == m_prev) [[unlikely]] {
      m_prev = nullptr;
      return *this;
    }

    m_current = m_prev;
    m_prev = prev;
    return *this;
  }

  [[nodiscard]] List_iterator operator--(int) noexcept {
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
    if (link == node_type::NULL_PTR || link == node_type::DELETING_MARK) [[unlikely]] {
      return nullptr;
    } else [[likely]] {
      auto& node = (base[link].*N)();
      return const_cast<node_pointer>(&node);
    }
  }

  [[nodiscard]] node_pointer to_node(typename node_type::Link_type link) const noexcept {
    if (link == node_type::NULL_PTR || link == node_type::DELETING_MARK) [[unlikely]] {
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

  [[nodiscard]] item_pointer remove(item_reference item) noexcept {
    uint32_t retries{};
    auto& node = (item.*N)();

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      auto node_links = node.m_links.load(std::memory_order_acquire);

      /* Check if already removed or being deleted */
      if (node_links == Node::NULL_LINK) [[unlikely]] {
        return nullptr;  /* Already removed */
      }

      auto link_data = unpack_links(node_links);

      /* Check if already being deleted by another thread */
      if (link_data.is_deleting()) [[unlikely]] {
        return nullptr;  /* Another thread is deleting this */
      }

      /* Save original values before any modifications */
      auto original_prev = link_data.prev;
      auto original_next = link_data.next;
      auto prev_node = to_node(original_prev);
      auto next_node = to_node(original_next);
      auto node_link = to_link(node);

      /* Step 1: Mark node as "deleting" - this is the commit point */
      /* We encode the original_next in the prev field since we need it later */
      /* Use DELETING_MARK as next to indicate deletion, keep prev as-is */
      uint64_t deleting_links = pack_links(Node::DELETING_MARK, original_prev,
                                           (link_data.next_version + 1) & Node::VERSION_MASK,
                                           link_data.prev_version);

      if (!node.m_links.compare_exchange_strong(node_links, deleting_links, std::memory_order_acq_rel)) [[unlikely]] {
        continue;  /* Node was modified, retry */
      }

      /* Node is now marked as deleting - we own this deletion */
      /* Decrement size immediately since deletion is committed */
      m_size.fetch_sub(1, std::memory_order_relaxed);

      /* Step 2: Update head if this was the head */
      if (original_prev == Node::NULL_PTR) {
        typename node_type::Link_type expected_head = node_link;
        while (!m_head.compare_exchange_weak(expected_head, original_next, std::memory_order_acq_rel)) {
          if (expected_head != node_link) break;  /* Head already updated */
        }
      }

      /* Step 3: Update tail if this was the tail */
      if (original_next == Node::NULL_PTR) {
        typename node_type::Link_type expected_tail = node_link;
        while (!m_tail.compare_exchange_weak(expected_tail, original_prev, std::memory_order_acq_rel)) {
          if (expected_tail != node_link) break;  /* Tail already updated */
        }
      }

      /* Step 4: Update prev_node->next to skip this node */
      if (prev_node != nullptr) [[likely]] {
        uint32_t prev_retries{};
        uint64_t prev_links;
        Link_pack prev_link_data;

        do {
          if (prev_retries++ >= Node::MAX_RETRIES) [[unlikely]] {
            break;  /* Give up but continue - deletion is committed */
          }

          prev_link_data = unpack_links(prev_links = prev_node->m_links.load(std::memory_order_acquire));

          if (prev_links == Node::NULL_LINK || prev_link_data.is_deleting()) [[unlikely]] {
            break;  /* prev_node also being deleted */
          }

          /* Check if already updated */
          if (prev_link_data.next != node_link) [[unlikely]] {
            break;  /* Already updated or something else happened */
          }

        } while (!prev_node->m_links.compare_exchange_weak(prev_links,
                  pack_links(original_next, prev_link_data.prev,
                            (prev_link_data.next_version + 1) & Node::VERSION_MASK, prev_link_data.prev_version),
                  std::memory_order_acq_rel));
      }

      /* Step 5: Update next_node->prev to skip this node */
      if (next_node != nullptr) [[likely]] {
        uint32_t next_retries{};
        uint64_t next_links;
        Link_pack next_link_data;

        do {
          if (next_retries++ >= Node::MAX_RETRIES) [[unlikely]] {
            break;  /* Give up but continue - deletion is committed */
          }

          next_link_data = unpack_links(next_links = next_node->m_links.load(std::memory_order_acquire));

          if (next_links == Node::NULL_LINK || next_link_data.is_deleting()) [[unlikely]] {
            break;  /* next_node also being deleted */
          }

          /* Check if already updated */
          if (next_link_data.prev != node_link) [[unlikely]] {
            break;  /* Already updated or something else happened */
          }

        } while (!next_node->m_links.compare_exchange_weak(next_links,
                  pack_links(next_link_data.next, original_prev,
                            next_link_data.next_version, (next_link_data.prev_version + 1) & Node::VERSION_MASK),
                  std::memory_order_acq_rel));
      }

      /* Step 6: Finalize - mark node as fully removed */
      node.m_links.store(Node::NULL_LINK, std::memory_order_release);

      return to_item(node);
    }

    /* Failed to remove after max retries */
    return nullptr;
  }

  [[nodiscard]] bool push_front(item_reference item) noexcept {
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

            if (old_head_links == Node::NULL_LINK) [[unlikely]] {
              /* Old head was removed, try to restore state */
              m_head.store(old_head_link, std::memory_order_release);
              node.invalidate();
              return false;
            }

          } while (!old_head->m_links.compare_exchange_weak(old_head_links,
                    pack_links(old_head_data.next, new_node_link,
                              old_head_data.next_version, (old_head_data.prev_version + 1) & Node::VERSION_MASK),
                    std::memory_order_acq_rel));
        }

        typename node_type::Link_type expected_tail = Node::NULL_PTR;
        m_tail.compare_exchange_strong(expected_tail, new_node_link, std::memory_order_acq_rel);

        m_size.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }

    node.invalidate();
    return false;
  }

  [[nodiscard]] bool push_back(item_reference item) noexcept {
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

            if (old_tail_links == Node::NULL_LINK) [[unlikely]] {
              /* Old tail was removed, try to restore state */
              m_tail.store(old_tail_link, std::memory_order_release);
              node.invalidate();
              return false;
            }

          } while (!old_tail->m_links.compare_exchange_weak(old_tail_links,
                    pack_links(new_node_link, old_tail_data.prev,
                              (old_tail_data.next_version + 1) & Node::VERSION_MASK, old_tail_data.prev_version),
                    std::memory_order_acq_rel));
        }

        typename node_type::Link_type expected_head = Node::NULL_PTR;
        m_head.compare_exchange_strong(expected_head, new_node_link, std::memory_order_acq_rel);

        m_size.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }

    node.invalidate();
    return false;
  }

  [[nodiscard]] bool insert_after(item_reference item, item_reference new_item) noexcept {
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

      if (node_links == Node::NULL_LINK || link_data.is_deleting()) [[unlikely]] {
        /* Node was removed or is being deleted */
        new_node.invalidate();
        return false;
      }

      /* Set new node's links */
      new_node.m_links.store(pack_links(link_data.next, to_link(node), 0, 0), std::memory_order_relaxed);

      if (node.m_links.compare_exchange_strong(node_links,
            pack_links(new_node_link, link_data.prev,
                      (link_data.next_version + 1) & Node::VERSION_MASK, link_data.prev_version),
            std::memory_order_acq_rel)) [[likely]] {

        /* Update next node's prev link if it exists */
        bool next_updated = true;
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

            if (next_links == Node::NULL_LINK || next_link_data.is_deleting()) [[unlikely]] {
              /* Next node was removed or being deleted, restore and retry outer loop */
              node.m_links.store(node_links, std::memory_order_release);
              next_updated = false;
              break;
            }

            if (next_link_data.prev != to_link(node)) [[unlikely]] {
              /* Next node was modified, restore and retry outer loop */
              node.m_links.store(node_links, std::memory_order_release);
              next_updated = false;
              break;
            }

          } while (!next_node->m_links.compare_exchange_weak(next_links,
                    pack_links(next_link_data.next, new_node_link,
                              next_link_data.next_version, (next_link_data.prev_version + 1) & Node::VERSION_MASK),
                    std::memory_order_acq_rel));

          if (!next_updated) {
            continue;  /* Retry outer loop */
          }
        } else {
          /* Update tail if necessary */
          auto expected_tail = to_link(node);
          m_tail.compare_exchange_strong(expected_tail, new_node_link, std::memory_order_acq_rel);
        }

        m_size.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }

    new_node.invalidate();
    return false;
  }

  [[nodiscard]] bool insert_before(item_reference item, item_reference new_item) noexcept {
    uint32_t retries{};
    auto& node = (item.*N)();
    auto& new_node = (new_item.*N)();

    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      uint64_t node_links = node.m_links.load(std::memory_order_acquire);
      auto link_data = unpack_links(node_links);
      auto new_node_link = to_link(new_node);

      if (node_links == Node::NULL_LINK || link_data.is_deleting()) [[unlikely]] {
        /* Node was removed or is being deleted */
        new_node.invalidate();
        return false;
      }

      /* Set new node's links */
      new_node.m_links.store(pack_links(to_link(node), link_data.prev, 0, 0), std::memory_order_relaxed);

      if (node.m_links.compare_exchange_strong(node_links,
            pack_links(link_data.next, new_node_link,
                      link_data.next_version, (link_data.prev_version + 1) & Node::VERSION_MASK),
            std::memory_order_acq_rel)) [[likely]] {

        /* Update prev node's next link if it exists */
        bool prev_updated = true;
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

            if (prev_links == Node::NULL_LINK || prev_link_data.is_deleting()) [[unlikely]] {
              /* Prev node was removed or being deleted, restore and retry outer loop */
              node.m_links.store(node_links, std::memory_order_release);
              prev_updated = false;
              break;
            }

            if (prev_link_data.next != to_link(node)) [[unlikely]] {
              /* Prev node was modified, restore and retry outer loop */
              node.m_links.store(node_links, std::memory_order_release);
              prev_updated = false;
              break;
            }

          } while (!prev_node->m_links.compare_exchange_weak(prev_links,
                    pack_links(new_node_link, prev_link_data.prev,
                              (prev_link_data.next_version + 1) & Node::VERSION_MASK, prev_link_data.prev_version),
                    std::memory_order_acq_rel));

          if (!prev_updated) {
            continue;  /* Retry outer loop */
          }
        } else {
          /* Update head if necessary */
          auto expected_head = to_link(node);
          m_head.compare_exchange_strong(expected_head, new_node_link, std::memory_order_acq_rel);
        }

        m_size.fetch_add(1, std::memory_order_relaxed);
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

    while (current != Node::NULL_PTR && current != Node::DELETING_MARK && retries++ < Node::MAX_RETRIES) [[likely]] {
      auto node = to_node(current);
      auto item = to_item(*node);

      auto links = node->m_links.load(std::memory_order_acquire);
      auto link_data = unpack_links(links);

      if (links == Node::NULL_LINK || link_data.is_deleting()) [[unlikely]] {
        /* Node was removed or being deleted, try to recover from head */
        current = m_head.load(std::memory_order_acquire);
        retries++;
        continue;
      }

      if (predicate(item)) [[unlikely]] {
        return item;
      }

      current = link_data.next;
    }

    return nullptr;
  }

  [[nodiscard]] item_pointer pop_front() noexcept {
    uint32_t retries{};
    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      auto link = m_head.load(std::memory_order_acquire);
      if (link == Node::NULL_PTR) [[unlikely]] {
        return nullptr;
      }
      if (auto* item = remove(*to_item(link))) [[likely]] {
        return item;
      }
    }
    return nullptr;
  }

  [[nodiscard]] item_pointer pop_back() noexcept {
    uint32_t retries{};
    while (retries++ < Node::MAX_RETRIES) [[likely]] {
      auto link = m_tail.load(std::memory_order_acquire);
      if (link == Node::NULL_PTR) [[unlikely]] {
        return nullptr;
      }
      if (auto* item = remove(*to_item(link))) [[likely]] {
        return item;
      }
    }
    return nullptr;
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
  [[nodiscard]] bool validate_node_links(const node_type& node) const noexcept {
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

  [[nodiscard]] size_t size() const noexcept {
    return m_size.load(std::memory_order_relaxed);
  }

private:
  std::pair<item_pointer, item_pointer> m_bounds{};
  std::atomic<typename node_type::Link_type> m_head{node_type::NULL_PTR};
  std::atomic<typename node_type::Link_type> m_tail{node_type::NULL_PTR};
  std::atomic<size_t> m_size{0};
};

} // namespace ut
