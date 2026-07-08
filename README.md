# High-Performance Multi-Core Task Scheduler & Benchmark Harness

A low-latency, lock-free task scheduling and evaluation engine built using DPDK (`rte_ring`, `rte_mempool`) primitives. This repository functions as an architectural laboratory designed to measure micro-architectural jitter, lock contention, and cache locality variations across different queue topologies, CPU waiting strategies, and packet routing algorithms.

To guarantee nanosecond-level accuracy, the entire execution hot-path adheres to a **Zero-I/O policy**, completely isolating active hardware execution states from system calls, memory allocations, or operating system interference noise.

---

## Key Architectural Dimensions

### 1. Queue Topologies
* **Centralized Shared Queue (MPMC):** A single centralized lock-free ring where all worker threads dynamically compete for tasks using structural Multi-Producer Multi-Consumer atomics. Excellent for automatic load balancing, highly prone to lock contention at scale.
* **Distributed Dedicated Queues (SPSC):** Each worker core owns an isolated Single-Producer Single-Consumer lock-free ring. Tasks are explicitly routed by the master thread, minimizing core interaction and maximizing cache line locality.

### 2. Low-Level Core Waiting Strategies
* **Pure Polling:** Worker threads spin continuously inside an aggressive lock-free dequeue loop. Offers the absolute lowest possible latency response times at the expense of 100% CPU utilization.
* **Adaptive Yield:** Executes a low-overhead hardware pause instruction (`rte_pause() / _mm_pause()`) when the ring buffer is empty, optimizing memory bus throughput and power envelopes while keeping scheduling jitter to a minimum.

### 3. Integrated Routing Algorithms
* **Round-Robin:** Strict load balancing using lock-free global atomic counter distribution.
* **Least-Loaded:** Real-time queue-depth sniffing (`rte_ring_count`) to route incoming tasks to the least congested worker ring.
* **Flow-Based Affinity:** Binds virtual user profile hashes/flow IDs to explicit cores, ensuring clean L1/L2 instruction and data cache locality.

---

## Directory Layout

```text
task-scheduler/
├── CMakeLists.txt          # Root orchestration mapping dependencies
├── include/
│   ├── scheduler.h         # Flat public interface definitions (No vtable overhead)
│   └── profiler.h          # Zero-overhead inline telemetry engine
├── src/
│   ├── scheduler.c         # Monolithic execution engine and worker loop matrices
│   └── profiler.c          # Out-of-path post-execution reporting system
├── tests/
│   ├── CMakeLists.txt      # Unit tests build configuration
│   └── unit_tests.c        # Self-contained edge case validation suite
└── benchmarks/
    ├── CMakeLists.txt      # Performance evaluation harness build setup
    └── perf_harness.c      # Multi-scenario traffic injection engine (Zero-I/O)



Complete Deployment & Execution Guide
Follow these sequential steps in your native Linux terminal to prepare the ecosystem, compile the workspace, and run the evaluation matrix.

Target Environment Prerequisites:

OS: Ubuntu 22.04 LTS (Native Dual-Boot recommended for precise hardware metrics)

Compiler: GCC 11+ or Clang 14+ supporting C11 standard atomics

Build Tool: CMake 3.10+

Step 1: Install System Dependencies
Update package definitions and install core compiler chains alongside the native DPDK development packages:

Bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libdpdk-dev libnuma-dev


Step 2: Configure Linux Hugepages
Allocate continuous physical memory channels to bypass kernel virtual memory page table bottlenecks:

Bash
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
grep Huge /proc/meminfo
Note: To make this configuration permanent across system reboots, add vm.nr_hugepages = 512 to your /etc/sysctl.conf file.



Step 3: Workspace Directory Setup
Create the isolated development tree block layout and step into the project directory:

Bash
mkdir -p task-scheduler/include task-scheduler/src task-scheduler/tests task-scheduler/benchmarks
cd task-scheduler
Populate the directories with your corresponding source components before executing the next compilation stage.



Step 4: Building the Workspace
Generate the build system configuration and compile the binaries under aggressive Release configurations with the inline profiling feature switched on:

Bash
cmake -B build -S . -DENABLE_PROFILING_FEATURE=ON
cmake --build build --config Release



Step 5: Running Functional Correctness (Unit Tests)
Execute the non-blocking validation harness to verify bounding checks, edge constraints, and setup invariants:

Bash
./build/tests/unit_tests



Step 6: Running Architectural Benchmarks (Performance Arena)
Execute the multi-scenario traffic simulation harness. Root privileges are strictly required to authorize DPDK hardware core affinity pinning and hugepage mappings:

Bash
sudo ./build/benchmarks/perf_harness
Example Performance Telemetry Report
Once execution cycles gracefully spin down and the worker threads are joined, the post-execution analyzer safe-window prints a micro-architectural breakdown map translating raw TSC ticks to localized fractional nanoseconds:

====================================================================================
                       AUTOMATED PROFILER ARCHITECTURAL REPORT                     
====================================================================================
Core Index | Total Packets   | Avg Latency(ns)    | Min Latency(ns) | Max Latency(ns)
------------------------------------------------------------------------------------
Worker 0   | 25000           | 42.15              | 12.00           | 184.50
Worker 1   | 25000           | 44.60              | 11.50           | 192.10
Worker 2   | 25000           | 41.90              | 12.10           | 175.25
Worker 3   | 25000           | 43.02              | 11.80           | 189.00
====================================================================================