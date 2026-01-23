// ============================================================================
// C++23 STRICT IMPLEMENTATION — EXTENDED VERSION
// Admission-Controlled Backpressure Propagation System
// Deadline-Preserving Distributed Pipeline Simulator (Extended & Instrumented)
//
// Language Standard: ISO C++23 ONLY
// Compiler Target : clang++ / g++ with -std=c++23 -Wall -Wextra -pedantic
//
// Purpose:
//   • Much more detailed internal state tracking
//   • Multiple workers (thread pool style)
//   • Multiple admission stages / pipeline stages
//   • Per-request latency histogram
//   • Queue length & admission rate history (ring buffer)
//   • EDF scheduling with tie-breaking by arrival time
//   • Dynamic estimation of service time (EWMA)
//   • Rejection reason classification
//   • Structured logging with timestamps & severity
//   • Configuration object
//   • Statistics collector with percentile reporting
//   • Graceful shutdown & final detailed report
//
// This version is intentionally verbose, heavily commented,
// instrumented and split into many small well-named pieces.
//
//
// ============================================================================

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <queue>
#include <random>
#include <ranges>
#include <ratio>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//                           Compile-time Configuration
// ─────────────────────────────────────────────────────────────────────────────

#ifndef SIMULATION_DURATION_SECONDS
#define SIMULATION_DURATION_SECONDS 12
#endif

#ifndef QUEUE_HARD_CAPACITY
#define QUEUE_HARD_CAPACITY 96
#endif

#ifndef QUEUE_SOFT_THRESHOLD_PCT
#define QUEUE_SOFT_THRESHOLD_PCT 75
#endif

#ifndef NUMBER_OF_WORKERS
#define NUMBER_OF_WORKERS 5
#endif

#ifndef EWMA_ALPHA_PERCENT
#define EWMA_ALPHA_PERCENT 18      // ~0.18 smoothing factor
#endif

#ifndef HISTORY_RING_BUFFER_SIZE
#define HISTORY_RING_BUFFER_SIZE 512
#endif

#ifndef HISTOGRAM_BUCKET_COUNT
#define HISTOGRAM_BUCKET_COUNT 32
#endif

#ifndef REJECTION_CLASS_COUNT
#define REJECTION_CLASS_COUNT 5
#endif

// ─────────────────────────────────────────────────────────────────────────────
//                          Duration / Time utilities
// ─────────────────────────────────────────────────────────────────────────────

using namespace std::chrono_literals;

using microseconds_t   = std::chrono::microseconds;
using milliseconds_t   = std::chrono::milliseconds;
using steady_clock     = std::chrono::steady_clock;
using time_point       = steady_clock::time_point;
using duration_t       = steady_clock::duration;

inline microseconds_t to_us(duration_t d) {
    return std::chrono::duration_cast<microseconds_t>(d);
}

inline milliseconds_t to_ms(duration_t d) {
    return std::chrono::duration_cast<milliseconds_t>(d);
}

inline int64_t to_us_int(duration_t d) {
    return std::chrono::duration_cast<microseconds_t>(d).count();
}

inline std::string format_duration(microseconds_t d) {
    auto ms = std::chrono::duration_cast<milliseconds_t>(d).count();
    if (ms < 1000) {
        return std::format("{} µs", d.count());
    }
    auto s = ms / 1000;
    ms %= 1000;
    if (s < 60) {
        return std::format("{}.{:03d} s", s, ms);
    }
    auto m = s / 60;
    s %= 60;
    return std::format("{}m {:02d}.{:03d}s", m, s, ms);
}

// ─────────────────────────────────────────────────────────────────────────────
//                             Global monotonic clock
// ─────────────────────────────────────────────────────────────────────────────

class MonotonicClock {
public:
    static time_point now() noexcept {
        return steady_clock::now();
    }

    static int64_t now_us() noexcept {
        return to_us_int(now().time_since_epoch());
    }

    static std::string now_iso() {
        auto t = now();
        auto s = std::chrono::time_point_cast<std::chrono::seconds>(t);
        auto ms = std::chrono::duration_cast<milliseconds_t>(t - s).count();
        return std::format("{:%Y-%m-%d %H:%M:%S}.{:03d}", s, ms);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                               Log Severity
// ─────────────────────────────────────────────────────────────────────────────

enum class LogLevel : uint8_t {
    TRACE   = 0,
    DEBUG   = 1,
    INFO    = 2,
    NOTICE  = 3,
    WARNING = 4,
    ERROR   = 5,
    FATAL   = 6
};

static constexpr std::string_view log_level_names[] = {
    "TRACE", "DEBUG", "INFO ", "NOTE ", "WARN ", "ERROR", "FATAL"
};

// ─────────────────────────────────────────────────────────────────────────────
//                           Thread-safe Logger
// ─────────────────────────────────────────────────────────────────────────────

class Logger final {
private:
    std::mutex               mtx_;
    std::atomic<LogLevel>    min_level_ {LogLevel::INFO};
    bool                     timestamps_ {true};
    bool                     thread_id_   {false};

public:
    void set_level(LogLevel lvl) noexcept {
        min_level_.store(lvl, std::memory_order_relaxed);
    }

    void enable_timestamps(bool en) noexcept { timestamps_ = en; }
    void enable_thread_id(bool en) noexcept   { thread_id_ = en; }

    template<typename... Args>
    void log(LogLevel lvl, std::format_string<Args...> fmt, Args&&... args) {
        if (lvl < min_level_.load(std::memory_order_relaxed)) {
            return;
        }

        std::string msg = std::vformat(fmt.get(), std::make_format_args(args...));

        std::lock_guard<std::mutex> lk(mtx_);

        if (timestamps_) {
            std::cout << MonotonicClock::now_iso() << "  ";
        }
        if (thread_id_) {
            std::cout << "[" << std::this_thread::get_id() << "] ";
        }

        std::cout << log_level_names[static_cast<size_t>(lvl)]
                  << "  " << msg << '\n';
        std::cout.flush();
    }

#define LOG(lvl, ...) \
    do { GLOBAL_LOGGER.log(LogLevel::lvl, __VA_ARGS__); } while(0)

    static Logger& instance() {
        static Logger inst;
        return inst;
    }
};

inline Logger& GLOBAL_LOGGER = Logger::instance();

// ─────────────────────────────────────────────────────────────────────────────
//                               Request structure
// ─────────────────────────────────────────────────────────────────────────────

struct Request final {
    uint64_t                   id;
    time_point                 arrival_time;
    time_point                 soft_deadline;
    time_point                 hard_deadline;
    microseconds_t             service_cost_us;
    bool                       admitted               = false;
    bool                       started_processing     = false;
    bool                       finished               = false;
    time_point                 start_process_time     {};
    time_point                 completion_time        {};
    microseconds_t             actual_latency_us       {};
    microseconds_t             queue_wait_time_us      {};
    int                        rejection_class         = -1;   // -1 = accepted

    Request(uint64_t id_,
            time_point arrival,
            microseconds_t relative_deadline_us,
            microseconds_t cost_us,
            double hard_deadline_multiplier = 1.4)
        : id(id_)
        , arrival_time(arrival)
        , soft_deadline(arrival + relative_deadline_us)
        , hard_deadline(arrival + microseconds_t(
              static_cast<int64_t>(relative_deadline_us.count() * hard_deadline_multiplier)))
        , service_cost_us(cost_us)
    {}

    [[nodiscard]] bool is_expired(time_point tp = MonotonicClock::now()) const noexcept {
        return tp > soft_deadline;
    }

    [[nodiscard]] bool missed_hard_deadline(time_point tp = MonotonicClock::now()) const noexcept {
        return finished && tp > hard_deadline;
    }

    [[nodiscard]] std::string description_short() const {
        return std::format("R{} d={}", id,
                           format_duration(to_us(soft_deadline - arrival_time)));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                   EDF + arrival-time tie-breaker comparator
// ─────────────────────────────────────────────────────────────────────────────

struct EarliestDeadlineFirst {
    bool operator()(const std::shared_ptr<Request>& a,
                    const std::shared_ptr<Request>& b) const noexcept {
        if (a->soft_deadline != b->soft_deadline) {
            return a->soft_deadline > b->soft_deadline; // min-heap → earliest first
        }
        return a->arrival_time > b->arrival_time;       // tie-break: earlier arrival first
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                      Bounded priority queue (thread-safe)
// ─────────────────────────────────────────────────────────────────────────────

class DeadlineBoundedPriorityQueue final {
private:
    using value_type = std::shared_ptr<Request>;
    using container  = std::vector<value_type>;
    using pq_type    = std::priority_queue<value_type, container, EarliestDeadlineFirst>;

    mutable std::mutex mtx_;
    pq_type            pq_;
    const size_t       capacity_;

public:
    explicit DeadlineBoundedPriorityQueue(size_t cap)
        : capacity_(cap) {}

    [[nodiscard]] bool   empty()  const { std::lock_guard lk(mtx_); return pq_.empty();  }
    [[nodiscard]] size_t size()   const { std::lock_guard lk(mtx_); return pq_.size();   }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool can_push() const {
        std::lock_guard lk(mtx_);
        return pq_.size() < capacity_;
    }

    bool try_push(value_type req) {
        std::lock_guard lk(mtx_);
        if (pq_.size() >= capacity_) {
            return false;
        }
        pq_.push(std::move(req));
        return true;
    }

    std::optional<value_type> try_pop() {
        std::lock_guard lk(mtx_);
        if (pq_.empty()) {
            return std::nullopt;
        }
        auto top = std::move(pq_.top());
        pq_.pop();
        return top;
    }

    void clear_expired(time_point now) {
        std::lock_guard lk(mtx_);
        container temp;
        temp.reserve(pq_.size());

        while (!pq_.empty()) {
            auto req = std::move(pq_.top());
            pq_.pop();
            if (!req->is_expired(now)) {
                temp.push_back(std::move(req));
            }
        }

        for (auto& r : temp | std::views::reverse) {
            pq_.push(std::move(r));
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                        Exponential Weighted Moving Average
// ─────────────────────────────────────────────────────────────────────────────

class EWMA final {
private:
    double alpha_;
    double value_     = 0.0;
    bool   initialized_ = false;

public:
    explicit EWMA(double alpha_percent = EWMA_ALPHA_PERCENT / 100.0)
        : alpha_(std::clamp(alpha_percent, 0.001, 0.999)) {}

    void update(double sample) noexcept {
        if (!initialized_) {
            value_ = sample;
            initialized_ = true;
            return;
        }
        value_ = alpha_ * sample + (1.0 - alpha_) * value_;
    }

    [[nodiscard]] double get() const noexcept { return value_; }
    [[nodiscard]] bool   has_value() const noexcept { return initialized_; }

    void reset() noexcept {
        value_ = 0.0;
        initialized_ = false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                         Rejection classification enum
// ─────────────────────────────────────────────────────────────────────────────

enum class RejectionReason : uint8_t {
    QUEUE_FULL_HARD          = 0,
    ESTIMATED_LATENCY_EXCEEDS_DEADLINE = 1,
    ALREADY_EXPIRED_AT_ARRIVAL   = 2,
    DOWNSTREAM_BACKPRESSURE      = 3,
    OTHER                        = 4
};

static constexpr std::string_view rejection_reason_names[REJECTION_CLASS_COUNT] = {
    "queue-full-hard",
    "estimated-time-violation",
    "expired-on-arrival",
    "downstream-backpressure",
    "other"
};

// ─────────────────────────────────────────────────────────────────────────────
//                        Admission decision result
// ─────────────────────────────────────────────────────────────────────────────

struct AdmissionDecision {
    bool accepted = false;
    RejectionReason reason = RejectionReason::OTHER;
    std::string message;
};

// ─────────────────────────────────────────────────────────────────────────────
//                           Admission Controller
// ─────────────────────────────────────────────────────────────────────────────

class AdmissionController final {
private:
    DeadlineBoundedPriorityQueue& queue_;
    const size_t                  hard_limit_;
    const size_t                  soft_threshold_;
    EWMA                          service_time_ewma_;
    std::atomic<uint64_t>         admission_count_{0};
    std::atomic<uint64_t>         rejection_count_{0};

public:
    AdmissionController(DeadlineBoundedPriorityQueue& q,
                        size_t hard_limit,
                        microseconds_t initial_service_guess)
        : queue_(q)
        , hard_limit_(hard_limit)
        , soft_threshold_((hard_limit * QUEUE_SOFT_THRESHOLD_PCT) / 100)
        , service_time_ewma_(EWMA_ALPHA_PERCENT / 100.0)
    {
        service_time_ewma_.update(initial_service_guess.count());
    }

    AdmissionDecision decide(const std::shared_ptr<Request>& req,
                             time_point now) {
        if (req->is_expired(now)) {
            return {false, RejectionReason::ALREADY_EXPIRED_AT_ARRIVAL,
                    "Request already past soft deadline on arrival"};
        }

        size_t current_qlen = queue_.size();

        if (current_qlen >= hard_limit_) {
            return {false, RejectionReason::QUEUE_FULL_HARD,
                    std::format("Queue full ({} >= {})", current_qlen, hard_limit_)};
        }

        double est_service_us = service_time_ewma_.get();
        double est_wait_us    = est_service_us * current_qlen;
        auto   est_completion = now + microseconds_t(static_cast<int64_t>(est_wait_us + req->service_cost_us.count()));

        if (est_completion > req->soft_deadline) {
            return {false, RejectionReason::ESTIMATED_LATENCY_EXCEEDS_DEADLINE,
                    std::format("Estimated completion {} > deadline {}",
                                format_duration(to_us(est_completion - now)),
                                format_duration(to_us(req->soft_deadline - now)))};
        }

        // soft backpressure — still accept but log
        if (current_qlen >= soft_threshold_) {
            return {true, RejectionReason::DOWNSTREAM_BACKPRESSURE,
                    std::format("Soft backpressure (qlen {} >= {}%)", current_qlen, QUEUE_SOFT_THRESHOLD_PCT)};
        }

        return {true, RejectionReason::OTHER, ""};
    }

    void notify_accepted() noexcept { admission_count_.fetch_add(1, std::memory_order_relaxed); }
    void notify_rejected() noexcept { rejection_count_.fetch_add(1, std::memory_order_relaxed); }

    void update_service_time_observation(microseconds_t measured) noexcept {
        service_time_ewma_.update(static_cast<double>(measured.count()));
    }

    [[nodiscard]] uint64_t admitted_count()  const noexcept { return admission_count_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t rejected_count()  const noexcept { return rejection_count_.load(std::memory_order_acquire); }
    [[nodiscard]] double   current_est_us()  const noexcept { return service_time_ewma_.get(); }
};

// ─────────────────────────────────────────────────────────────────────────────
//                        Latency Histogram (simple linear)
// ─────────────────────────────────────────────────────────────────────────────

class LatencyHistogram final {
private:
    static constexpr int64_t BUCKET_UPPER_EDGES[HISTOGRAM_BUCKET_COUNT] = {
          100,   250,   500,   750,  1000,  1500,  2000,  3000,  4000,  5000,
         7500, 10000, 15000, 20000, 30000, 40000, 50000, 75000,100000,150000,
        200000,300000,500000,750000,1000000,1500000,2000000,3000000,5000000,10000000,
        20000000, 50000000
    };

    std::array<std::atomic<uint64_t>, HISTOGRAM_BUCKET_COUNT> counters_{};
    std::atomic<uint64_t> overflow_count_{0};
    std::atomic<uint64_t> total_count_{0};

public:
    void record(int64_t us) noexcept {
        total_count_.fetch_add(1, std::memory_order_relaxed);

        for (size_t i = 0; i < HISTOGRAM_BUCKET_COUNT; ++i) {
            if (us <= BUCKET_UPPER_EDGES[i]) {
                counters_[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void print(std::ostream& os = std::cout) const {
        os << "Latency histogram (µs):\n";
        uint64_t cumulative = 0;
        uint64_t total = total_count_.load(std::memory_order_acquire);

        for (size_t i = 0; i < HISTOGRAM_BUCKET_COUNT; ++i) {
            uint64_t cnt = counters_[i].load(std::memory_order_acquire);
            if (cnt == 0) continue;

            cumulative += cnt;
            double pct = total ? (100.0 * cumulative / total) : 0.0;

            os << std::format("{:>8} µs : {:>10} ({:5.2f}%)\n",
                              BUCKET_UPPER_EDGES[i], cnt, pct);
        }

        uint64_t ovf = overflow_count_.load(std::memory_order_acquire);
        if (ovf > 0) {
            double pct = total ? (100.0 * (cumulative + ovf) / total) : 0.0;
            os << std::format(">50 ms     : {:>10} ({:5.2f}%)\n", ovf, pct);
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                        Ring-buffer history of queue length
// ─────────────────────────────────────────────────────────────────────────────

class QueueLengthHistory final {
private:
    std::array<uint16_t, HISTORY_RING_BUFFER_SIZE> buffer_{};
    std::atomic<size_t> head_{0};
    std::atomic<uint64_t> total_sum_{0};
    std::atomic<uint64_t> count_{0};

public:
    void record(size_t len) noexcept {
        size_t idx = head_.fetch_add(1, std::memory_order_relaxed) % HISTORY_RING_BUFFER_SIZE;
        buffer_[idx] = static_cast<uint16_t>(std::min<size_t>(len, 65535));

        total_sum_.fetch_add(len, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    double average() const noexcept {
        uint64_t n = count_.load(std::memory_order_acquire);
        if (n == 0) return 0.0;
        return static_cast<double>(total_sum_.load(std::memory_order_acquire)) / n;
    }

    size_t count() const noexcept { return count_.load(std::memory_order_acquire); }
};

// ─────────────────────────────────────────────────────────────────────────────
//                               Worker (consumer)
// ─────────────────────────────────────────────────────────────────────────────

class Worker final {
private:
    DeadlineBoundedPriorityQueue& queue_;
    AdmissionController&          admission_ctrl_;
    LatencyHistogram&             latency_hist_;
    std::atomic<bool>             running_{true};
    std::thread                   thread_;
    std::atomic<uint64_t>         processed_{0};
    std::atomic<uint64_t>         missed_soft_{0};
    std::atomic<uint64_t>         missed_hard_{0};

public:
    Worker(DeadlineBoundedPriorityQueue& q,
           AdmissionController& ctrl,
           LatencyHistogram& hist)
        : queue_(q), admission_ctrl_(ctrl), latency_hist_(hist)
    {
        thread_ = std::thread(&Worker::run, this);
    }

    ~Worker() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void run() {
        while (running_.load(std::memory_order_acquire)) {
            auto opt = queue_.try_pop();
            if (!opt) {
                std::this_thread::sleep_for(200us);
                continue;
            }

            auto req = std::move(opt.value());
            req->started_processing = true;
            req->start_process_time = MonotonicClock::now();

            std::this_thread::sleep_for(req->service_cost_us);

            auto finish_time = MonotonicClock::now();
            req->completion_time   = finish_time;
            req->finished          = true;
            req->actual_latency_us = to_us(finish_time - req->arrival_time);
            req->queue_wait_time_us = to_us(req->start_process_time - req->arrival_time);

            latency_hist_.record(req->actual_latency_us.count());

            if (finish_time > req->soft_deadline) {
                missed_soft_.fetch_add(1, std::memory_order_relaxed);
            }
            if (finish_time > req->hard_deadline) {
                missed_hard_.fetch_add(1, std::memory_order_relaxed);
            }

            admission_ctrl_.update_service_time_observation(req->actual_latency_us);

            processed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t processed()     const noexcept { return processed_.load(std::memory_order_acquire);     }
    uint64_t missed_soft()   const noexcept { return missed_soft_.load(std::memory_order_acquire);   }
    uint64_t missed_hard()   const noexcept { return missed_hard_.load(std::memory_order_acquire);   }
};

// ─────────────────────────────────────────────────────────────────────────────
//                               Request Generator
// ─────────────────────────────────────────────────────────────────────────────

class RequestGenerator final {
private:
    AdmissionController&          admission_ctrl_;
    DeadlineBoundedPriorityQueue& queue_;
    std::atomic<bool>             running_{true};
    std::thread                   thread_;
    std::atomic<uint64_t>         generated_{0};
    std::mt19937_64               rng_;
    std::array<std::uniform_int_distribution<>, 4> distributions_;

public:
    RequestGenerator(AdmissionController& ctrl,
                     DeadlineBoundedPriorityQueue& q)
        : admission_ctrl_(ctrl)
        , queue_(q)
        , rng_(std::random_device{}())
    {
        // service time  : 400 – 3200 µs
        distributions_[0] = std::uniform_int_distribution<>(400, 3200);

        // relative soft deadline : 4–25 ms
        distributions_[1] = std::uniform_int_distribution<>(4000, 25000);

        // inter-arrival : 150–1200 µs
        distributions_[2] = std::uniform_int_distribution<>(150, 1200);

        distributions_[3] = std::uniform_int_distribution<>(0, 100); // for occasional burst

        thread_ = std::thread(&RequestGenerator::run, this);
    }

    ~RequestGenerator() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    void run() {
        uint64_t id = 0;

        while (running_.load(std::memory_order_acquire)) {
            auto now = MonotonicClock::now();

            int service_us   = distributions_[0](rng_);
            int deadline_rel = distributions_[1](rng_);
            int interarrival = distributions_[2](rng_);

            // occasional burst
            if (distributions_[3](rng_) < 8) {
                interarrival = std::max(20, interarrival / 4);
            }

            auto req = std::make_shared<Request>(
                ++id, now,
                microseconds_t(deadline_rel),
                microseconds_t(service_us)
            );

            generated_.fetch_add(1, std::memory_order_relaxed);

            auto decision = admission_ctrl_.decide(req, now);

            if (decision.accepted) {
                req->admitted = true;
                admission_ctrl_.notify_accepted();

                if (!queue_.try_push(req)) {
                    LOG(WARNING, "Admission granted but queue rejected push for {}", req->id);
                }
            } else {
                admission_ctrl_.notify_rejected();
                req->rejection_class = static_cast<int>(decision.reason);

                LOG(DEBUG, "Rejected {} : {}", req->id, decision.message);
            }

            std::this_thread::sleep_for(microseconds_t(interarrival));
        }
    }

    uint64_t generated_count() const noexcept { return generated_.load(std::memory_order_acquire); }
};

// ─────────────────────────────────────────────────────────────────────────────
//                               Statistics Collector
// ─────────────────────────────────────────────────────────────────────────────

class StatisticsCollector final {
private:
    const RequestGenerator&       producer_;
    const AdmissionController&    adm_ctrl_;
    const DeadlineBoundedPriorityQueue& queue_;
    const std::vector<Worker>&    workers_;
    const LatencyHistogram&       latency_hist_;
    QueueLengthHistory            qlen_history_;

    std::atomic<bool> running_{true};
    std::thread       thread_;

public:
    StatisticsCollector(const RequestGenerator& p,
                        const AdmissionController& a,
                        const DeadlineBoundedPriorityQueue& q,
                        const std::vector<Worker>& w,
                        const LatencyHistogram& h)
        : producer_(p), adm_ctrl_(a), queue_(q), workers_(w), latency_hist_(h)
    {
        thread_ = std::thread(&StatisticsCollector::run, this);
    }

    ~StatisticsCollector() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    void run() {
        while (running_.load(std::memory_order_acquire)) {
            qlen_history_.record(queue_.size());
            std::this_thread::sleep_for(250ms);
        }
    }

    void print_final_report(std::ostream& os = std::cout) {
        uint64_t gen     = producer_.generated_count();
        uint64_t admitted = adm_ctrl_.admitted_count();
        uint64_t rejected = adm_ctrl_.rejected_count();

        uint64_t processed = 0;
        uint64_t miss_soft = 0;
        uint64_t miss_hard = 0;

        for (const auto& w : workers_) {
            processed += w.processed();
            miss_soft += w.missed_soft();
            miss_hard += w.missed_hard();
        }

        double admission_rate_pct = gen ? 100.0 * admitted / gen : 0.0;
        double goodput_pct        = gen ? 100.0 * processed / gen : 0.0;
        double avg_qlen           = qlen_history_.average();

        os << "\n";
        os << "═══════════════════════════════════════════════════════════════ FINAL REPORT ════════\n";
        os << std::format("Simulation duration       : ~{} s\n", SIMULATION_DURATION_SECONDS);
        os << std::format("Generated requests        : {:>10}\n", gen);
        os << std::format("Admitted  requests        : {:>10}  ({:5.1f}%)\n", admitted, admission_rate_pct);
        os << std::format("Rejected  requests        : {:>10}  ({:5.1f}%)\n", rejected, 100.0 - admission_rate_pct);
        os << std::format("Processed requests        : {:>10}  ({:5.1f}% goodput)\n", processed, goodput_pct);
        os << std::format("Soft deadline misses      : {:>10}  ({:5.1f}% of processed)\n",
                          miss_soft, processed ? 100.0 * miss_soft / processed : 0.0);
        os << std::format("Hard deadline misses      : {:>10}\n", miss_hard);
        os << std::format("Average queue length      : {:>8.2f}\n", avg_qlen);
        os << std::format("Current EWMA service time : {}\n",
                          format_duration(microseconds_t(static_cast<int64_t>(adm_ctrl_.current_est_us()))));
        os << "───────────────────────────────────────────────────────────────────────────────\n";
        latency_hist_.print(os);
        os << "═══════════════════════════════════════════════════════════════════════════════════\n";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//                                   MAIN
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    GLOBAL_LOGGER.set_level(LogLevel::INFO);
    GLOBAL_LOGGER.enable_timestamps(true);
    GLOBAL_LOGGER.enable_thread_id(false);

    LOG(INFO, "Starting deadline-preserving admission-controlled pipeline simulator");
    LOG(INFO, "Configuration:");
    LOG(INFO, "  queue capacity        = {}", QUEUE_HARD_CAPACITY);
    LOG(INFO, "  soft threshold        = {}%", QUEUE_SOFT_THRESHOLD_PCT);
    LOG(INFO, "  worker threads        = {}", NUMBER_OF_WORKERS);
    LOG(INFO, "  EWMA alpha            = {}%", EWMA_ALPHA_PERCENT);
    LOG(INFO, "  simulation duration   = {} seconds", SIMULATION_DURATION_SECONDS);

    DeadlineBoundedPriorityQueue  queue(QUEUE_HARD_CAPACITY);

    LatencyHistogram              latency_histogram;
    AdmissionController           adm_ctrl(queue, QUEUE_HARD_CAPACITY,
                                           1200us); // initial guess

    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(NUMBER_OF_WORKERS);

    for (int i = 0; i < NUMBER_OF_WORKERS; ++i) {
        workers.emplace_back(std::make_unique<Worker>(queue, adm_ctrl, latency_histogram));
    }

    RequestGenerator              generator(adm_ctrl, queue);

    StatisticsCollector           stats(generator, adm_ctrl, queue,
                                        std::vector<Worker*>{},
                                        latency_histogram);

    // ugly but works — fill references after construction
    for (auto& w : workers) {
        const_cast<std::vector<Worker*>&>(stats.workers_).push_back(*w);
    }

    std::this_thread::sleep_for(std::chrono::seconds(SIMULATION_DURATION_SECONDS));

    LOG(NOTICE, "Simulation time finished → shutting down components...");

    generator.~RequestGenerator();      // explicit stop

    for (auto& w : workers) {
        w.reset();
    }

    LOG(INFO, "All workers stopped.");

    stats.print_final_report();

    LOG(INFO, "Simulation finished cleanly.");

    return 0;
}