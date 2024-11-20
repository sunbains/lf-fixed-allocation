# Lock-free Doubly Linked List Design Document

## 1. Architecture Overview

### 1.1 Core Components

```mermaid
classDiagram
    class Node {
        +atomic<uint64_t> m_links
        +is_null() bool
        +is_valid() bool
    }

    class List {
        +atomic<Link_type> m_head
        +atomic<Link_type> m_tail
        +pair<item_pointer, item_pointer> m_bounds
        +push_front(item) bool
        +push_back(item) bool
        +insert_after(item, new_item) bool
        +insert_before(item, new_item) bool
        +remove(item) optional
        +find(predicate) pointer
    }

    class List_iterator {
        +m_base pointer
        +m_prev node_pointer
        +m_current node_pointer
        +operator++()
        +operator--()
    }

    List *-- Node
    List *-- List_iterator
```

### 1.2 Memory Layout

```mermaid
graph TD
    subgraph List Object
        H[Head atomic ptr] --> N1
        T[Tail atomic ptr] --> N3
    end
    
    subgraph User Buffer
        N1[Node 1] -- next --> N2[Node 2]
        N2 -- next --> N3[Node 3]
        N3 -- prev --> N2
        N2 -- prev --> N1
    end

    subgraph Node Structure
        L[64-bit atomic links]
        L -- upper 32 bits --> NP[Next Pointer]
        L -- lower 32 bits --> PP[Prev Pointer]
    end
```

## 2. Operation Linearization Points

### 2.1 Push Front

```mermaid
sequenceDiagram
    participant T as Thread
    participant H as Head
    participant N as New Node
    participant OH as Old Head

    Note over T,OH: Linearization Point 1: CAS on Head
    T->>N: Initialize links
    T->>H: CAS(old_head, new_node)
    alt CAS Success
        T->>OH: Update prev pointer
        Note over T,OH: Linearization Point 2: CAS on Old Head links
    else CAS Failure
        T->>T: Retry operation
    end
```

**Linearization Points:**
1. Successful CAS on head pointer
2. For non-empty list: Successful CAS on old head's links

### 2.2 Remove Operation

```mermaid
sequenceDiagram
    participant T as Thread
    participant N as Node to Remove
    participant P as Prev Node
    participant NX as Next Node

    T->>N: Read links
    alt Node is head
        T->>H: CAS head to next
    end
    alt Node is tail
        T->>T: CAS tail to prev
    end
    Note over T,N: Linearization Point: CAS node links to NULL
    T->>N: CAS links to NULL_LINK
    alt CAS Success
        T->>P: Update next pointer
        T->>NX: Update prev pointer
    else CAS Failure
        T->>T: Retry operation
    end
```

**Linearization Point:** Successful CAS of node links to NULL_LINK

## 3. Control Flow Analysis

### 3.1 Insert After Operation

```mermaid
graph TD
    Start[Start] --> ReadLinks[Read target node links]
    ReadLinks --> ValidateNode{Node valid?}
    ValidateNode -- No --> Fail[Return false]
    ValidateNode -- Yes --> SetNewLinks[Set new node links]
    SetNewLinks --> CASTarget[CAS target node links]
    CASTarget -- Success --> UpdateNext[Update next node]
    CASTarget -- Failure --> Retry{Retry < max?}
    Retry -- Yes --> ReadLinks
    Retry -- No --> Fail
    UpdateNext -- Success --> Success[Return true]
    UpdateNext -- Failure --> RestoreLinks[Restore original links]
    RestoreLinks --> Retry
```

### 3.2 Iterator Progression

```mermaid
graph TD
    Start[Start] --> ReadCurrent[Read current node links]
    ReadCurrent --> ValidateLinks{Valid links?}
    ValidateLinks -- No --> RecoveryLoop[Enter recovery loop]
    ValidateLinks -- Yes --> UpdatePrev[Update prev pointer]
    RecoveryLoop --> FindValidNode[Find next valid node]
    FindValidNode --> CheckRetries{Max retries?}
    CheckRetries -- Yes --> Throw[Throw iterator invalid]
    CheckRetries -- No --> ReadCurrent
    UpdatePrev --> MoveCurrent[Move to next node]
    MoveCurrent --> End[End]
```

## 4. Critical Path Analysis

### 4.1 Lock-free Property Verification

Each operation maintains the lock-free property by ensuring:
1. No operation holds locks
2. Failed CAS operations don't prevent progress
3. Retry loops have maximum iteration limits
4. System-wide progress is guaranteed

### 4.2 Memory Ordering Requirements

```cpp
// Required memory ordering for each atomic operation
Operation                  | Store         | Load          | CAS
--------------------------|---------------|---------------|----------------
Read node links           | -             | Acquire       | -
Update node links         | Release       | -             | AcqRel
Update head/tail          | Release       | Acquire       | AcqRel
Validate node            | -             | Acquire       | -
```

## 5. ABA Prevention Analysis

### 5.1 Link Structure
```cpp
64-bit atomic word:
[63:32] Next pointer
[31:0]  Prev pointer
```

**ABA Prevention Mechanism:**
1. Combined atomic updates of both pointers
2. Unique link combinations for each state
3. No pointer reuse during node lifetime

### 5.2 State Transitions

```mermaid
stateDiagram-v2
    [*] --> Valid: Initialize
    Valid --> Removed: CAS to NULL_LINK
    Removed --> [*]: Node cleanup
    Valid --> Modified: CAS new links
    Modified --> Modified: CAS new links
    Modified --> Removed: CAS to NULL_LINK
```

## 6. Performance Considerations

### 6.1 Cache Line Optimization

```
Node Layout:
+------------------+
| m_links (8 bytes)|
+------------------+
Aligned to prevent false sharing
```

### 6.2 Critical Paths

High-contention scenarios:
1. Head modifications (push_front)
2. Tail modifications (push_back)
3. Adjacent node removals
4. Iterator recovery paths

### 6.3 Memory Access Patterns

```mermaid
graph TD
    subgraph Sequential Access
        SA[Read Node] --> SB[Next Node] --> SC[Next Node]
    end
    
    subgraph Random Access
        RA[Find Target] --> RB[Update Links] --> RC[Update Adjacent]
    end
```

## 7. Consistency Guidelines

### 7.1 Invariants
1. Head has no prev pointer
2. Tail has no next pointer
3. Non-removed nodes form continuous chain
4. All node links are bi-directional

### 7.2 Recovery Mechanisms
1. Iterator recovery from removed nodes
2. Link validation during traversal
3. Atomic state restoration on failure

## 8. Testing Strategy

### 8.1 Test Categories
1. Basic Operations
2. Concurrent Operations
3. Edge Cases
4. Stress Tests
5. Performance Benchmarks

### 8.2 Verification Methods
1. Linearization point analysis
2. Memory ordering verification
3. State transition validation
4. Progress guarantee verification
