# Lock-free Doubly Linked List

A high-performance lock-free doubly linked list implementation in modern C++. This implementation provides thread-safe operations without using traditional locks or mutexes, instead utilizing atomic operations and compare-and-swap (CAS) mechanisms.

## Features

- **Lock-free Operations**: All operations are lock-free, ensuring threads never block
- **Thread Safety**: Full thread-safe implementation for concurrent access
- **ABA Problem Prevention**: Uses 64-bit CAS combining next/prev pointers to prevent ABA issues
- **Memory Efficient**: No additional memory overhead per node beyond the required links
- **STL Compatible**: Provides standard bidirectional iterator interface
- **Modern C++**: Written in modern C++23 with clear, maintainable code

## Operations

- `push_front()`: Add element to the front of the list
- `push_back()`: Add element to the back of the list
- `insert_after()`: Insert element after a given node
- `insert_before()`: Insert element before a given node
- `remove()`: Remove an element from the list
- `find()`: Find an element using a predicate
- Bidirectional iteration support

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

The implementation is designed for high performance in concurrent scenarios:

- **Lock-free Design**: No mutex contentions or blocking
- **Cache-friendly**: Minimizes false sharing and cache line bouncing
- **Efficient Memory Usage**: No dynamic allocations during operations
- **Optimized CAS Operations**: Minimizes the number of CAS operations required
- **Retry Limits**: Prevents theoretical infinite loops in high contention

### Benchmarks

The repository includes comprehensive benchmarks measuring:
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

All binary files will be added to the bin/ directory. Make sure it exists.

```bash
mkdir build && cd build
cmake ..
make

# Run tests and benchmarks
./bin/<binary_name>

```

### CMake Integration

```cmake
find_package(ut_lock_free_list CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ut::lock_free_list)
```

## Implementation Details

The implementation uses several key techniques to achieve lock-free operation:

1. **Combined Link Storage**:
   - Both next and prev pointers are packed into a single 64-bit integer
   - Enables atomic updates of both links simultaneously
   - Prevents ABA problems without generation counting

2. **Memory Management**:
   - User-provided buffer for node storage
   - No dynamic allocation during operations
   - Clear ownership semantics

3. **Atomic Operations**:
   - Uses C++23 atomic operations
   - Carefully chosen memory orderings for performance
   - Minimized number of CAS operations

4. **Iterator Safety**:
   - Handles concurrent modifications
   - Recovers from removed nodes
   - Provides consistent traversal semantics

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

### Before submitting a PR:

1. Ensure all tests pass
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

