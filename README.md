# Lock-free Doubly Linked List

A high-performance lock-free doubly linked list implementation in modern C++. This implementation provides thread-safe operations without using traditional locks or mutexes, instead utilizing atomic operations and compare-and-swap (CAS) mechanisms.

## Features

- **Lock-free Operations**: No locks; a thread that stalls in the middle of an operation never blocks the others, they complete its work for it
- **Linearizable**: Every mutating operation and `find()` takes effect atomically at a single compare-and-swap
- **Recyclable Nodes**: Items can be removed and re-inserted any number of times; every CAS target carries a modification tag to rule out ABA
- **Memory Efficient**: 16 bytes per node, no allocation, nodes are addressed by their index in a user provided buffer
- **Bidirectional Iteration**: Weakly consistent forward and reverse iterators
- **Modern C++**: Written in C++23

## Operations

- `push_front()`, `push_back()`: Add an item at either end
- `insert_after()`, `insert_before()`: Insert an item next to one that is in the list; returns `false` if it is not
- `remove()`: Remove an item; returns `nullptr` if it is not in the list
- `pop_front()`, `pop_back()`: Remove and return the item at either end
- `find()`: Find an item using a predicate
- `size()`: Number of items (exact when no operation is in progress)
- Bidirectional iteration

When `remove()` or `pop_*()` return an item, the caller owns it exclusively and
can re-insert it right away. Inserting an item that is already in a list fails.

Iterators are weakly consistent. If the element that an iterator points to is
removed concurrently, advancing the iterator throws `ut::Iterator_invalidated`.

## Usage

```cpp
#include "ut/lock_free_list.h"

// Define your data structure
struct My_data {
    ut::Node& node() noexcept { return m_node; }

    int m_value{};
    ut::Node m_node{};
};

// Create a list
std::vector<My_data> buffer(1000);
ut::List<My_data, &My_data::node> list(buffer.data(), buffer.data() + buffer.size());

// Add elements
buffer[0].m_value = 42;
list.push_back(buffer[0]);

// Find elements
auto item = list.find([](const My_data* data) {
    return data->m_value == 42;
});

// Iterate
for (const auto& data : list) {
    std::cout << data.m_value << "\n";
}
```

## Performance

- **Lock-free Design**: No mutex contention or blocking
- **No Allocation**: The list works on a buffer that the user provides
- **O(1) Operations**: At both ends and next to a known item. The prev links are hints that are validated against the next links; a search from the head is only needed if another thread stalls in the middle of an operation
- **Scales Where a List Can**: Operations on different parts of the list don't share cache lines (the item count is striped per thread) and scale with the number of threads. Operations on the same end of the list all need the same link; there the list uses randomized exponential backoff to keep the throughput steady instead of collapsing
- **Sequentially Consistent Atomics**: The algorithm relies on a total order of the atomic operations on the links

Throughput in million operations per second on a 2 x 24 core Xeon Platinum 8260, 65536 items (`./bin/benchmark-scaling`), compared with a plain doubly linked list protected by a `std::mutex`:

| Workload | Threads | Lock-free | Mutex |
|---|---|---|---|
| `middle`: remove + insert_after at random positions | 1 | 20 | 38 |
| | 16 | 59 | 1.5 |
| | 64 | 83 | 1.4 |
| `before`: remove + insert_before at random positions | 1 | 18 | 39 |
| | 64 | 84 | 1.9 |
| `lru`: remove at a random position + push_front | 1 | 22 | 47 |
| | 64 | 4.3 | 1.8 |
| `queue`: pop_front + push_back | 1 | 24 | 45 |
| | 64 | 1.2 | 2.0 |

A single list is the wrong tool for 64 threads that all work on the same end: use several lists.

### Benchmarks

The repository includes benchmarks measuring:
- Thread scaling against a mutex protected list (`benchmark-scaling`)
- Operation throughput
- Cache misses
- Branch prediction misses
- Scaling with thread count
- High contention scenarios

## Building and Testing

### Requirements

- C++23 compatible compiler (tested with GCC 12+ and Clang 16+)
- CMake 3.20 or higher
- Google Benchmark (for benchmarks)
- Google Test (for tests)

### Build Instructions

All binary files will be added to the bin/ directory.

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j

./bin/simple_tests   # single threaded unit tests
./bin/mt_tests       # stress tests with real threads
./bin/dst_tests      # deterministic simulation + property based tests
```

### Deterministic Simulation Tests

`dst_tests` compiles the list against an instrumented atomic
(`tests/sim/sim.h`) and runs randomly generated concurrent programs under
randomly generated, but fully reproducible, schedules. Every atomic access is
a scheduling point. Each run is checked for linearizability against a
sequential model, and for a number of structural invariants. Failures are
shrunk to a minimal program and printed with the seed that reproduces them.

```bash
LF_DST_SEED=12345 ./bin/dst_tests       # a different part of the search space
LF_DST_CASES=1000000 ./bin/dst_tests    # more programs per property
LF_DST_TRACE=1 ./bin/dst_tests          # print every atomic step of a failing run
```

See [docs/lock-free-list-design.md](docs/lock-free-list-design.md) for the
algorithm and the properties that are tested.

### CMake Integration

```cmake
find_package(ut_lock_free_list CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ut::lock_free_list)
```

## Implementation Details

See [docs/lock-free-list-design.md](docs/lock-free-list-design.md). In short:

1. **The next links are the truth**: A node is in the list if it is reachable
   from the head via next links, and every operation takes effect with one CAS
   on one next link. Deletion marks the node first and unlinks it afterwards;
   any thread that runs into a marked node unlinks it.

2. **The prev links are hints**: They are repaired after every change and are
   exact when the list is quiescent, but they are always validated against the
   next links before they are used.

3. **Tags**: Each link word carries a counter that is incremented on every
   modification and survives removal and re-insertion of the node, so a
   successful CAS proves that the word was not modified in between.

4. **Memory Management**: User-provided buffer for node storage, no dynamic
   allocation, links are 30 bit indexes.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

### Before submitting a PR:

1. Ensure all tests pass, including `dst_tests`
2. Run benchmarks to verify performance
3. Add tests for new functionality
4. Follow the existing code style
5. Update documentation as needed

## License

See the LICENSE file for details.

## Version History

- 1.0.0 (2024-XX-XX)
  - Initial release
  - Core lock-free operations
  - Full test coverage
  - Performance benchmarks

