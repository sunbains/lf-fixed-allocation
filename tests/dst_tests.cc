/**
 * Deterministic simulation + property based tests for ut::List.
 *
 * Every test case is a randomly generated concurrent program (a few simulated
 * threads, each with a short sequence of list operations) that is executed
 * under a randomly generated, but fully deterministic, schedule (see
 * sim/sim.h). After each run the following properties are checked:
 *
 *  1. Progress: the run finishes within a step budget (no livelock).
 *  2. Linearizability: there is a sequential ordering of all the mutating
 *     operations and find(), consistent with their real-time order, that
 *     explains every result and the final contents of the list.
 *  3. Quiescent consistency: once all threads are done, forward traversal,
 *     backward traversal and size() agree, there are no cycles, and the
 *     list's own invariant check (exact prev links, no operation half done)
 *     passes.
 *  4. Conservation: every item is either in the list or owned by exactly
 *     one thread (nothing is lost, nothing is handed out twice).
 *  5. Iteration: concurrent iteration terminates and only yields valid
 *     items; when there are no concurrent removals it yields every item
 *     that was present before it started, in list order, without duplicates.
 *
 * On failure the program is shrunk to a minimal failing one and printed
 * along with the seed. Runs are reproducible:
 *
 *   LF_DST_SEED=<n>       base seed (default: fixed)
 *   LF_DST_CASES=<n>      number of generated programs per property
 *   LF_DST_SCHEDULES=<n>  number of schedules per directed scenario
 *   LF_DST_FIRST_CASE=<n> index of the first generated program
 *   LF_DST_MAX_STEPS=<n>  step budget of a run, more than that is a livelock
 *   LF_DST_TRACE=1        print every atomic step of a failing run
 *   LF_DST_TRACE=2        print every atomic step of every run
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sim/sim.h"

#define UT_LOCK_FREE_LIST_ATOMIC ::sim::Atomic
/* Fall back to the search from the head almost immediately, otherwise it
would hardly ever run. Override to test the other extreme. */
#ifndef UT_LOCK_FREE_LIST_SPIN_LIMIT
#define UT_LOCK_FREE_LIST_SPIN_LIMIT 2
#endif
#include "ut/lock_free_list.h"

namespace {

struct Item {
  ut::Node& node() noexcept { return m_node; }

  int m_id{};
  ut::Node m_node{};
};

using List = ut::List<Item, &Item::node>;

constexpr int NONE = -1;

uint64_t env_or(const char* name, uint64_t fallback) {
  const auto value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::strtoull(value, nullptr, 0) : fallback;
}

enum class Kind : uint8_t {
  push_front,
  push_back,
  insert_after,
  insert_before,
  remove,
  pop_front,
  pop_back,
  find,
  iterate_forward,
  iterate_backward,
  count
};

const char* to_string(Kind kind) {
  static constexpr const char* NAMES[] = {
    "push_front", "push_back", "insert_after", "insert_before", "remove",
    "pop_front", "pop_back", "find", "iterate_forward", "iterate_backward"};
  return NAMES[static_cast<size_t>(kind)];
}

bool is_insert(Kind kind) {
  return kind == Kind::push_front || kind == Kind::push_back || kind == Kind::insert_after || kind == Kind::insert_before;
}

bool is_removal(Kind kind) {
  return kind == Kind::remove || kind == Kind::pop_front || kind == Kind::pop_back;
}

bool is_iteration(Kind kind) {
  return kind == Kind::iterate_forward || kind == Kind::iterate_backward;
}

bool has_target(Kind kind) {
  return kind == Kind::insert_after || kind == Kind::insert_before || kind == Kind::remove || kind == Kind::find;
}

struct Op {
  Kind m_kind{};
  /* Item id for insert_after/insert_before/remove/find. */
  int m_target{NONE};
};

/** A concurrent test program.
 *
 * Items [0, m_n_items) live in a fixed buffer. The ones listed in m_initial
 * are in the list (in that order) before the threads start. Every other item
 * i is initially owned by thread (i % number of threads). An insert operation
 * inserts an item owned by the calling thread (it is skipped if the thread
 * owns none) and a successful remove/pop transfers ownership to the caller,
 * so items get recycled the way a real user of the list would recycle them. */
struct Program {
  int m_n_items{};
  std::vector<int> m_initial{};
  std::vector<std::vector<Op>> m_threads{};
};

std::string to_string(const Program& program) {
  std::ostringstream os;

  os << "items: " << program.m_n_items << ", initial list: [";
  for (size_t i = 0; i < program.m_initial.size(); ++i) {
    os << (i ? ", " : "") << program.m_initial[i];
  }
  os << "]\n";

  for (size_t t = 0; t < program.m_threads.size(); ++t) {
    os << "  thread " << t << ":";
    for (const auto& op : program.m_threads[t]) {
      os << " " << to_string(op.m_kind);
      if (has_target(op.m_kind)) {
        os << "(" << op.m_target << ")";
      }
      os << ";";
    }
    os << "\n";
  }
  return os.str();
}

struct Event {
  int m_thread{};
  Kind m_kind{};
  int m_target{NONE};
  /* The item inserted by an insert operation. */
  int m_item{NONE};
  /* bool for inserts, item id or NONE for remove/pop/find. */
  int m_result{NONE};
  /* Items seen by an iteration. */
  std::vector<int> m_seq{};
  bool m_threw{};
  bool m_overflow{};
  bool m_null_item{};
  uint64_t m_invoke{};
  uint64_t m_response{};
};

std::string to_string(const Event& event) {
  std::ostringstream os;

  os << "[" << event.m_invoke << ", " << event.m_response << "] thread " << event.m_thread << ": " << to_string(event.m_kind) << "(";
  if (has_target(event.m_kind)) {
    os << event.m_target;
  }
  if (is_insert(event.m_kind)) {
    os << (has_target(event.m_kind) ? ", " : "") << "new=" << event.m_item;
  }
  os << ") -> ";

  if (is_iteration(event.m_kind)) {
    os << "{";
    for (size_t i = 0; i < event.m_seq.size(); ++i) {
      os << (i ? ", " : "") << event.m_seq[i];
    }
    os << "}" << (event.m_threw ? " threw Iterator_invalidated" : "") << (event.m_overflow ? " DID NOT TERMINATE" : "") << (event.m_null_item ? " DEREFERENCED NOTHING" : "");
  } else if (is_insert(event.m_kind)) {
    os << (event.m_result ? "true" : "false");
  } else if (event.m_result == NONE) {
    os << "null";
  } else {
    os << event.m_result;
  }
  return os.str();
}

bool g_trace_ops{};

void trace_op(const Event& event, const char* what) {
  if (g_trace_ops) [[unlikely]] {
    std::fprintf(stderr, "%s %s\n", what, to_string(event).c_str());
  }
}

struct Run_result {
  bool m_completed{};
  uint64_t m_steps{};
  std::vector<Event> m_history{};
  /* Items owned by each thread when it finished. */
  std::vector<std::vector<int>> m_owned{};

  /* Observations made once all the threads are done. */
  std::vector<int> m_forward{};
  std::vector<int> m_backward{};
  bool m_forward_ok{};
  bool m_backward_ok{};
  size_t m_size{};
  bool m_consistent{true};
  std::vector<int> m_drained{};
  bool m_all_unlinked_after_drain{};
};

template <typename Iterator>
bool collect(Iterator first, Iterator last, size_t limit, std::vector<int>& out) {
  try {
    for (; first != last; ++first) {
      if (out.size() > limit) {
        return false;
      }
      /* A broken list can hand out an iterator to nothing. Report it
      instead of crashing. */
      const auto ptr = first.operator->();

      if (ptr == nullptr) {
        return false;
      }
      out.push_back(ptr->m_id);
    }
  } catch (const ut::Iterator_invalidated&) {
    return false;
  }
  return true;
}

/** Execute the program under the schedule determined by the seed. */
Run_result execute(const Program& program, uint64_t schedule_seed) {
  const auto n_items = static_cast<size_t>(program.m_n_items);
  const auto n_threads = program.m_threads.size();

  std::vector<Item> buffer(n_items);
  for (size_t i = 0; i < n_items; ++i) {
    buffer[i].m_id = static_cast<int>(i);
  }

  List list(buffer.data(), buffer.data() + buffer.size());
  Run_result result;

  std::vector<bool> in_initial(n_items, false);
  for (auto id : program.m_initial) {
    in_initial[static_cast<size_t>(id)] = true;
    if (!list.push_back(buffer[static_cast<size_t>(id)])) {
      throw std::logic_error("failed to set up the initial list");
    }
  }

  result.m_owned.resize(n_threads);
  for (size_t i = 0; i < n_items; ++i) {
    if (!in_initial[i]) {
      result.m_owned[i % n_threads].push_back(static_cast<int>(i));
    }
  }

  static const auto max_steps = env_or("LF_DST_MAX_STEPS", 100'000);
  sim::Simulation simulation({schedule_seed, max_steps});
  const size_t iteration_limit = 4 * n_items + 16;
  std::vector<std::function<void()>> tasks;

  for (size_t t = 0; t < n_threads; ++t) {
    tasks.emplace_back([&, t]() {
      auto& owned = result.m_owned[t];

      for (const auto& op : program.m_threads[t]) {
        Event event;

        event.m_thread = static_cast<int>(t);
        event.m_kind = op.m_kind;
        event.m_target = op.m_target;

        if (is_insert(op.m_kind)) {
          if (owned.empty()) {
            continue;
          }
          event.m_item = owned.back();
        }

        auto target = has_target(op.m_kind) ? &buffer[static_cast<size_t>(op.m_target)] : nullptr;
        auto item = event.m_item != NONE ? &buffer[static_cast<size_t>(event.m_item)] : nullptr;
        auto id_of = [](Item* ptr) { return ptr != nullptr ? ptr->m_id : NONE; };

        event.m_invoke = simulation.tick();
        trace_op(event, "begin");

        switch (op.m_kind) {
          case Kind::push_front:
            event.m_result = list.push_front(*item);
            break;
          case Kind::push_back:
            event.m_result = list.push_back(*item);
            break;
          case Kind::insert_after:
            event.m_result = list.insert_after(*target, *item);
            break;
          case Kind::insert_before:
            event.m_result = list.insert_before(*target, *item);
            break;
          case Kind::remove:
            event.m_result = id_of(list.remove(*target));
            break;
          case Kind::pop_front:
            event.m_result = id_of(list.pop_front());
            break;
          case Kind::pop_back:
            event.m_result = id_of(list.pop_back());
            break;
          case Kind::find:
            event.m_result = id_of(list.find([id = op.m_target](const Item* ptr) { return ptr->m_id == id; }));
            break;
          case Kind::iterate_forward:
            try {
              for (auto it = list.begin(); it != list.end(); ++it) {
                if (event.m_seq.size() > iteration_limit) {
                  event.m_overflow = true;
                  break;
                }
                const auto ptr = it.operator->();

                if (ptr == nullptr) {
                  event.m_null_item = true;
                  break;
                }
                event.m_seq.push_back(ptr->m_id);
              }
            } catch (const ut::Iterator_invalidated&) {
              event.m_threw = true;
            }
            break;
          case Kind::iterate_backward:
            try {
              for (auto it = list.rbegin(); it != list.rend(); ++it) {
                if (event.m_seq.size() > iteration_limit) {
                  event.m_overflow = true;
                  break;
                }
                const auto ptr = it.operator->();

                if (ptr == nullptr) {
                  event.m_null_item = true;
                  break;
                }
                event.m_seq.push_back(ptr->m_id);
              }
            } catch (const ut::Iterator_invalidated&) {
              event.m_threw = true;
            }
            break;
          case Kind::count:
            break;
        }

        event.m_response = simulation.tick();
        trace_op(event, "end");

        if (is_insert(op.m_kind) && event.m_result) {
          owned.pop_back();
        } else if (is_removal(op.m_kind) && event.m_result != NONE) {
          owned.push_back(event.m_result);
        }

        result.m_history.push_back(std::move(event));
      }
    });
  }

  result.m_completed = simulation.run(std::move(tasks)) == sim::Simulation::Outcome::completed;
  result.m_steps = simulation.steps();

  if (!result.m_completed) {
    return result;
  }

  /* All threads are done, the list is quiescent. */
  result.m_forward_ok = collect(list.begin(), list.end(), n_items, result.m_forward);
  result.m_backward_ok = collect(list.rbegin(), list.rend(), n_items, result.m_backward);
  result.m_size = list.size();

  /* The prev links are hints that the list validates before it uses them,
  whether they are exact can't be observed through the regular interface. */
  if constexpr (requires { list.is_consistent(); }) {
    result.m_consistent = list.is_consistent();
  }

  for (size_t i = 0; i <= n_items; ++i) {
    auto ptr = list.pop_front();
    if (ptr == nullptr) {
      break;
    }
    result.m_drained.push_back(ptr->m_id);
  }

  result.m_all_unlinked_after_drain = std::all_of(buffer.begin(), buffer.end(), [](Item& entry) { return entry.m_node.is_null(); });

  return result;
}

/* ------------------------------------------------------------------------ */
/* Linearizability checker (Wing & Gong / Lowe style search with memoization) */
/* ------------------------------------------------------------------------ */

using Model = std::vector<int>;

/** Apply the event to the sequential model.
@return false if the recorded result is impossible in this state. */
bool apply(Model& model, const Event& event) {
  auto find = [&](int id) { return std::find(model.begin(), model.end(), id); };

  switch (event.m_kind) {
    case Kind::push_front:
    case Kind::push_back:
      /* The caller owns the item, the insert must succeed. */
      if (!event.m_result || find(event.m_item) != model.end()) {
        return false;
      }
      model.insert(event.m_kind == Kind::push_front ? model.begin() : model.end(), event.m_item);
      return true;

    case Kind::insert_after:
    case Kind::insert_before: {
      auto it = find(event.m_target);
      const bool expected = it != model.end();

      if (static_cast<bool>(event.m_result) != expected) {
        return false;
      }
      if (expected) {
        if (find(event.m_item) != model.end()) {
          return false;
        }
        model.insert(event.m_kind == Kind::insert_after ? it + 1 : it, event.m_item);
      }
      return true;
    }

    case Kind::remove: {
      auto it = find(event.m_target);

      if (it == model.end()) {
        return event.m_result == NONE;
      }
      if (event.m_result != event.m_target) {
        return false;
      }
      model.erase(it);
      return true;
    }

    case Kind::pop_front:
      if (model.empty()) {
        return event.m_result == NONE;
      }
      if (event.m_result != model.front()) {
        return false;
      }
      model.erase(model.begin());
      return true;

    case Kind::pop_back:
      if (model.empty()) {
        return event.m_result == NONE;
      }
      if (event.m_result != model.back()) {
        return false;
      }
      model.pop_back();
      return true;

    case Kind::find:
      return event.m_result == (find(event.m_target) != model.end() ? event.m_target : NONE);

    default:
      return true;
  }
}

struct Linearizability_checker {
  Linearizability_checker(std::vector<const Event*> events, Model final_state)
    : m_events(std::move(events)), m_final(std::move(final_state)) {}

  [[nodiscard]] bool check(const Model& initial) {
    if (m_events.size() > 64) {
      throw std::logic_error("history too long for the checker");
    }
    return search(0, initial);
  }

private:
  bool search(uint64_t done, const Model& model) {
    const auto n = m_events.size();

    if (done == (n == 64 ? ~0ULL : (1ULL << n) - 1)) {
      return model == m_final;
    }

    if (!m_visited.emplace(done, model).second) {
      return false;
    }

    /* An operation can be linearized next only if no other pending
    operation completed before it was invoked. */
    uint64_t min_response = ~0ULL;

    for (size_t i = 0; i < n; ++i) {
      if (!(done & (1ULL << i))) {
        min_response = std::min(min_response, m_events[i]->m_response);
      }
    }

    for (size_t i = 0; i < n; ++i) {
      if ((done & (1ULL << i)) || m_events[i]->m_invoke > min_response) {
        continue;
      }

      Model next = model;

      if (apply(next, *m_events[i]) && search(done | (1ULL << i), next)) {
        return true;
      }
    }
    return false;
  }

  std::vector<const Event*> m_events;
  Model m_final;
  std::set<std::pair<uint64_t, Model>> m_visited{};
};

bool has_duplicates(std::vector<int> values) {
  std::sort(values.begin(), values.end());
  return std::adjacent_find(values.begin(), values.end()) != values.end();
}

bool is_subsequence(const std::vector<int>& needle, const std::vector<int>& haystack) {
  size_t i{};

  for (auto value : haystack) {
    if (i < needle.size() && needle[i] == value) {
      ++i;
    }
  }
  return i == needle.size();
}

std::string join(const std::vector<int>& values) {
  std::ostringstream os;

  os << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    os << (i ? ", " : "") << values[i];
  }
  os << "]";
  return os.str();
}

/** Check all the properties.
@return a description of the violated property, empty if all of them hold. */
std::string check(const Program& program, const Run_result& result) {
  if (!result.m_completed) {
    return "no progress: step budget exhausted (livelock or unbounded loop)";
  }

  /* Quiescent consistency. */
  if (!result.m_forward_ok) {
    return "forward traversal of the quiescent list did not terminate cleanly: " + join(result.m_forward);
  }
  if (has_duplicates(result.m_forward)) {
    return "forward traversal of the quiescent list has duplicates: " + join(result.m_forward);
  }
  if (!result.m_backward_ok) {
    return "backward traversal of the quiescent list did not terminate cleanly: " + join(result.m_backward) + ", forward: " + join(result.m_forward);
  }
  {
    auto reversed = result.m_backward;
    std::reverse(reversed.begin(), reversed.end());

    if (reversed != result.m_forward) {
      return "forward and backward traversals disagree: forward " + join(result.m_forward) + ", backward (reversed) " + join(reversed);
    }
  }
  if (result.m_size != result.m_forward.size()) {
    return "size() is " + std::to_string(result.m_size) + " but the list has " + std::to_string(result.m_forward.size()) + " items";
  }

  if (!result.m_consistent) {
    return "is_consistent() is false for the quiescent list (stale prev link or an operation that is half done), list: " + join(result.m_forward);
  }

  /* Conservation of items. */
  {
    std::vector<int> all = result.m_forward;

    for (const auto& owned : result.m_owned) {
      all.insert(all.end(), owned.begin(), owned.end());
    }
    std::sort(all.begin(), all.end());

    std::vector<int> expected(static_cast<size_t>(program.m_n_items));
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<int>(i);
    }

    if (all != expected) {
      return "items lost or duplicated: list + owned = " + join(all);
    }
  }

  if (result.m_drained != result.m_forward) {
    return "draining with pop_front() returned " + join(result.m_drained) + " but the list was " + join(result.m_forward);
  }
  if (!result.m_all_unlinked_after_drain) {
    return "a node is still linked after the list was drained";
  }

  /* Concurrent iteration. */
  bool insert_only{true};

  for (const auto& event : result.m_history) {
    insert_only = insert_only && !is_removal(event.m_kind);
  }

  for (const auto& event : result.m_history) {
    if (!is_iteration(event.m_kind)) {
      continue;
    }

    if (event.m_overflow) {
      return "iteration did not terminate: " + to_string(event);
    }
    if (event.m_null_item) {
      return "iterator that is not at the end points to nothing: " + to_string(event);
    }

    for (auto id : event.m_seq) {
      if (id < 0 || id >= program.m_n_items) {
        return "iteration returned garbage: " + to_string(event);
      }
    }

    if (!insert_only) {
      continue;
    }

    /* Without removals an iteration must be an exact, in order, view of
    the items that were present when it started, plus optionally some of
    the items that were inserted while it was running. */
    if (event.m_threw) {
      return "iterator invalidated without any removal: " + to_string(event);
    }

    auto seq = event.m_seq;

    if (event.m_kind == Kind::iterate_backward) {
      std::reverse(seq.begin(), seq.end());
    }

    if (has_duplicates(seq) || !is_subsequence(seq, result.m_forward)) {
      return "iteration is not an in-order view of the list: " + to_string(event) + ", final list " + join(result.m_forward);
    }

    std::vector<int> present = program.m_initial;

    for (const auto& other : result.m_history) {
      if (is_insert(other.m_kind) && other.m_result && other.m_response < event.m_invoke) {
        present.push_back(other.m_item);
      }
    }

    for (auto id : present) {
      if (std::find(seq.begin(), seq.end(), id) == seq.end()) {
        return "iteration missed item " + std::to_string(id) + " that was in the list before it started: " + to_string(event);
      }
    }
  }

  /* Linearizability. */
  std::vector<const Event*> events;

  for (const auto& event : result.m_history) {
    if (!is_iteration(event.m_kind)) {
      events.push_back(&event);
    }
  }

  Linearizability_checker checker(std::move(events), result.m_forward);

  if (!checker.check(program.m_initial)) {
    return "history is not linearizable (final list " + join(result.m_forward) + ")";
  }

  return {};
}

/* ------------------------------------------------------------------------ */
/* Generation, shrinking and the property driver                              */
/* ------------------------------------------------------------------------ */

struct Profile {
  const char* m_name{};
  /* Relative weight of each operation kind. */
  uint32_t m_weights[static_cast<size_t>(Kind::count)]{};
  uint32_t m_max_threads{4};
  uint32_t m_max_ops{5};
  uint32_t m_max_initial{5};
};

Program generate(sim::Rng& rng, const Profile& profile) {
  Program program;

  const auto n_threads = rng.range(2, profile.m_max_threads);
  const auto n_initial = rng.range(0, profile.m_max_initial);
  const auto n_free = rng.range(1, n_threads + 2);

  program.m_n_items = static_cast<int>(n_initial + n_free);

  for (uint64_t i = 0; i < n_initial; ++i) {
    program.m_initial.push_back(static_cast<int>(i));
  }

  uint32_t total_weight{};

  for (auto weight : profile.m_weights) {
    total_weight += weight;
  }

  program.m_threads.resize(n_threads);

  for (auto& ops : program.m_threads) {
    for (auto n = rng.range(1, profile.m_max_ops); n > 0; --n) {
      Op op;
      auto pick = rng.below(total_weight);

      for (size_t k = 0; k < static_cast<size_t>(Kind::count); ++k) {
        if (pick < profile.m_weights[k]) {
          op.m_kind = static_cast<Kind>(k);
          break;
        }
        pick -= profile.m_weights[k];
      }

      if (has_target(op.m_kind)) {
        /* Prefer items that start out in the list, that is where the
        interesting conflicts are. */
        if (n_initial > 0 && rng.chance(3, 4)) {
          op.m_target = static_cast<int>(rng.below(n_initial));
        } else {
          op.m_target = static_cast<int>(rng.below(static_cast<uint64_t>(program.m_n_items)));
        }
      }
      ops.push_back(op);
    }
  }
  return program;
}

constexpr uint64_t DEFAULT_SEED = 0x5eed'1157'd15c'0de5ULL;
constexpr uint32_t SCHEDULES_PER_PROGRAM = 8;

struct Failure {
  Program m_program{};
  uint64_t m_schedule_seed{};
  std::string m_error{};
};

/* The run in progress, for the crash handler. */
const Program* g_current_program{};
uint64_t g_current_seed{};

/** If the list crashes or trips an assertion, say which run did it. */
void report_crash(int sig) {
  if (g_current_program != nullptr) {
    std::fprintf(stderr, "\n*** signal %d while running schedule seed %llu of:\n%s", sig,
                 static_cast<unsigned long long>(g_current_seed), to_string(*g_current_program).c_str());
  }
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

const bool g_crash_handler_installed = [] {
  std::signal(SIGABRT, report_crash);
  std::signal(SIGSEGV, report_crash);
  return true;
}();

/** Try a number of schedules, starting with the given one.
@return true if one of them violates a property. */
bool fails(const Program& program, uint64_t schedule_seed, uint32_t attempts, Failure& failure) {
  for (uint32_t i = 0; i < attempts; ++i) {
    const auto seed = i == 0 ? schedule_seed : sim::mix(schedule_seed, i);

    g_current_program = &program;
    g_current_seed = seed;

    const bool trace_all = env_or("LF_DST_TRACE", 0) > 1;

    if (trace_all) {
      std::fprintf(stderr, "--- trace: schedule seed %llu ---\n%s", static_cast<unsigned long long>(seed), to_string(program).c_str());
      sim::Simulation::set_trace(true);
      g_trace_ops = true;
    }

    auto error = check(program, execute(program, seed));

    if (trace_all) {
      sim::Simulation::set_trace(false);
      g_trace_ops = false;
    }
    g_current_program = nullptr;

    if (!error.empty()) {
      failure = Failure{program, seed, std::move(error)};
      return true;
    }
  }
  return false;
}

/** Greedily remove threads, operations and initial items for as long as
some schedule still fails. */
Failure shrink(Failure failure) {
  constexpr uint32_t ATTEMPTS = 400;

  for (bool progress = true; progress;) {
    progress = false;

    auto try_candidate = [&](Program candidate) {
      Failure smaller;

      if (fails(candidate, failure.m_schedule_seed, ATTEMPTS, smaller)) {
        failure = std::move(smaller);
        progress = true;
        return true;
      }
      return false;
    };

    for (size_t t = 0; t < failure.m_program.m_threads.size() && failure.m_program.m_threads.size() > 1;) {
      auto candidate = failure.m_program;

      candidate.m_threads.erase(candidate.m_threads.begin() + static_cast<std::ptrdiff_t>(t));
      if (!try_candidate(std::move(candidate))) {
        ++t;
      }
    }

    for (size_t t = 0; t < failure.m_program.m_threads.size(); ++t) {
      for (size_t i = 0; i < failure.m_program.m_threads[t].size();) {
        auto candidate = failure.m_program;

        candidate.m_threads[t].erase(candidate.m_threads[t].begin() + static_cast<std::ptrdiff_t>(i));
        if (!try_candidate(std::move(candidate))) {
          ++i;
        }
      }
    }

    for (size_t i = 0; i < failure.m_program.m_initial.size();) {
      auto candidate = failure.m_program;

      candidate.m_initial.erase(candidate.m_initial.begin() + static_cast<std::ptrdiff_t>(i));
      if (!try_candidate(std::move(candidate))) {
        ++i;
      }
    }
  }
  return failure;
}

std::string describe(const Failure& failure) {
  std::ostringstream os;

  os << failure.m_error << "\n"
     << "schedule seed: " << failure.m_schedule_seed << "\n"
     << to_string(failure.m_program);

  /* LF_DST_TRACE=1 prints every atomic step of the failing run. */
  const bool trace = env_or("LF_DST_TRACE", 0) != 0;

  if (trace) {
    std::fprintf(stderr, "--- trace: schedule seed %llu ---\n%s", static_cast<unsigned long long>(failure.m_schedule_seed), to_string(failure.m_program).c_str());
  }

  sim::Simulation::set_trace(trace);
  g_trace_ops = trace;
  const auto result = execute(failure.m_program, failure.m_schedule_seed);
  sim::Simulation::set_trace(false);
  g_trace_ops = false;

  os << "history (" << result.m_steps << " atomic steps):\n";
  for (const auto& event : result.m_history) {
    os << "  " << to_string(event) << "\n";
  }
  if (result.m_completed) {
    os << "final list: forward " << join(result.m_forward) << ", backward " << join(result.m_backward) << ", size() " << result.m_size << "\n";
  }
  return os.str();
}

/** Explore many schedules of one fixed program. */
void explore(const Program& program, uint32_t default_schedules) {
  const auto base = env_or("LF_DST_SEED", DEFAULT_SEED);
  const auto schedules = static_cast<uint32_t>(env_or("LF_DST_SCHEDULES", default_schedules));

  for (uint32_t i = 0; i < schedules; ++i) {
    Failure failure;

    if (fails(program, sim::mix(base, i), 1, failure)) {
      FAIL() << "schedule " << i << " of " << schedules << " (LF_DST_SEED=" << base << ")\n"
             << "--- failing run ---\n" << describe(failure)
             << "--- shrunk ---\n" << describe(shrink(failure));
    }
  }
}

/** Generate random programs and explore a few schedules of each. */
void run_property(const Profile& profile, uint64_t default_cases) {
  const auto base = env_or("LF_DST_SEED", DEFAULT_SEED);
  const auto cases = env_or("LF_DST_CASES", default_cases);
  uint64_t runs{};

  const auto first = env_or("LF_DST_FIRST_CASE", 0);

  for (uint64_t i = first; i < first + cases; ++i) {
    const auto case_seed = sim::mix(base, i);
    sim::Rng rng(case_seed);
    const auto program = generate(rng, profile);
    Failure failure;

    runs += SCHEDULES_PER_PROGRAM;

    if (fails(program, case_seed, SCHEDULES_PER_PROGRAM, failure)) {
      FAIL() << profile.m_name << ": case " << i << " (LF_DST_SEED=" << base << " LF_DST_FIRST_CASE=" << i << " LF_DST_CASES=1)\n"
             << "--- failing run ---\n" << describe(failure)
             << "--- shrunk ---\n" << describe(shrink(failure));
    }
  }

  ::testing::Test::RecordProperty("runs", static_cast<int>(runs));
}

Program make_program(int n_items, std::vector<int> initial, std::vector<std::vector<Op>> threads) {
  return Program{n_items, std::move(initial), std::move(threads)};
}

/* ------------------------------------------------------------------------ */
/* The simulation itself                                                      */
/* ------------------------------------------------------------------------ */

TEST(Simulation_test, same_seed_same_execution) {
  sim::Rng rng(42);
  Profile profile{"determinism", {2, 2, 2, 2, 3, 2, 2, 1, 1, 1}};

  for (int i = 0; i < 200; ++i) {
    const auto program = generate(rng, profile);
    const auto seed = rng.next();
    const auto a = execute(program, seed);
    const auto b = execute(program, seed);

    ASSERT_EQ(a.m_steps, b.m_steps);
    ASSERT_EQ(a.m_forward, b.m_forward);
    ASSERT_EQ(a.m_history.size(), b.m_history.size());

    for (size_t k = 0; k < a.m_history.size(); ++k) {
      ASSERT_EQ(to_string(a.m_history[k]), to_string(b.m_history[k]));
    }
  }
}

TEST(Simulation_test, schedules_differ_across_seeds) {
  /* Two threads pushing to the front: both orders must be explored. */
  const auto program = make_program(2, {}, {{{Kind::push_front}}, {{Kind::push_front}}});
  std::set<std::vector<int>> outcomes;

  for (uint64_t seed = 0; seed < 200; ++seed) {
    outcomes.insert(execute(program, seed).m_forward);
  }

  EXPECT_EQ(outcomes.size(), 2U);
}

TEST(Simulation_test, checker_rejects_non_linearizable_history) {
  /* pop_front() returned the second item of [0, 1] with nothing else
  running: no sequential execution can explain that. */
  Event event;

  event.m_kind = Kind::pop_front;
  event.m_result = 1;
  event.m_invoke = 1;
  event.m_response = 2;

  EXPECT_FALSE(Linearizability_checker({&event}, {0}).check({0, 1}));

  event.m_result = 0;
  EXPECT_TRUE(Linearizability_checker({&event}, {1}).check({0, 1}));
}

/* ------------------------------------------------------------------------ */
/* Directed scenarios: fixed programs, many schedules                         */
/* ------------------------------------------------------------------------ */

/* The scenario of Multi_threaded_list_test.concurrent_remove_inconsistency_bug:
one thread removes nodes while another inserts before their successors. */
TEST(List_scenario_test, remove_vs_insert_before_neighbour) {
  explore(make_program(8, {0, 1, 2, 3, 4, 5},
                       {{{Kind::remove, 2}, {Kind::remove, 3}},
                        {{Kind::insert_before, 3}, {Kind::insert_before, 4}}}),
          20'000);
}

TEST(List_scenario_test, remove_adjacent_nodes) {
  explore(make_program(5, {0, 1, 2, 3, 4},
                       {{{Kind::remove, 1}}, {{Kind::remove, 2}}, {{Kind::remove, 3}}}),
          20'000);
}

TEST(List_scenario_test, concurrent_push_both_ends) {
  explore(make_program(4, {},
                       {{{Kind::push_front}, {Kind::push_front}},
                        {{Kind::push_back}, {Kind::push_back}}}),
          20'000);
}

TEST(List_scenario_test, pop_front_vs_insert_before_first) {
  explore(make_program(4, {0, 1},
                       {{{Kind::pop_front}}, {{Kind::insert_before, 0}}, {{Kind::remove, 0}}}),
          20'000);
}

TEST(List_scenario_test, pop_back_vs_insert_after_last) {
  explore(make_program(4, {0, 1},
                       {{{Kind::pop_back}}, {{Kind::insert_after, 1}}, {{Kind::remove, 1}}}),
          20'000);
}

TEST(List_scenario_test, pop_both_ends_of_short_list) {
  explore(make_program(3, {0, 1},
                       {{{Kind::pop_front}, {Kind::pop_front}},
                        {{Kind::pop_back}, {Kind::pop_back}},
                        {{Kind::push_back}, {Kind::pop_front}}}),
          20'000);
}

/* Items are continuously removed and re-inserted, which is what a free list
or an LRU does. This is the ABA torture test. */
TEST(List_scenario_test, recycle_items_as_queue) {
  const std::vector<Op> rotate = {{Kind::pop_front}, {Kind::push_back}, {Kind::pop_front}, {Kind::push_back}};

  explore(make_program(3, {0, 1, 2}, {rotate, rotate, rotate}), 20'000);
}

TEST(List_scenario_test, recycle_items_as_stack) {
  const std::vector<Op> churn = {{Kind::pop_back}, {Kind::push_back}, {Kind::pop_back}, {Kind::push_front}};

  explore(make_program(2, {0, 1}, {churn, churn, {{Kind::remove, 0}, {Kind::insert_after, 1}, {Kind::remove, 1}}}), 20'000);
}

TEST(List_scenario_test, remove_and_reinsert_same_item) {
  explore(make_program(3, {0, 1, 2},
                       {{{Kind::remove, 1}, {Kind::insert_after, 0}, {Kind::remove, 1}, {Kind::insert_before, 2}},
                        {{Kind::remove, 1}, {Kind::push_front}, {Kind::insert_before, 1}},
                        {{Kind::insert_after, 1}, {Kind::find, 1}}}),
          20'000);
}

/* An insert_before() whose target dies before the new node is linked takes
effect just before the target died. Thread 2 first observes that the target
(0) is gone and then looks at the new node (1, owned by thread 1) through its
handle. Whatever it sees must be consistent with that order. */
TEST(List_scenario_test, insert_before_dying_target_observed_via_new_item) {
  explore(make_program(3, {0},
                       {{{Kind::remove, 0}},
                        {{Kind::insert_before, 0}},
                        {{Kind::insert_after, 0}, {Kind::remove, 1}}}),
          200'000);

  explore(make_program(4, {3, 0},
                       {{{Kind::pop_back}},
                        {{Kind::insert_before, 0}},
                        {{Kind::find, 0}, {Kind::insert_after, 1}, {Kind::remove, 1}}}),
          200'000);
}

/* Stale threads must not leave a wrong prev link behind: several structural
changes next to the same node, in quick succession. */
TEST(List_scenario_test, prev_links_are_exact_when_quiescent) {
  explore(make_program(6, {0, 1, 2},
                       {{{Kind::insert_before, 1}, {Kind::remove, 0}},
                        {{Kind::insert_before, 1}, {Kind::insert_after, 0}},
                        {{Kind::remove, 0}, {Kind::insert_before, 1}, {Kind::pop_front}}}),
          100'000);
}

/* ------------------------------------------------------------------------ */
/* Properties over randomly generated programs                                */
/* ------------------------------------------------------------------------ */

/*                                        pf pb ia ib rm pF pB fi it ir */
TEST(List_property_test, all_operations) {
  run_property({"all_operations", {2, 2, 3, 3, 4, 2, 2, 1, 1, 1}}, 20'000);
}

TEST(List_property_test, mutators_only) {
  run_property({"mutators_only", {2, 2, 3, 3, 4, 2, 2, 0, 0, 0}}, 20'000);
}

TEST(List_property_test, removal_heavy) {
  run_property({"removal_heavy", {1, 1, 1, 1, 5, 3, 3, 1, 0, 0}, 4, 5, 6}, 20'000);
}

TEST(List_property_test, queue_and_stack) {
  run_property({"queue_and_stack", {3, 3, 0, 0, 0, 3, 3, 0, 0, 0}, 4, 6, 3}, 20'000);
}

TEST(List_property_test, inserts_with_concurrent_iteration) {
  run_property({"inserts_with_concurrent_iteration", {2, 2, 3, 3, 0, 0, 0, 1, 3, 3}}, 20'000);
}

TEST(List_property_test, two_threads_long) {
  run_property({"two_threads_long", {2, 2, 3, 3, 4, 2, 2, 1, 0, 0}, 2, 10, 4}, 10'000);
}

} // namespace
