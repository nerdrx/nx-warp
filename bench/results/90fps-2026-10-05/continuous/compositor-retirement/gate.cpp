#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
int failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("FAIL:%d: %s\n", __LINE__, #expr); } } while (false)

enum class result { success, timeout, error };

// Candidate separate-submit-fence retirement state. It does not model Vulkan calls.
struct retirement_gate {
    uint64_t last_submitted = 0;
    uint64_t pending_generation = 0;
    uint64_t completed_generation = 0;
    uint64_t mirror_generation = 0;
    bool submit_pending = false;
    bool mirror_pending = false;

    bool can_acquire() const { return !submit_pending && !mirror_pending; }

    bool submit(result reset, result submit_result, bool mirror_reader = false) {
        if (submit_pending || mirror_pending || reset != result::success || submit_result != result::success)
            return false;
        pending_generation = ++last_submitted;
        submit_pending = true;
        mirror_pending = mirror_reader;
        mirror_generation = mirror_reader ? pending_generation : 0;
        return true;
    }

    // This fence retires only compositor submission work, not a downstream image reader.
    bool observe_fence(uint64_t generation, result status) {
        if (!submit_pending || generation != pending_generation || status != result::success)
            return false;
        completed_generation = generation;
        submit_pending = false;
        return true;
    }

    // A mirror reader has an independent lifetime and completion event.
    bool observe_mirror(uint64_t generation, result status) {
        if (!mirror_pending || generation != mirror_generation || status != result::success)
            return false;
        mirror_pending = false;
        mirror_generation = 0;
        return true;
    }

    // The compositor's existing compute timeline/query/motion flow is independent.
    void observe_compute_timeline(result) {}
};

// Mirrors the existing compositor conditions: query results and motion-field send
// are considered only after the compute timeline wait succeeds; query statistics are
// recorded only on query success. This path never retires the separate full-submit fence.
struct existing_compute_path {
    bool motion_unsafe = false;
    bool query_attempted = false;
    bool query_stats_recorded = false;
    bool motion_send_attempted = false;
    bool motion_pending = true;
    bool compute_error_aborted = false;

    void after_compute_wait(result wait, result query) {
        query_attempted = query_stats_recorded = motion_send_attempted = compute_error_aborted = false;
        if (wait == result::timeout) {
            motion_unsafe = true;
            motion_pending = false;
            return;
        }
        if (wait == result::error) {
            compute_error_aborted = true;
            return;
        }
        motion_unsafe = false;
        query_attempted = true;
        query_stats_recorded = query == result::success;
        motion_send_attempted = true;
    }
};

void initial_and_submit_failures() {
    retirement_gate g;
    CHECK(g.can_acquire()); CHECK(!g.submit_pending && !g.mirror_pending); CHECK(g.completed_generation == 0);
    CHECK(!g.submit(result::error, result::success));
    CHECK(g.last_submitted == 0 && g.completed_generation == 0 && !g.submit_pending && !g.mirror_pending && g.can_acquire());
    CHECK(!g.submit(result::success, result::error));
    CHECK(g.last_submitted == 0 && g.completed_generation == 0 && !g.submit_pending && !g.mirror_pending && g.can_acquire());
}

void healthy_compute_and_mirror_tail_do_not_retire() {
    retirement_gate g;
    CHECK(g.submit(result::success, result::success, true));
    const auto gen = g.pending_generation;
    CHECK(gen == 1 && g.submit_pending && g.mirror_pending && !g.can_acquire());
    CHECK(!g.submit(result::success, result::success));
    CHECK(g.submit_pending && g.pending_generation == gen && !g.can_acquire());

    existing_compute_path path;
    path.after_compute_wait(result::success, result::success);
    // Compute signal can feed a mirror reader, but is neither full-submit fence nor mirror completion.
    CHECK(path.query_attempted && path.query_stats_recorded && path.motion_send_attempted);
    g.observe_compute_timeline(result::success);
    CHECK(g.submit_pending && g.mirror_pending && !g.can_acquire() && g.completed_generation == 0);
    CHECK(g.observe_fence(gen, result::success));
    CHECK(!g.submit_pending && g.mirror_pending && !g.can_acquire());
    CHECK(g.observe_mirror(gen, result::success));
    CHECK(g.can_acquire());
}

void timeout_retry_and_error() {
    retirement_gate g;
    CHECK(g.submit(result::success, result::success));
    const auto gen = g.pending_generation;
    CHECK(!g.observe_fence(gen, result::timeout));
    CHECK(g.submit_pending && !g.can_acquire() && g.completed_generation == 0);

    existing_compute_path path;
    path.after_compute_wait(result::timeout, result::success);
    g.observe_compute_timeline(result::timeout);
    CHECK(path.motion_unsafe && !path.query_attempted && !path.motion_send_attempted && !path.motion_pending);
    CHECK(g.submit_pending && !g.can_acquire());

    CHECK(!g.observe_fence(gen, result::error));
    CHECK(g.submit_pending && !g.can_acquire() && g.completed_generation == 0);
    // A later successful status check of the same full-submit generation retires it.
    path.after_compute_wait(result::success, result::error);
    CHECK(!path.motion_unsafe && path.query_attempted && !path.query_stats_recorded && path.motion_send_attempted);
    CHECK(g.observe_fence(gen, result::success));
    CHECK(!g.submit_pending && g.can_acquire() && g.completed_generation == gen);

    path.motion_pending = true;
    path.after_compute_wait(result::error, result::success);
    CHECK(path.compute_error_aborted && !path.query_attempted && !path.motion_send_attempted && path.motion_pending);
}

void generation_reuse_rejects_old_completion() {
    retirement_gate g;
    CHECK(g.submit(result::success, result::success));
    const auto first = g.pending_generation;
    CHECK(g.observe_fence(first, result::success));
    CHECK(g.can_acquire());
    CHECK(g.submit(result::success, result::success));
    const auto second = g.pending_generation;
    CHECK(second == first + 1 && g.submit_pending);
    CHECK(!g.observe_fence(first, result::success));
    CHECK(g.submit_pending && !g.can_acquire() && g.completed_generation == first);
    CHECK(g.observe_fence(second, result::success));
    CHECK(!g.submit_pending && g.can_acquire() && g.completed_generation == second);
}
} // namespace

int main() {
    initial_and_submit_failures();
    healthy_compute_and_mirror_tail_do_not_retire();
    timeout_retry_and_error();
    generation_reuse_rejects_old_completion();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
