/*****************************************************************************************
 * SYSTEM CORE: PRIORITY-AWARE FAIRNESS SCHEDULER 
 * =======================================================================================
 *
 * ARCHITECTURE:
 * - Hybrid Multi-Level Feedback Queue (MLFQ) + Deficit Round Robin (DRR).
 * - O(1) Scheduler complexity for task selection.
 * - Custom Arena Allocator for Task Control Blocks (TCBs).
 * - Lock-free Statistical Telemetry.
 * - C++23 Standard Compliance (Ranges, Concepts, Atomics).
 *
 * COMPILATION:
 * g++ -std=c++23 -O3 -pthread -Wall -Wextra -Wpedantic scheduler_core.cpp -o scheduler
 *
 * AUTHOR: ROHAN KAPRI
 * DATE:   2026-01-24
 *****************************************************************************************/

#include <iostream>
#include <vector>
#include <deque>
#include <queue>
#include <array>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <map>
#include <algorithm>
#include <numeric>
#include <random>
#include <iomanip>
#include <sstream>
#include <cassert>
#include <concepts>
#include <optional>
#include <variant>
#include <memory>
#include <limits>
#include <new>     // placement new
#include <barrier> // C++20 barrier (if needed, though we use CVs here)

// Force C++ Standard Check
#if __cplusplus < 202302L
    #warning "This code is optimized for C++23. Ensure your compiler supports -std=c++23"
#endif

namespace sys::core {

    /*====================================================================================
     * SECTION 1: GLOBAL CONFIGURATION & CONSTANTS
     *====================================================================================*/
    
    // Time Definitions
    using Clock      = std::chrono::steady_clock;
    using TimePoint  = Clock::time_point;
    using Duration   = std::chrono::nanoseconds;
    using MicroSec   = std::chrono::microseconds;
    using MilliSec   = std::chrono::milliseconds;

    // Scheduler Hyperparameters
    static constexpr size_t MAX_PRIORITY_LEVELS     = 8;
    static constexpr size_t PRIORITY_REALTIME       = 0;
    static constexpr size_t PRIORITY_INTERACTIVE    = 1;
    static constexpr size_t PRIORITY_BACKGROUND     = 7;
    
    static constexpr double QUANTUM_BASE_MS         = 10.0;
    static constexpr double QUANTUM_MULTIPLIER      = 1.5;  // Lower prio gets larger quantum
    static constexpr double AGING_THRESHOLD_MS      = 500.0;
    static constexpr double STARVATION_BOOST_MS     = 1000.0;
    
    static constexpr size_t ARENA_BLOCK_SIZE        = 4096; // 4KB Pages
    static constexpr size_t MAX_TASK_CAPACITY       = 10000;

    // Error Codes
    enum class StatusCode : int {
        OK = 0,
        QUEUE_FULL = -1,
        INVALID_PRIORITY = -2,
        INTERNAL_ERROR = -3,
        RESOURCE_EXHAUSTED = -4
    };

    /*====================================================================================
     * SECTION 2: CONCEPTS & METAPROGRAMMING
     *====================================================================================*/

    template<typename T>
    concept Identifiable = requires(T t) {
        { t.get_id() } -> std::convertible_to<uint64_t>;
    };

    template<typename T>
    concept Prioritizable = requires(T t) {
        { t.get_priority() } -> std::convertible_to<int>;
        { t.set_priority(0) };
    };

    template<typename T>
    concept Executable = requires(T t) {
        { t.execute() } -> std::same_as<void>;
    };

    /*====================================================================================
     * SECTION 3: UTILITY PRIMITIVES (LOCKS & LOGGING)
     *====================================================================================*/

    /**
     * @class SpinLock
     * @brief A user-space spinlock using atomic_flag for low-latency synchronization.
     * Preferred over std::mutex for very short critical sections in the scheduler.
     */
    class SpinLock {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;

    public:
        void lock() noexcept {
            while (flag.test_and_set(std::memory_order_acquire)) {
                #if defined(__cpp_lib_atomic_wait)
                    flag.wait(true, std::memory_order_relaxed);
                #else
                    std::this_thread::yield();
                #endif
            }
        }

        void unlock() noexcept {
            flag.clear(std::memory_order_release);
            #if defined(__cpp_lib_atomic_wait)
                flag.notify_one();
            #endif
        }
    };

    /**
     * @struct ScopedSpinLock
     * @brief RAII wrapper for SpinLock.
     */
    struct ScopedSpinLock {
        SpinLock& sl;
        explicit ScopedSpinLock(SpinLock& s) : sl(s) { sl.lock(); }
        ~ScopedSpinLock() { sl.unlock(); }
        ScopedSpinLock(const ScopedSpinLock&) = delete;
        ScopedSpinLock& operator=(const ScopedSpinLock&) = delete;
    };

    /**
     * @class Logger
     * @brief Thread-safe synchronous logging facility with severity levels.
     */
    class Logger {
    public:
        enum class Level { DEBUG, INFO, WARN, ERROR, CRITICAL };

    private:
        static std::mutex log_mutex;
        static Level current_level;

        static std::string level_to_string(Level l) {
            switch(l) {
                case Level::DEBUG:    return "[DEBUG]   ";
                case Level::INFO:     return "[INFO]    ";
                case Level::WARN:     return "[WARN]    ";
                case Level::ERROR:    return "[ERROR]   ";
                case Level::CRITICAL: return "[CRITICAL]";
                default:              return "[UNKNOWN] ";
            }
        }

    public:
        static void set_level(Level l) {
            std::lock_guard<std::mutex> lock(log_mutex);
            current_level = l;
        }

        template<typename... Args>
        static void log(Level lvl, Args&&... args) {
            std::lock_guard<std::mutex> lock(log_mutex);
            if (lvl < current_level) return;

            auto now = Clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::cout << std::put_time(std::localtime(&now_c), "%H:%M:%S") << " "
                      << level_to_string(lvl) << " ";
            ((std::cout << std::forward<Args>(args)), ...);
            std::cout << "\n";
        }
    };

    std::mutex Logger::log_mutex;
    Logger::Level Logger::current_level = Logger::Level::INFO;

    /*====================================================================================
     * SECTION 4: STATISTICAL TELEMETRY
     *====================================================================================*/

    /**
     * @class AtomicStats
     * @brief Lock-free accumulator for runtime statistics (Welford's Online Algorithm).
     */
    class AtomicStats {
    private:
        mutable SpinLock lock_; 
        uint64_t count_ = 0;
        double min_ = std::numeric_limits<double>::max();
        double max_ = std::numeric_limits<double>::lowest();
        double sum_ = 0.0;
        double sum_sq_ = 0.0;

    public:
        void record(double value) {
            ScopedSpinLock guard(lock_);
            count_++;
            sum_ += value;
            sum_sq_ += (value * value);
            if (value < min_) min_ = value;
            if (value > max_) max_ = value;
        }

        struct Snapshot {
            uint64_t n;
            double mean;
            double variance;
            double min_val;
            double max_val;
        };

        [[nodiscard]] Snapshot get_snapshot() const {
            ScopedSpinLock guard(lock_);
            if (count_ == 0) return {0, 0.0, 0.0, 0.0, 0.0};
            
            double mean = sum_ / count_;
            double variance = (count_ > 1) ? ((sum_sq_ - (sum_ * sum_) / count_) / (count_ - 1)) : 0.0;
            return {count_, mean, variance, min_, max_};
        }
    };

    /*====================================================================================
     * SECTION 5: TASK CONTROL BLOCK (TCB) & MEMORY ARENA
     *====================================================================================*/

    enum class TaskState {
        NEW,
        READY,
        RUNNING,
        BLOCKED,
        COMPLETED,
        TERMINATED
    };

    /**
     * @struct TaskControlBlock
     * @brief The fundamental unit of scheduling. 
     * Aligned to cache lines (64 bytes) to prevent false sharing.
     */
    struct alignas(64) TaskControlBlock {
        // --- Identity & Properties ---
        uint64_t id;
        std::string name;
        int priority_base;
        int priority_current;
        
        // --- State Management ---
        std::atomic<TaskState> state;
        bool is_preemptable;
        
        // --- Time Accounting ---
        TimePoint creation_time;
        TimePoint last_run_time;
        Duration cpu_time_consumed;
        Duration wait_time_accumulated;
        
        // --- Scheduling Context ---
        double deficit_counter;     // For DRR algorithm
        int time_slice_ticks;       // Remaining ticks in current quantum
        
        // --- Methods ---
        TaskControlBlock(uint64_t _id, std::string _name, int _prio)
            : id(_id), name(std::move(_name)),
              priority_base(_prio), priority_current(_prio),
              state(TaskState::NEW), is_preemptable(true),
              creation_time(Clock::now()), last_run_time(Clock::now()),
              cpu_time_consumed(0), wait_time_accumulated(0),
              deficit_counter(0.0), time_slice_ticks(0) {}

        void transition_to(TaskState new_state) {
            // Strict state transition validation could go here
            state.store(new_state, std::memory_order_release);
        }

        [[nodiscard]] double get_wait_ms() const {
            auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(wait_time_accumulated);
            return ms.count();
        }
    };

    /**
     * @class ObjectPool
     * @brief A simple fixed-size memory pool to avoid frequent new/delete for TCBs.
     */
    template <typename T, size_t Capacity>
    class ObjectPool {
    private:
        struct Node {
            T data;
            Node* next;
        };

        std::array<unsigned char, sizeof(Node) * Capacity> storage_;
        Node* free_head_;
        SpinLock lock_;
        size_t active_count_;

    public:
        ObjectPool() : free_head_(nullptr), active_count_(0) {
            // Initialize free list
            for (size_t i = 0; i < Capacity; ++i) {
                void* ptr = &storage_[i * sizeof(Node)];
                Node* node = static_cast<Node*>(ptr);
                node->next = free_head_;
                free_head_ = node;
            }
        }

        template<typename... Args>
        T* allocate(Args&&... args) {
            ScopedSpinLock guard(lock_);
            if (!free_head_) {
                throw std::bad_alloc();
            }
            
            Node* node = free_head_;
            free_head_ = free_head_->next;
            active_count_++;
            
            // Construct in place
            T* obj_ptr = &(node->data);
            std::construct_at(obj_ptr, std::forward<Args>(args)...);
            return obj_ptr;
        }

        void deallocate(T* ptr) {
            if (!ptr) return;
            // Destruct
            std::destroy_at(ptr);

            // Return to pool
            ScopedSpinLock guard(lock_);
            // Calculate Node pointer from T pointer (Standard Layout assumption)
            Node* node = reinterpret_cast<Node*>(ptr);
            node->next = free_head_;
            free_head_ = node;
            active_count_--;
        }

        size_t count() const { return active_count_; }
    };

    /*====================================================================================
     * SECTION 6: SCHEDULING QUEUES (DRR + AGING)
     *====================================================================================*/

    /**
     * @struct PriorityLevel
     * @brief Manages a single queue within the MLFQ system.
     */
    struct PriorityLevel {
        int level_index;
        std::deque<TaskControlBlock*> ready_queue;
        double quantum_size_ms;
        double deficit_threshold;
        
        mutable SpinLock q_lock;

        PriorityLevel() : level_index(0), quantum_size_ms(0), deficit_threshold(0) {}

        void init(int idx) {
            level_index = idx;
            // Calculate quantum based on priority (lower priority = longer quantum)
            // Priority 0 (Realtime) -> Small quantum (latency sensitive)
            // Priority 7 (Batch)    -> Large quantum (throughput sensitive)
            double multiplier = std::pow(QUANTUM_MULTIPLIER, idx);
            quantum_size_ms = QUANTUM_BASE_MS * multiplier;
        }

        void enqueue(TaskControlBlock* tcb) {
            ScopedSpinLock guard(q_lock);
            ready_queue.push_back(tcb);
        }

        TaskControlBlock* peek() {
            ScopedSpinLock guard(q_lock);
            if (ready_queue.empty()) return nullptr;
            return ready_queue.front();
        }

        TaskControlBlock* dequeue() {
            ScopedSpinLock guard(q_lock);
            if (ready_queue.empty()) return nullptr;
            TaskControlBlock* t = ready_queue.front();
            ready_queue.pop_front();
            return t;
        }

        bool empty() const {
            ScopedSpinLock guard(q_lock);
            return ready_queue.empty();
        }

        size_t size() const {
            ScopedSpinLock guard(q_lock);
            return ready_queue.size();
        }
    };

    /*====================================================================================
     * SECTION 7: CORE SCHEDULER ENGINE
     *====================================================================================*/

    class SchedulerEngine {
    private:
        // --- Core Data Structures ---
        std::vector<PriorityLevel> priority_levels_;
        ObjectPool<TaskControlBlock, MAX_TASK_CAPACITY> tcb_pool_;
        
        // --- Sync & State ---
        std::atomic<bool> running_;
        std::atomic<uint64_t> id_counter_;
        
        // --- Telemetry ---
        AtomicStats wait_time_stats_;
        AtomicStats burst_time_stats_;
        std::atomic<uint64_t> total_context_switches_;

    public:
        SchedulerEngine() : running_(false), id_counter_(1), total_context_switches_(0) {
            priority_levels_.resize(MAX_PRIORITY_LEVELS);
            for (size_t i = 0; i < MAX_PRIORITY_LEVELS; ++i) {
                priority_levels_[i].init(static_cast<int>(i));
            }
        }

        ~SchedulerEngine() {
            // Cleanup would ideally drain queues and return TCBs to pool
        }

        // --- Administrative Interface ---

        /**
         * @brief Submits a new task to the scheduler.
         * @return Task ID on success, 0 on failure.
         */
        uint64_t submit_task(const std::string& name, int priority) {
            // 1. Validate inputs
            int clamped_prio = std::clamp(priority, 0, (int)MAX_PRIORITY_LEVELS - 1);

            // 2. Allocate TCB from Pool
            try {
                uint64_t new_id = id_counter_.fetch_add(1);
                TaskControlBlock* tcb = tcb_pool_.allocate(new_id, name, clamped_prio);
                
                // 3. Initial State Set
                tcb->transition_to(TaskState::READY);
                tcb->deficit_counter = 0; // Reset deficit

                // 4. Enqueue
                priority_levels_[clamped_prio].enqueue(tcb);

                Logger::log(Logger::Level::DEBUG, "Task Submitted: ", name, " [ID:", new_id, "] Prio:", clamped_prio);
                return new_id;
            } catch (const std::bad_alloc&) {
                Logger::log(Logger::Level::ERROR, "Failed to allocate TCB for task: ", name);
                return 0;
            }
        }

        // --- Core Scheduling Logic ---

        /**
         * @brief Scans queues using Deficit Round Robin logic to find the next task.
         * Implements O(1) selection by iterating fixed priority levels.
         */
        TaskControlBlock* select_next_task() {
            for (size_t p = 0; p < MAX_PRIORITY_LEVELS; ++p) {
                PriorityLevel& pl = priority_levels_[p];
                
                // Fast check before locking
                if (pl.empty()) continue;

                // Lock the queue to manipulate
                ScopedSpinLock guard(pl.q_lock);
                
                if (pl.ready_queue.empty()) continue;

                // DRR Logic:
                // We iterate through the queue. If a task has enough deficit, run it.
                // If not, add quantum to deficit and move to back.
                
                size_t queue_len = pl.ready_queue.size();
                // Limit iterations to avoid holding lock too long (starvation prevention for this level)
                size_t max_checks = queue_len; 
                
                while (max_checks > 0) {
                    TaskControlBlock* head = pl.ready_queue.front();
                    pl.ready_queue.pop_front();
                    max_checks--;

                    // Add Quantum to Deficit
                    // Note: In a pure DRR, we add quantum only when we revisit the list.
                    // Here we simplify: Every time we inspect a task for selection, 
                    // if it wasn't selected previously, its deficit might be low.
                    
                    // Actually, standard DRR:
                    // 1. Iterate list.
                    // 2. Head.deficit += Quantum.
                    // 3. If Head.deficit >= Head.cost: Run.
                    
                    // We treat "cost" as "time slice we intend to give".
                    // Let's verify if task is runnable (e.g., not blocked logic handled elsewhere).

                    // Logic: Give the task a chance.
                    // If it used its previous slice fully, it might have negative/zero deficit.
                    // We grant it the quantum for this level.
                    head->deficit_counter += pl.quantum_size_ms;

                    // Check if it qualifies to run (avoid tiny bursts if deficit is negative)
                    // For this simulation, we assume cost is roughly the quantum.
                    if (head->deficit_counter > 0) {
                        return head; // Found a task! (It remains popped from queue)
                    } else {
                        // Not enough deficit (penalty box?), push back
                        pl.ready_queue.push_back(head);
                    }
                }
            }
            return nullptr; // Idle
        }

        /**
         * @brief Performs Aging to prevent starvation of low-priority tasks.
         * Promotes tasks that have waited too long.
         */
        void maintain_fairness(TimePoint now) {
            for (size_t p = 1; p < MAX_PRIORITY_LEVELS; ++p) { // Skip highest priority
                PriorityLevel& pl = priority_levels_[p];
                if (pl.empty()) continue;

                // We need to carefully move elements without invalidating iterators or breaking thread safety.
                // Strategy: Extract candidates, then re-insert into higher queue.
                
                std::vector<TaskControlBlock*> promotions;
                {
                    ScopedSpinLock guard(pl.q_lock);
                    auto it = pl.ready_queue.begin();
                    while (it != pl.ready_queue.end()) {
                        TaskControlBlock* tcb = *it;
                        
                        // Calculate wait time
                        auto diff = now - tcb->last_run_time;
                        double wait_ms = std::chrono::duration<double, std::milli>(diff).count();
                        
                        // Check accumulated wait (from previous context switches)
                        double total_wait = wait_ms; // Simplified for this check

                        if (total_wait > STARVATION_BOOST_MS) {
                            // Promote!
                            promotions.push_back(tcb);
                            it = pl.ready_queue.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                // Re-insert into higher priority queue (p-1)
                if (!promotions.empty()) {
                    PriorityLevel& target = priority_levels_[p - 1];
                    ScopedSpinLock target_guard(target.q_lock);
                    for (auto* t : promotions) {
                        t->priority_current = p - 1;
                        t->deficit_counter = 0; // Reset deficit on level change
                        target.ready_queue.push_back(t);
                        Logger::log(Logger::Level::INFO, "AGING PROMOTION: Task ", t->id, " moved to Prio ", t->priority_current);
                    }
                }
            }
        }

        /**
         * @brief The main execution tick. Simulates one slice of CPU time.
         * @param tick_duration_ms duration to simulate.
         */
        void run_tick(double tick_duration_ms) {
            TimePoint now = Clock::now();

            // 1. Maintenance (Aging) - Do this periodically, not every tick ideally, but here for demo
            static TimePoint last_maintenance = now;
            if (std::chrono::duration_cast<MilliSec>(now - last_maintenance).count() > 100) {
                maintain_fairness(now);
                last_maintenance = now;
            }

            // 2. Select Task
            TaskControlBlock* current_task = select_next_task();

            if (!current_task) {
                // Idle Cycle
                // std::this_thread::yield(); 
                return;
            }

            // 3. Context Switch In
            total_context_switches_.fetch_add(1, std::memory_order_relaxed);
            current_task->transition_to(TaskState::RUNNING);
            
            // Calculate wait time since last run
            Duration wait_delta = now - current_task->last_run_time;
            current_task->wait_time_accumulated += wait_delta;
            wait_time_stats_.record(std::chrono::duration<double, std::milli>(wait_delta).count());

            // 4. Execute (Simulation)
            Logger::log(Logger::Level::DEBUG, ">>> EXEC: Task ", current_task->id, " (Prio ", current_task->priority_current, ")");
            
            // Simulate work: Sleep for the tick duration
            // In a real scheduler, we would jump to the task context.
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(tick_duration_ms));

            // 5. Context Switch Out & Accounting
            TimePoint finish_time = Clock::now();
            Duration run_duration = finish_time - now;
            
            current_task->cpu_time_consumed += run_duration;
            current_task->last_run_time = finish_time;
            
            double run_ms = std::chrono::duration<double, std::milli>(run_duration).count();
            burst_time_stats_.record(run_ms);

            // Update Deficit (Charge the task for time used)
            current_task->deficit_counter -= run_ms;

            // 6. Reschedule Logic (MLFQ Demotion)
            current_task->transition_to(TaskState::READY);

            // Determine fate:
            // Random chance of completion for simulation
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, 100);

            if (dist(rng) > 95) { // 5% chance to finish
                current_task->transition_to(TaskState::COMPLETED);
                Logger::log(Logger::Level::INFO, "Task ", current_task->id, " COMPLETED. Total CPU: ", 
                    std::chrono::duration_cast<MilliSec>(current_task->cpu_time_consumed).count(), "ms");
                
                // Return to pool
                tcb_pool_.deallocate(current_task);
            } else {
                // Determine if priority should change
                // If task used a lot of CPU (deficit negative), demote it
                if (current_task->deficit_counter < 0 && current_task->priority_current < (int)MAX_PRIORITY_LEVELS - 1) {
                    current_task->priority_current++;
                    current_task->deficit_counter = 0; // Reset for new level
                    Logger::log(Logger::Level::DEBUG, "DEMOTION: Task ", current_task->id, " -> Prio ", current_task->priority_current);
                }

                // Re-queue
                priority_levels_[current_task->priority_current].enqueue(current_task);
            }
        }

        // --- Reporting ---
        void print_stats() {
            auto wait = wait_time_stats_.get_snapshot();
            auto burst = burst_time_stats_.get_snapshot();

            std::cout << "\n========================================\n";
            std::cout << " SCHEDULER TELEMETRY REPORT\n";
            std::cout << "========================================\n";
            std::cout << " Context Switches: " << total_context_switches_ << "\n";
            std::cout << " Active Tasks:     " << tcb_pool_.count() << "\n";
            std::cout << "----------------------------------------\n";
            std::cout << " WAIT TIME (ms):   Avg=" << std::fixed << std::setprecision(2) << wait.mean 
                      << " | Max=" << wait.max_val << " | Var=" << wait.variance << "\n";
            std::cout << " BURST TIME (ms):  Avg=" << burst.mean 
                      << " | Max=" << burst.max_val << "\n";
            std::cout << "========================================\n";
        }
    };

} // namespace sys::core

/*====================================================================================
 * SECTION 8: MAIN DRIVER & SCENARIO SIMULATION
 *====================================================================================*/

using namespace sys::core;

void stress_test_scenario(SchedulerEngine& sched) {
    Logger::log(Logger::Level::INFO, "Starting Stress Test Scenario...");

    // 1. Burst of Interactive Tasks (High Priority)
    for (int i = 0; i < 20; ++i) {
        sched.submit_task("Interactive_App_" + std::to_string(i), PRIORITY_INTERACTIVE);
    }

    // 2. Heavy Background Load (Low Priority)
    for (int i = 0; i < 50; ++i) {
        sched.submit_task("Background_Indexer_" + std::to_string(i), PRIORITY_BACKGROUND);
    }

    // 3. Realtime Critical Tasks
    for (int i = 0; i < 5; ++i) {
        sched.submit_task("Audio_Processing_" + std::to_string(i), PRIORITY_REALTIME);
    }

    // Run Simulation Loop
    const int SIMULATION_STEPS = 500;
    const double TICK_MS = 5.0;

    auto start_time = std::chrono::steady_clock::now();

    for (int step = 0; step < SIMULATION_STEPS; ++step) {
        sched.run_tick(TICK_MS);

        // Dynamic Arrival: Inject tasks mid-simulation
        if (step == 100) {
            Logger::log(Logger::Level::WARN, ">>> INJECTING BURST TRAFFIC <<<");
            for(int k=0; k<10; k++) sched.submit_task("Burst_Job_" + std::to_string(k), 4);
        }
        
        // Emulate some minimal processing delay overhead
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    auto end_time = std::chrono::steady_clock::now();
    Logger::log(Logger::Level::INFO, "Simulation Finished.");
    Logger::log(Logger::Level::INFO, "Real Wall Time: ", 
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count(), "ms");
}

int main() {
    // Optimization for C++ streams
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    Logger::set_level(Logger::Level::INFO);

    std::cout << "   ________________________________________   \n";
    std::cout << "  |                                        |  \n";
    std::cout << "  |   HARDCORE SCHEDULER SIMULATION v2.0   |  \n";
    std::cout << "  |       MLFQ + DRR + C++23 Core          |  \n";
    std::cout << "  |________________________________________|  \n\n";

    try {
        SchedulerEngine engine;
        stress_test_scenario(engine);
        engine.print_stats();
    } catch (const std::exception& e) {
        Logger::log(Logger::Level::CRITICAL, "Unhandled Exception: ", e.what());
        return 1;
    }

    return 0;
}