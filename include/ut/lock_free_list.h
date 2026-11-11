#pragma once

/**
 * Lock-free intrusive doubly linked list over a fixed buffer.
 *
 * Nodes are addressed by their index in a user supplied array, the list
 * never allocates. See docs/lock-free-list-design.md for the algorithm and
 * the reasoning behind it. The short version:
 *
 *  - The forward (next) chain is the truth. A node is in the list if and
 *    only if it is reachable from the head sentinel via next links.
 *  - The prev links are hints. They are repaired after every structural
 *    change and are exact whenever the list is quiescent, but a reader must
 *    validate them against the next chain (the list does that internally).
 *  - Every word that is the target of a CAS carries a modification tag, so
 *    that nodes can be removed and re-inserted at will without ABA problems.
 *
 * All mutating operations and find() are linearizable and lock-free.
 * Iterators are weakly consistent (see List_iterator).
 */

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

/* The atomic template to use. Only the tests override this, to substitute
an instrumented atomic that makes every access a deterministic scheduling
point (see tests/sim/sim.h). */
#ifndef UT_LOCK_FREE_LIST_ATOMIC
#define UT_LOCK_FREE_LIST_ATOMIC std::atomic
#endif

/* How many times to retry the O(1) path (a prev hint that is momentarily
stale, or waiting for another thread that is between two steps) before
falling back to an O(n) search from the head. The fallback is what makes the
list lock-free when a thread is stalled, it is not needed otherwise. The
tests set this to a small value in order to exercise the fallback. */
#ifndef UT_LOCK_FREE_LIST_SPIN_LIMIT
#define UT_LOCK_FREE_LIST_SPIN_LIMIT 256
#endif

namespace ut {

/** Tell the CPU that this is a spin-wait loop. */
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

/* Upper bound, in cpu_relax() calls, of the exponential backoff after losing
a race for a link. */
#ifndef UT_LOCK_FREE_LIST_MAX_BACKOFF
#define UT_LOCK_FREE_LIST_MAX_BACKOFF 4096
#endif

/**
 * Exponential backoff for one operation.
 *
 * When many threads go for the same link, typically the head or the tail,
 * retrying immediately only makes the cache line bounce between the cores
 * and the throughput collapses as threads are added. Backing off keeps it
 * roughly constant. For the same reason a thread that finds a pop_front()
 * in progress gives its owner a moment to finish before it helps: helping
 * is needed for lock-freedom, but everybody helping at once is a stampede.
 */
struct Backoff {
  /** Call after losing a race. */
  void pause() noexcept {
    /* Randomized, otherwise the threads that collided retry in lockstep
    and collide again. Seeded with the address of this object, which is on
    the stack of the thread. */
    if (m_random == 0) {
      m_random = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) >> 4) | 1;
    }

    m_random ^= m_random << 13;
    m_random ^= m_random >> 17;
    m_random ^= m_random << 5;

    for (auto n = m_random % m_delay + 1; n > 0; --n) {
      cpu_relax();
    }

    if (m_delay < UT_LOCK_FREE_LIST_MAX_BACKOFF) {
      m_delay *= 2;
    }
  }

  /** Call when another thread's operation is in the way.
  @return true if the caller should check again, the thread has waited a
  little. False if it has waited long enough and should help instead. */
  [[nodiscard]] bool wait() noexcept {
    if (m_waits >= MAX_WAITS) {
      return false;
    }

    ++m_waits;
    pause();
    return true;
  }

  static constexpr uint32_t MAX_WAITS = UT_LOCK_FREE_LIST_SPIN_LIMIT < 8 ? UT_LOCK_FREE_LIST_SPIN_LIMIT : 8;

  uint32_t m_delay{1};
  uint32_t m_waits{};
  uint32_t m_random{};
};

/** Thrown by an iterator when the element it points to was removed from
the list by a concurrent operation and the traversal cannot continue. */
struct Iterator_invalidated : public std::runtime_error {
  explicit Iterator_invalidated(const char* msg) : std::runtime_error(msg) {}
};

/**
 * The list hook, embed one in every item.
 *
 * Life cycle of a node (the state lives in m_next):
 *
 *   free -> pending -> clean -> mark | mark_flagged -> [unlinked] -> free
 *
 *   free          Not in a list. The owner may insert it.
 *   pending       Claimed by an insert that has not completed yet.
 *   clean         In the list.
 *   mark          Logically deleted by remove() or pop_back().
 *   mark_flagged  Logically deleted by pop_front().
 *   unlinked      Was marked, and another thread has unlinked it on behalf
 *                 of its owner.
 *
 * A node can go through this cycle any number of times.
 */
struct Node {
  using Link_type = uint32_t;

  static constexpr uint32_t LINK_BITS = 30;
  static constexpr Link_type LINK_MASK = (Link_type{1} << LINK_BITS) - 1;

  /** Null link, the node is not linked. */
  static constexpr Link_type NULL_PTR = LINK_MASK;
  /** Links of the sentinels, which live in the List and not in the buffer. */
  static constexpr Link_type HEAD_LINK = LINK_MASK - 1;
  static constexpr Link_type TAIL_LINK = LINK_MASK - 2;
  /** Maximum number of items that a list can manage. */
  static constexpr Link_type MAX_ITEMS = TAIL_LINK;

  enum State : uint64_t {
    CLEAN = 0,
    /* The node is being inserted. The inserting thread clears this once
    the node is reachable. */
    PENDING = 1,
    /* Only ever set on the head sentinel: the first node is being popped,
    the link from the head to it is frozen. */
    FLAG = 2,
    /* Logically deleted, the next link is frozen. */
    MARK = 3,
    /* Logically deleted by pop_front(), the next link is frozen. */
    MARK_FLAGGED = 4,
    /* A marked node that was unlinked by a thread other than its owner.
    This tells the owner, in O(1), that the node is not reachable anymore. */
    UNLINKED = 5
  };

  /** Layout of m_next: [tag:31][state:3][link:30]. The tag is incremented
  on every modification and is never reset, not even when the node is
  removed and re-inserted. */
  static constexpr uint32_t STATE_BITS = 3;
  static constexpr uint32_t NEXT_TAG_SHIFT = LINK_BITS + STATE_BITS;

  /** Layout of m_prev: [tag:34][link:30]. */
  static constexpr uint32_t PREV_TAG_SHIFT = LINK_BITS;

  [[nodiscard]] static constexpr Link_type link_of(uint64_t word) noexcept {
    return static_cast<Link_type>(word) & LINK_MASK;
  }

  [[nodiscard]] static constexpr State state_of(uint64_t word) noexcept {
    return static_cast<State>((word >> LINK_BITS) & ((uint64_t{1} << STATE_BITS) - 1));
  }

  [[nodiscard]] static constexpr bool is_dead(uint64_t word) noexcept {
    return state_of(word) >= MARK;
  }

  /** @return the successor of the next word "old", with the given link and state. */
  [[nodiscard]] static constexpr uint64_t make_next(uint64_t old, Link_type link, State state) noexcept {
    return (((old >> NEXT_TAG_SHIFT) + 1) << NEXT_TAG_SHIFT) | (static_cast<uint64_t>(state) << LINK_BITS) | link;
  }

  /** @return the successor of the prev word "old", with the given link. */
  [[nodiscard]] static constexpr uint64_t make_prev(uint64_t old, Link_type link) noexcept {
    return (((old >> PREV_TAG_SHIFT) + 1) << PREV_TAG_SHIFT) | link;
  }

  Node() = default;

  /* List membership is a property of the node's identity (its address),
  not of its value: a copy is never linked and assigning to a node doesn't
  change whether it is linked. This makes "buffer[i] = Item{...}" safe. */
  Node(const Node&) noexcept {}
  Node& operator=(const Node&) noexcept { return *this; }

  /** @return true if the node is not in a list. */
  [[nodiscard]] bool is_null() const noexcept {
    return link_of(m_next.load()) == NULL_PTR;
  }

  UT_LOCK_FREE_LIST_ATOMIC<uint64_t> m_next{NULL_PTR};
  UT_LOCK_FREE_LIST_ATOMIC<uint64_t> m_prev{NULL_PTR};
};

template <typename T, auto N>
struct List;

/**
 * Bidirectional iterator.
 *
 * Iterators are weakly consistent: they never block, never touch memory
 * outside the buffer and, as long as the element they point to stays in
 * the list, they visit every element that is in the list for the whole
 * duration of the traversal exactly once and in order. Elements inserted or
 * removed concurrently may or may not be seen.
 *
 * If the element that the iterator points to is removed concurrently the
 * iterator is invalidated: advancing it either throws Iterator_invalidated
 * or, if the item was already re-inserted, continues from the new position.
 */
template <typename T, auto N, bool IsConst = false>
struct List_iterator {
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = std::conditional_t<IsConst, const T*, T*>;
  using reference = std::conditional_t<IsConst, const T&, T&>;
  using iterator_category = std::bidirectional_iterator_tag;

  using list_type = List<T, N>;
  using link_type = Node::Link_type;

  List_iterator() = default;

  List_iterator(const list_type* list, link_type current) noexcept
    : m_list(list), m_current(current) {}

  template <bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
  List_iterator(const List_iterator<T, N, WasConst>& rhs) noexcept
    : m_list(rhs.m_list), m_current(rhs.m_current) {}

  [[nodiscard]] bool operator==(const List_iterator& rhs) const noexcept {
    return m_current == rhs.m_current;
  }

  [[nodiscard]] bool operator!=(const List_iterator& rhs) const noexcept {
    return !(*this == rhs);
  }

  [[nodiscard]] reference operator*() const noexcept {
    return *operator->();
  }

  /** @return the item, nullptr for end(). */
  [[nodiscard]] pointer operator->() const noexcept {
    return m_list != nullptr && m_current < Node::TAIL_LINK ? m_list->to_item(m_current) : nullptr;
  }

  List_iterator& operator++() {
    if (m_list != nullptr) {
      m_current = m_list->iterator_next(m_current);
    }
    return *this;
  }

  List_iterator operator++(int) {
    List_iterator tmp = *this;
    ++(*this);
    return tmp;
  }

  /** Decrementing begin() is a no-op. */
  List_iterator& operator--() {
    if (m_list != nullptr) {
      m_current = m_list->iterator_prev(m_current);
    }
    return *this;
  }

  List_iterator operator--(int) {
    List_iterator tmp = *this;
    --(*this);
    return tmp;
  }

  const list_type* m_list{};
  /** Link of the current item, TAIL_LINK for end(). */
  link_type m_current{Node::TAIL_LINK};
};

/**
 * Reverse iterator, same guarantees as List_iterator.
 *
 * This is not std::reverse_iterator<List_iterator> because that adapter
 * points one past the element that it refers to and looks the element up
 * when it is dereferenced. With concurrent removals that lookup can find
 * nothing even though the iterator just compared unequal to rend(). This
 * iterator points at its element: if it is not equal to rend() then it can
 * be dereferenced.
 */
template <typename T, auto N, bool IsConst = false>
struct List_reverse_iterator {
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using pointer = std::conditional_t<IsConst, const T*, T*>;
  using reference = std::conditional_t<IsConst, const T&, T&>;
  using iterator_category = std::bidirectional_iterator_tag;

  using list_type = List<T, N>;
  using link_type = Node::Link_type;

  List_reverse_iterator() = default;

  List_reverse_iterator(const list_type* list, link_type current) noexcept
    : m_list(list), m_current(current) {}

  template <bool WasConst, typename = std::enable_if_t<IsConst && !WasConst>>
  List_reverse_iterator(const List_reverse_iterator<T, N, WasConst>& rhs) noexcept
    : m_list(rhs.m_list), m_current(rhs.m_current) {}

  [[nodiscard]] bool operator==(const List_reverse_iterator& rhs) const noexcept {
    return m_current == rhs.m_current;
  }

  [[nodiscard]] bool operator!=(const List_reverse_iterator& rhs) const noexcept {
    return !(*this == rhs);
  }

  [[nodiscard]] reference operator*() const noexcept {
    return *operator->();
  }

  /** @return the item, nullptr for rend(). */
  [[nodiscard]] pointer operator->() const noexcept {
    return m_list != nullptr && m_current < Node::TAIL_LINK ? m_list->to_item(m_current) : nullptr;
  }

  List_reverse_iterator& operator++() {
    if (m_list != nullptr) {
      m_current = m_list->reverse_iterator_next(m_current);
    }
    return *this;
  }

  List_reverse_iterator operator++(int) {
    List_reverse_iterator tmp = *this;
    ++(*this);
    return tmp;
  }

  /** Decrementing rbegin() is a no-op. */
  List_reverse_iterator& operator--() {
    if (m_list != nullptr) {
      m_current = m_list->reverse_iterator_prev(m_current);
    }
    return *this;
  }

  List_reverse_iterator operator--(int) {
    List_reverse_iterator tmp = *this;
    --(*this);
    return tmp;
  }

  const list_type* m_list{};
  /** Link of the current item, HEAD_LINK for rend(). */
  link_type m_current{Node::HEAD_LINK};
};

template <typename T, auto N>
struct List {
  using iterator = List_iterator<T, N, false>;
  using const_iterator = List_iterator<T, N, true>;
  using reverse_iterator = List_reverse_iterator<T, N, false>;
  using const_reverse_iterator = List_reverse_iterator<T, N, true>;

  using value_type = T;
  using node_type = std::remove_reference_t<typename std::invoke_result_t<decltype(N), T>>;
  using node_pointer = node_type*;
  using item_type = T;
  using item_pointer = item_type*;
  using item_reference = item_type&;
  using link_type = Node::Link_type;

  /** @param[in] base  First item of the buffer.
  @param[in] end   One past the last item of the buffer.
  All the nodes in the buffer must be unlinked. */
  List(item_pointer base, item_pointer end) noexcept : m_bounds(base, end) {
    assert(base != nullptr);
    assert(base <= end);
    assert(static_cast<size_t>(end - base) <= Node::MAX_ITEMS);

    m_head.m_next.store(Node::make_next(0, Node::TAIL_LINK, Node::CLEAN));
    m_tail.m_prev.store(Node::make_prev(0, Node::HEAD_LINK));
  }

  List(const List&) = delete;
  List& operator=(const List&) = delete;

  [[nodiscard]] link_type to_link(const item_type& item) const noexcept {
    assert(&item >= m_bounds.first && &item < m_bounds.second);
    return static_cast<link_type>(&item - m_bounds.first);
  }

  [[nodiscard]] item_pointer to_item(link_type link) const noexcept {
    assert(link < Node::TAIL_LINK);
    return m_bounds.first + link;
  }

  /** Insert the item at the front of the list.
  @return false if the item is already in a list (or is being inserted). */
  [[nodiscard]] bool push_front(item_reference item) noexcept {
    return insert_after(m_head, Node::HEAD_LINK, (item.*N)(), to_link(item));
  }

  /** Insert the item at the back of the list.
  @return false if the item is already in a list (or is being inserted). */
  [[nodiscard]] bool push_back(item_reference item) noexcept {
    return insert_before(Node::TAIL_LINK, (item.*N)(), to_link(item));
  }

  /** Insert new_item immediately after item.
  @return false if item is not in the list or if new_item is already in a list. */
  [[nodiscard]] bool insert_after(item_reference item, item_reference new_item) noexcept {
    return &item != &new_item && insert_after((item.*N)(), to_link(item), (new_item.*N)(), to_link(new_item));
  }

  /** Insert new_item immediately before item.
  @return false if item is not in the list or if new_item is already in a list. */
  [[nodiscard]] bool insert_before(item_reference item, item_reference new_item) noexcept {
    return &item != &new_item && insert_before(to_link(item), (new_item.*N)(), to_link(new_item));
  }

  /** Remove the item from the list.
  @return the item, nullptr if it is not in the list (this includes the case
  where another thread is removing it). Once this returns the caller owns
  the item exclusively and may re-insert it. */
  [[nodiscard]] item_pointer remove(item_reference item) noexcept {
    auto& node = (item.*N)();
    auto word = node.m_next.load();
    uint64_t marked;
    uint32_t spins{};

    /* The successful CAS is the linearization point. */
    do {
      if (!is_alive(to_link(item), word, spins)) {
        return nullptr;
      }
      marked = Node::make_next(word, Node::link_of(word), Node::MARK);
    } while (!node.m_next.compare_exchange_weak(word, marked));

    if (!unlink(node, to_link(item), marked, spins)) {
      /* A concurrent pop_front() got it. */
      return nullptr;
    }

    release(node);
    count(-1);
    return &item;
  }

  /** Remove the first item.
  @return the item, nullptr if the list is empty. */
  [[nodiscard]] item_pointer pop_front() noexcept {
    for (Backoff backoff;;) {
      auto word = m_head.m_next.load();
      const auto first = Node::link_of(word);

      if (first == Node::TAIL_LINK) {
        return nullptr;
      }

      if (Node::state_of(word) == Node::FLAG) {
        if (!backoff.wait()) {
          help_pop_front(word);
        }
        continue;
      }

      /* Both CAS below can only succeed if the head's next word did not
      change in the meantime, in which case this was read while the node
      was the first node. */
      auto& node = this->node(first);
      const auto first_word = node.m_next.load();

      if (Node::is_dead(first_word)) {
        /* Removed by somebody else, get it out of the way. */
        const auto unlinked = Node::make_next(word, Node::link_of(first_word), Node::CLEAN);

        if (m_head.m_next.compare_exchange_strong(word, unlinked)) {
          acknowledge_unlink(node, first_word);
          fix_prev(m_head, Node::HEAD_LINK, unlinked);
        }
        continue;
      }

      /* The node was alive when it was read. Freeze the link from the head
      to it. From here on
      nothing can be inserted before it and it can't be unlinked by anybody
      else, so that it is still the first node at the instant that it is
      marked, which is the linearization point. The thread that sets the
      flag owns the node, the rest can be done by any thread. */
      const auto flagged = Node::make_next(word, first, Node::FLAG);

      if (!m_head.m_next.compare_exchange_strong(word, flagged)) {
        backoff.pause();
        continue;
      }

      do {
        help_pop_front(flagged);
      } while (m_head.m_next.load() == flagged);

      release(node);
      count(-1);
      return to_item(first);
    }
  }

  /** Remove the last item.
  @return the item, nullptr if the list is empty. */
  [[nodiscard]] item_pointer pop_back() noexcept {
    Backoff backoff;

    for (uint32_t spins{};;) {
      Position pos;

      if (find_pred(Node::TAIL_LINK, pos, spins) != Walk::found) {
        /* The hint is stale, it will be repaired shortly. */
        continue;
      }

      if (pos.m_prev == Node::HEAD_LINK) {
        return nullptr;
      }

      /* The next word of the last node points to the tail. Marking it with
      a CAS is atomic with the check that it is still the last node. */
      auto& node = this->node(pos.m_prev);
      const auto marked = Node::make_next(pos.m_word, Node::TAIL_LINK, Node::MARK);

      if (node.m_next.compare_exchange_strong(pos.m_word, marked) && unlink(node, pos.m_prev, marked, spins)) {
        release(node);
        count(-1);
        return to_item(pos.m_prev);
      }

      backoff.pause();
    }
  }

  /** Find the first item for which the predicate returns true.
  The predicate can be called more than once for the same item.
  @return the item, nullptr if there is none. */
  template <typename Predicate>
  [[nodiscard]] item_pointer find(Predicate predicate) noexcept {
    Position pos;
    auto before = [](link_type) { return false; };
    auto alive = [&](link_type link) { return static_cast<bool>(predicate(to_item(link))); };

    if (walk(Node::HEAD_LINK, m_head.m_next.load(), true, before, alive, pos) == Walk::found) {
      return to_item(Node::link_of(pos.m_word));
    }
    return nullptr;
  }

  [[nodiscard]] iterator begin() noexcept {
    return iterator(this, first_link());
  }

  [[nodiscard]] const_iterator begin() const noexcept {
    return const_iterator(this, first_link());
  }

  [[nodiscard]] iterator end() noexcept {
    return iterator(this, Node::TAIL_LINK);
  }

  [[nodiscard]] const_iterator end() const noexcept {
    return const_iterator(this, Node::TAIL_LINK);
  }

  [[nodiscard]] reverse_iterator rbegin() noexcept {
    return reverse_iterator(this, last_link());
  }

  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(this, last_link());
  }

  [[nodiscard]] reverse_iterator rend() noexcept {
    return reverse_iterator(this, Node::HEAD_LINK);
  }

  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(this, Node::HEAD_LINK);
  }

  /** @return the number of items. Exact when the list is quiescent,
  otherwise it can lag behind operations that are in progress. The count is
  kept in per thread stripes so that it is not a point of contention, which
  makes this O(number of stripes). */
  [[nodiscard]] size_t size() const noexcept {
    std::ptrdiff_t n{};

    for (const auto& counter : m_counters) {
      n += counter.m_value.load(std::memory_order_relaxed);
    }
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

  /** Check the invariants of a quiescent list, for tests and debugging:
  no operation is half done, the prev links are exact and size() is right.
  The result is meaningless while other threads are modifying the list.
  @return true if the list is consistent. */
  [[nodiscard]] bool is_consistent() const noexcept {
    const auto capacity = static_cast<size_t>(m_bounds.second - m_bounds.first);
    auto prev = Node::HEAD_LINK;
    size_t count{};

    for (;;) {
      const auto word = node(prev).m_next.load();
      const auto current = Node::link_of(word);

      if (current == Node::NULL_PTR || current == Node::HEAD_LINK || Node::state_of(word) != Node::CLEAN) {
        return false;
      }

      if (Node::link_of(node(current).m_prev.load()) != prev) {
        return false;
      }

      if (current == Node::TAIL_LINK) {
        break;
      }

      if (current >= capacity || ++count > capacity) {
        return false;
      }

      prev = current;
    }

    return count == size();
  }

private:
  template <typename, auto, bool>
  friend struct List_iterator;

  template <typename, auto, bool>
  friend struct List_reverse_iterator;

  /* How far to follow stale prev hints before falling back to a search
  from the head. */
  static constexpr uint32_t HINT_HOPS = 4;
  static constexpr uint32_t HINT_STEPS = 16;

  /** A link in the next chain: the word read from the m_next of m_prev. */
  struct Position {
    link_type m_prev{};
    uint64_t m_word{};
  };

  enum class Walk {
    found,
    /* Reached the tail. */
    end,
    /* Gave up, only if the walk didn't start at the head. */
    inconclusive
  };

  [[nodiscard]] node_type& node(link_type link) const noexcept {
    assert(link != Node::NULL_PTR);

    if (link >= Node::TAIL_LINK) [[unlikely]] {
      return link == Node::TAIL_LINK ? m_tail : m_head;
    }
    return (m_bounds.first[link].*N)();
  }

  /** Make the node free. The caller must own it exclusively: it is either
  pending and not linked, or dead and unlinked. In both cases no other
  thread can modify m_next (a CAS with a stale expected value fails because
  of the tag) and for every other thread there is no difference between the
  node being released a little later and the owner being slow, which is why
  this store doesn't have to be sequentially consistent. */
  static void release(node_type& node) noexcept {
    node.m_next.store(Node::make_next(node.m_next.load(), Node::NULL_PTR, Node::CLEAN), std::memory_order_release);
  }

  /** Make the prev hint of the successor point to pred.
  @param[in] word  The next word of pred that established the link. The
  hint is only written while pred.m_next is still exactly this word, i.e.
  while the link is intact. Whoever changes the link is responsible for the
  hint from then on. Because the prev word is read before the link is
  validated, a thread with a stale view can never overwrite the hint that
  was written for a newer link. */
  void fix_prev(node_type& pred, link_type pred_link, uint64_t word) noexcept {
    auto& succ = node(Node::link_of(word));
    auto old = succ.m_prev.load();

    for (;;) {
      if (pred.m_next.load() != word) {
        return;
      }
      if (succ.m_prev.compare_exchange_weak(old, Node::make_prev(old, pred_link))) {
        return;
      }
    }
  }

  /** Complete the pop_front() that set the flag.
  @param[in] flagged  The flagged next word of the head. */
  void help_pop_front(uint64_t flagged) noexcept {
    auto& victim = node(Node::link_of(flagged));
    auto word = victim.m_next.load();

    for (;;) {
      /* While the flag is in place the victim can't have been unlinked
      and recycled, therefore "word" is what we think it is. */
      if (m_head.m_next.load() != flagged) {
        return;
      }

      if (Node::state_of(word) == Node::MARK_FLAGGED) {
        break;
      }

      /* If the victim was marked by a concurrent remove()/pop_back() this
      takes it over: they notice that their mark is gone and back off. */
      const auto marked = Node::make_next(word, Node::link_of(word), Node::MARK_FLAGGED);

      if (victim.m_next.compare_exchange_weak(word, marked)) {
        word = marked;
        break;
      }
    }

    const auto unlinked = Node::make_next(flagged, Node::link_of(word), Node::CLEAN);

    if (m_head.m_next.compare_exchange_strong(flagged, unlinked)) {
      fix_prev(m_head, Node::HEAD_LINK, unlinked);
    }
  }

  /**
   * Walk the next chain, physically removing the dead nodes on the way.
   *
   * Invariant: "word" was read from the m_next of "prev" at a time when
   * prev was in the list and alive. Before acting on what was read from the
   * current node, the word of prev is read again. If it is unchanged then
   * the current node was the successor of prev, and therefore in the list,
   * when it was read. Without this check a node that is removed and
   * re-inserted elsewhere while the walk is standing on it would silently
   * teleport the walk to another part of the list.
   *
   * @param[in] prev         Where to start.
   * @param[in] word         Next word of prev.
   * @param[in] from_head    If true then restart from the head when the
   *                         validation fails, otherwise give up after a few
   *                         steps or if the validation fails.
   * @param[in] stop_before  Called with the link of each node, before
   *                         checking whether it is alive.
   * @param[in] stop_alive   Called with the link of each alive node.
   * @param[out] pos         If found, the link that points to the node.
   */
  template <typename Before, typename Alive>
  [[nodiscard]] Walk walk(link_type prev, uint64_t word, bool from_head, Before&& stop_before, Alive&& stop_alive, Position& pos) noexcept {
    /* The link that points to prev. As long as it is intact, prev has not
    left the list since it was read. */
    Position anchor{Node::NULL_PTR, 0};

    for (uint32_t steps{};; ++steps) {
      const auto current = Node::link_of(word);

      if (stop_before(current)) {
        pos = Position{prev, word};
        return Walk::found;
      }

      if (current == Node::TAIL_LINK) {
        return Walk::end;
      }

      if (!from_head && steps >= HINT_STEPS) {
        return Walk::inconclusive;
      }

      auto& prev_node = node(prev);
      const auto current_word = node(current).m_next.load();
      bool valid = prev_node.m_next.load() == word;

      if (valid && Node::is_dead(current_word)) {
        if (Node::state_of(word) == Node::FLAG) {
          help_pop_front(word);
        } else {
          const auto unlinked = Node::make_next(word, Node::link_of(current_word), Node::CLEAN);

          if (prev_node.m_next.compare_exchange_strong(word, unlinked)) {
            acknowledge_unlink(node(current), current_word);
            fix_prev(prev_node, prev, unlinked);
            word = unlinked;
            continue;
          }
        }

        /* Somebody else changed the link. We can only continue from prev
        if it never left the list: it is not enough that it is in the list
        now, it can have been removed and re-inserted somewhere else. The
        head never moves, for any other node check that the link that
        points to it is intact. */
        word = prev_node.m_next.load();

        if (prev == Node::HEAD_LINK) {
          continue;
        }

        if (anchor.m_prev != Node::NULL_PTR && !Node::is_dead(word) && node(anchor.m_prev).m_next.load() == anchor.m_word) {
          continue;
        }

        valid = false;
      }

      if (!valid) {
        if (!from_head) {
          return Walk::inconclusive;
        }
        prev = Node::HEAD_LINK;
        word = m_head.m_next.load();
        anchor.m_prev = Node::NULL_PTR;
        continue;
      }

      if (stop_alive(current)) {
        pos = Position{prev, word};
        return Walk::found;
      }

      anchor = Position{prev, word};
      prev = current;
      word = current_word;
    }
  }

  /** Tell the owner of a marked node that it was unlinked on its behalf.
  @param[in] word  The marked next word of the node. If the owner has found
  out on its own and has recycled the node then the tag doesn't match. */
  static void acknowledge_unlink(node_type& node, uint64_t word) noexcept {
    node.m_next.compare_exchange_strong(word, Node::make_next(word, Node::link_of(word), Node::UNLINKED));
  }

  /** Find the link that points to the target (which can be dead), using
  the prev hint of the target. O(1).
  @return found, or inconclusive: the target is not in the list or the hint
  is stale. A stale hint is repaired shortly by the thread that made it so. */
  [[nodiscard]] Walk find_pred_fast(link_type target, Position& pos) noexcept {
    auto before = [target](link_type link) { return link == target; };
    auto alive = [](link_type) { return false; };
    auto hint = Node::link_of(node(target).m_prev.load());

    for (uint32_t i{}; i < HINT_HOPS && hint != Node::NULL_PTR; ++i) {
      auto& hint_node = node(hint);
      const auto word = hint_node.m_next.load();

      if (Node::link_of(word) == Node::NULL_PTR || Node::state_of(word) == Node::PENDING) {
        /* Free, or possibly not linked yet. */
        break;
      }

      if (Node::is_dead(word)) {
        hint = Node::link_of(hint_node.m_prev.load());
        continue;
      }

      /* Clean or flagged, therefore in the list at the time of the load. */
      if (walk(hint, word, false, before, alive, pos) == Walk::found) {
        return Walk::found;
      }
      break;
    }
    return Walk::inconclusive;
  }

  /** Find the link that points to the target (which can be dead), with a
  search from the head. O(n).
  @return found, or end if the target is not in the list. */
  [[nodiscard]] Walk find_pred_slow(link_type target, Position& pos) noexcept {
    auto before = [target](link_type link) { return link == target; };
    auto alive = [](link_type) { return false; };

    return walk(Node::HEAD_LINK, m_head.m_next.load(), true, before, alive, pos);
  }

  /** Find the link that points to the target (which can be dead).
  Meant to be called in a retry loop: the first UT_LOCK_FREE_LIST_SPIN_LIMIT
  calls only try the O(1) path, after that it is a search from the head.
  @param[in,out] spins  Number of inconclusive calls so far.
  @return found, end if the target is not in the list (only after a search
  from the head), inconclusive if the caller should re-check and retry. */
  [[nodiscard]] Walk find_pred(link_type target, Position& pos, uint32_t& spins) noexcept {
    if (find_pred_fast(target, pos) == Walk::found) {
      return Walk::found;
    }

    if (spins < UT_LOCK_FREE_LIST_SPIN_LIMIT) {
      ++spins;
      cpu_relax();
      return Walk::inconclusive;
    }

    return find_pred_slow(target, pos);
  }

  /**
   * Check whether a node that the user passed in is in the list.
   *
   * The interesting case is a pending node. It is in the list from the
   * instant that it becomes reachable, other threads can already have seen
   * it there, even though the inserting thread has not cleared the pending
   * state yet. Therefore a search decides, not the state.
   *
   * If the search doesn't find it, "not in the list" is only correct if the
   * insert in progress can't take effect in the past. An insert_before()
   * can: if its target is deleted before the new node is linked, then it
   * takes effect just before that deletion. This is taken care of by the
   * search itself. It starts from the head and unlinks every dead node on
   * the way, so if the target was deleted before the search started then
   * it is unlinked by the time that the search is done and the insert
   * fails (the link that it wants to replace is gone). If the target was
   * deleted later, then "not in the list" was the truth when the search
   * started.
   *
   * @param[in,out] word  The next word of the node. If the node is alive
   * then the caller's subsequent CAS on this exact word ensures that the
   * answer still applies.
   * @param[in,out] spins  The spin budget of the operation, see find_pred().
   * There is one budget per operation, not one per loop: with nested
   * budgets a stalled thread would cost the others the product.
   * @return true if the node is in the list and not deleted.
   */
  [[nodiscard]] bool is_alive(link_type link, uint64_t& word, uint32_t& spins) noexcept {
    for (;;) {
      if (Node::link_of(word) == Node::NULL_PTR || Node::is_dead(word)) {
        return false;
      }

      if (Node::state_of(word) != Node::PENDING) [[likely]] {
        return true;
      }

      Position pos;

      /* Normally the insert completes, one way or the other, within a few
      instructions and the state changes. Only if the inserting thread is
      stalled does this end up searching from the head. */
      const auto result = find_pred(link, pos, spins);

      if (result == Walk::found) {
        return true;
      }

      /* The answer is for the insert attempt that wrote this word. */
      const auto current = node(link).m_next.load();

      if (current == word && result == Walk::end) {
        return false;
      }

      word = current;
    }
  }

  enum class Link_result {
    linked,
    /* The next word of pred has changed. */
    retry,
    /* The new node is in a list already, or is being inserted. */
    not_free
  };

  /** Link a node into the list, after pred.
  @param[in] pred_word       Next word of pred, read when pred was in the list.
  @param[in] succ_is_target  True for insert_before(): the successor must
                             be alive after the pending node names it as
                             its successor, see is_alive().
  @param[in,out] claimed     True if this thread already owns the new node,
                             i.e. it is pending. Set on the first attempt. */
  [[nodiscard]] Link_result link_in(node_type& pred, link_type pred_link, uint64_t pred_word, node_type& new_node, link_type new_link, bool succ_is_target, bool& claimed) noexcept {
    const auto succ_link = Node::link_of(pred_word);
    uint64_t pending;

    if (!claimed) {
      /* free -> pending */
      auto word = new_node.m_next.load();

      do {
        if (Node::link_of(word) != Node::NULL_PTR || Node::state_of(word) != Node::CLEAN) {
          return Link_result::not_free;
        }
        pending = Node::make_next(word, succ_link, Node::PENDING);
      } while (!new_node.m_next.compare_exchange_weak(word, pending));

      claimed = true;
    } else {
      /* The node is private, only threads with a stale view can touch it
      and their CAS can't succeed because of the tag. */
      pending = Node::make_next(new_node.m_next.load(), succ_link, Node::PENDING);
      new_node.m_next.store(pending);
    }

    /* The prev hint is exact if the CAS below succeeds. It is published by
    that CAS, a thread with a stale view that modifies it concurrently is
    simply overwritten. */
    new_node.m_prev.store(Node::make_prev(new_node.m_prev.load(), pred_link), std::memory_order_release);

    if (succ_is_target && succ_link != Node::TAIL_LINK) {
      const auto succ_word = node(succ_link).m_next.load();

      if (Node::link_of(succ_word) == Node::NULL_PTR || Node::is_dead(succ_word)) {
        return Link_result::retry;
      }
    }

    /* The linearization point of all the inserts. */
    if (!pred.m_next.compare_exchange_strong(pred_word, Node::make_next(pred_word, new_link, Node::CLEAN))) {
      return Link_result::retry;
    }

    auto expected = pending;
    const auto clean = Node::make_next(pending, succ_link, Node::CLEAN);

    /* If this fails then another thread has already changed the link to
    the successor, and with that taken over the responsibility for the prev
    hint of whatever follows this node now. */
    if (new_node.m_next.compare_exchange_strong(expected, clean)) {
      fix_prev(new_node, new_link, clean);
    }

    count(1);
    return Link_result::linked;
  }

  /** Give up on an insert. */
  [[nodiscard]] static bool abandon(node_type& new_node, bool claimed) noexcept {
    if (claimed) {
      release(new_node);
    }
    return false;
  }

  [[nodiscard]] bool insert_after(node_type& pred, link_type pred_link, node_type& new_node, link_type new_link) noexcept {
    Backoff backoff;
    uint32_t spins{};

    for (bool claimed{};;) {
      auto word = pred.m_next.load();

      if (Node::state_of(word) == Node::FLAG) {
        /* Only the head can be flagged. */
        if (!backoff.wait()) {
          help_pop_front(word);
        }
        continue;
      }

      if (!is_alive(pred_link, word, spins)) {
        return abandon(new_node, claimed);
      }

      switch (link_in(pred, pred_link, word, new_node, new_link, false, claimed)) {
        case Link_result::linked:
          return true;
        case Link_result::not_free:
          return false;
        case Link_result::retry:
          backoff.pause();
          break;
      }
    }
  }

  [[nodiscard]] bool insert_before(link_type succ_link, node_type& new_node, link_type new_link) noexcept {
    Backoff backoff;
    uint32_t spins{};

    for (bool claimed{};;) {
      if (succ_link != Node::TAIL_LINK) {
        auto word = node(succ_link).m_next.load();

        if (!is_alive(succ_link, word, spins)) {
          return abandon(new_node, claimed);
        }
      }

      Position pos;
      const auto result = find_pred(succ_link, pos, spins);

      if (result == Walk::end) {
        return abandon(new_node, claimed);
      }

      if (result == Walk::inconclusive) {
        continue;
      }

      if (Node::state_of(pos.m_word) == Node::FLAG) {
        /* The successor is being popped. */
        if (!backoff.wait()) {
          help_pop_front(pos.m_word);
        }
        continue;
      }

      switch (link_in(node(pos.m_prev), pos.m_prev, pos.m_word, new_node, new_link, true, claimed)) {
        case Link_result::linked:
          return true;
        case Link_result::not_free:
          return false;
        case Link_result::retry:
          backoff.pause();
          break;
      }
    }
  }

  /** Physically remove a node that this thread marked.
  @param[in] marked  The word that this thread wrote to mark the node.
  @return true if the node is unlinked and owned by the caller, false if a
  pop_front() took the node over. */
  [[nodiscard]] bool unlink(node_type& victim, link_type victim_link, uint64_t marked, uint32_t& spins) noexcept {
    const auto acknowledged = Node::make_next(marked, Node::link_of(marked), Node::UNLINKED);

    for (;;) {
      Position pos;
      const auto result = find_pred(victim_link, pos, spins);

      /* This must be checked after the link to the victim was read: if a
      pop_front() took the victim over then it can have been recycled and
      re-inserted by now, and the link that was found is to its new
      incarnation. If our mark is still there, then the CAS on the link
      below can only succeed if the victim never left the list. */
      const auto word = victim.m_next.load();

      if (word == acknowledged) {
        /* Another thread unlinked it for us. */
        return true;
      }

      if (word != marked) {
        return false;
      }

      if (result == Walk::end) {
        /* Unlinked by a helper that has not acknowledged it yet. A take
        over is only possible while the node is linked and our mark was
        still there after the search, so this is final. */
        return true;
      }

      if (result == Walk::inconclusive) {
        continue;
      }

      if (Node::state_of(pos.m_word) == Node::FLAG) {
        help_pop_front(pos.m_word);
        continue;
      }

      auto& pred = node(pos.m_prev);
      const auto unlinked = Node::make_next(pos.m_word, Node::link_of(marked), Node::CLEAN);

      if (pred.m_next.compare_exchange_strong(pos.m_word, unlinked)) {
        fix_prev(pred, pos.m_prev, unlinked);
        return true;
      }
    }
  }

  /* Read only traversal, used by the iterators. */

  /** @return the link of the first alive node starting from the node that
  the word points to, TAIL_LINK if there is none and NULL_PTR if the chain
  leads to a node that was unlinked in the meantime. */
  [[nodiscard]] link_type skip_dead(uint64_t word) const noexcept {
    for (;;) {
      const auto link = Node::link_of(word);

      if (link == Node::TAIL_LINK || link == Node::NULL_PTR) {
        return link;
      }

      word = node(link).m_next.load();

      if (!Node::is_dead(word)) {
        return Node::link_of(word) != Node::NULL_PTR ? link : Node::NULL_PTR;
      }
    }
  }

  [[nodiscard]] link_type first_link() const noexcept {
    for (;;) {
      const auto link = skip_dead(m_head.m_next.load());

      if (link != Node::NULL_PTR) {
        return link;
      }
    }
  }

  [[nodiscard]] link_type iterator_next(link_type current) const {
    if (current == Node::TAIL_LINK) {
      return current;
    }

    const auto word = node(current).m_next.load();
    const auto next = Node::link_of(word) != Node::NULL_PTR ? skip_dead(word) : Node::NULL_PTR;

    if (next == Node::NULL_PTR) {
      throw Iterator_invalidated("Iterator invalidated by a concurrent remove");
    }
    return next;
  }

  /** Follow the next chain from an alive node to the target.
  @return the link of the last alive node before the target, NULL_PTR if
  the target was not reached. */
  [[nodiscard]] link_type scan_to(link_type start, uint64_t word, link_type target, size_t limit) const noexcept {
    auto last_alive = start;

    for (size_t steps{}; steps <= limit; ++steps) {
      const auto link = Node::link_of(word);

      if (link == target) {
        return last_alive;
      }

      if (link == Node::TAIL_LINK || link == Node::NULL_PTR) {
        break;
      }

      word = node(link).m_next.load();

      if (!Node::is_dead(word)) {
        last_alive = link;
      }
    }
    return Node::NULL_PTR;
  }

  /** @return the link of the last alive node before current (which can be
  the tail), HEAD_LINK if there is none and NULL_PTR if current could not be
  found, which means that it was removed. */
  [[nodiscard]] link_type pred_link(link_type current) const noexcept {
    /* Fast path: the prev hint. It is only a hint, the next chain decides. */
    auto hint = Node::link_of(node(current).m_prev.load());

    for (uint32_t i{}; i < HINT_HOPS && hint != Node::NULL_PTR; ++i) {
      auto& hint_node = node(hint);
      const auto word = hint_node.m_next.load();

      if (Node::link_of(word) == Node::NULL_PTR || Node::state_of(word) == Node::PENDING) {
        break;
      }

      if (Node::is_dead(word)) {
        hint = Node::link_of(hint_node.m_prev.load());
        continue;
      }

      const auto prev = scan_to(hint, word, current, HINT_STEPS);

      if (prev != Node::NULL_PTR) {
        return prev;
      }
      break;
    }

    const auto capacity = static_cast<size_t>(m_bounds.second - m_bounds.first);

    return scan_to(Node::HEAD_LINK, m_head.m_next.load(), current, 2 * capacity + 16);
  }

  [[nodiscard]] link_type last_link() const noexcept {
    for (;;) {
      const auto link = pred_link(Node::TAIL_LINK);

      if (link != Node::NULL_PTR) {
        return link;
      }
    }
  }

  [[nodiscard]] link_type checked_pred_link(link_type current) const {
    const auto prev = pred_link(current);

    if (prev == Node::NULL_PTR) {
      throw Iterator_invalidated("Iterator invalidated by a concurrent remove");
    }
    return prev;
  }

  [[nodiscard]] link_type iterator_prev(link_type current) const {
    const auto prev = checked_pred_link(current);

    /* Decrementing begin() is a no-op. */
    return prev == Node::HEAD_LINK ? current : prev;
  }

  [[nodiscard]] link_type reverse_iterator_next(link_type current) const {
    return current == Node::HEAD_LINK ? current : checked_pred_link(current);
  }

  [[nodiscard]] link_type reverse_iterator_prev(link_type current) const {
    const auto next = current == Node::HEAD_LINK ? first_link() : iterator_next(current);

    /* Decrementing rbegin() is a no-op. */
    return next == Node::TAIL_LINK ? current : next;
  }

  std::pair<item_pointer, item_pointer> m_bounds{};

  /* Sentinels. Only m_head.m_next and m_tail.m_prev are used, keep the
  two ends of the list on separate cache lines. */
  alignas(64) mutable node_type m_head{};
  alignas(64) mutable node_type m_tail{};

  /* The item count, striped by thread: a single counter would be written
  by every operation of every thread. */
  static constexpr size_t COUNTER_STRIPES = 64;

  struct alignas(64) Counter {
    UT_LOCK_FREE_LIST_ATOMIC<std::ptrdiff_t> m_value{0};
  };

  [[nodiscard]] static size_t stripe() noexcept {
    static std::atomic<uint32_t> next_id{0};
    thread_local const uint32_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    return id % COUNTER_STRIPES;
  }

  void count(std::ptrdiff_t delta) noexcept {
    m_counters[stripe()].m_value.fetch_add(delta, std::memory_order_relaxed);
  }

  Counter m_counters[COUNTER_STRIPES]{};
};

} // namespace ut
