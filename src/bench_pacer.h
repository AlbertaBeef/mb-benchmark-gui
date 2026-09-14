// Frame-rate pacing for backends whose producers free-run.
//
// BenchEngine paces by sleeping after run_frame() returns. That works for a
// backend that SUBMITS inside run_frame() -- Hailo, DeepX, and Axelera's
// shipped batched path -- because no new work is offered while we sleep.
//
// It does nothing for a backend whose producer runs on its own thread:
//
//   MemryX    the SDK's threads call on_input() whenever a permit is free, and
//             permits are returned by on_output(), not by us
//   Qualcomm  our own nsps x depth worker_loop() threads
//   Axelera   our own stream_loop() threads, under MB_AXELERA_MULTI_INSTANCE=1
//
// There run_frame() only DRAINS a completion counter, so sleeping in the engine
// slows the counter and not the card. Measured 2026-09-14 on ResNet-50 at
// depth 4, capping a MemryX run at 300 fps: reported frames fell to 357 while
// the rail stayed at 8.24 W against 8.18 W uncapped -- i.e. the device never
// slowed at all. DeepX over the same test fell from 5.33 W to 2.47 W, which is
// what a cap is supposed to look like.
//
// That mattered beyond the control being cosmetic: a capped MemryX run paired a
// throttled frame count with unthrottled watts, so its fps/W and mJ/frame were
// wrong by whatever factor the cap bit.
//
// This pacer moves the wait to the producer, before the frame is submitted.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Thread-safe: several producers may share one pacer (Qualcomm runs
// nsps x depth of them). Absolute deadlines off a fixed origin, so a target
// rate does not drift; the origin is rebased if the device falls far enough
// behind that repaying the debt would become a burst of un-paced frames --
// the same rule BenchEngine's own loop uses, and for the same reason.
class FramePacer {
public:
    using clock = std::chrono::steady_clock;

    // fps <= 0 means unpaced: await() becomes a no-op with no locking at all,
    // so the max-speed path costs nothing.
    void set_target(double fps) {
        std::lock_guard<std::mutex> lk(mu_);
        period_ = fps > 0.0 ? 1.0 / fps : 0.0;
        origin_ = clock::now();
        issued_ = 0;
    }

    bool paced() const { return period_ > 0.0; }

    // Block until the next frame is due. Returns false if stopped while
    // waiting, so a producer can break its loop rather than submit one more.
    //
    // `frames` is what this submission will retire -- 1 everywhere except a
    // fixed-batch Axelera artifact, where one invocation produces N results.
    // Advancing by that keeps a target FPS a *frame* rate rather than becoming
    // an invocation rate, matching BenchEngine::Worker.
    bool await(unsigned frames = 1) {
        if (period_ <= 0.0) return !stop_;          // unpaced: no lock taken
        std::unique_lock<std::mutex> lk(mu_);
        if (stop_) return false;
        const auto deadline =
            origin_ + std::chrono::duration_cast<clock::duration>(
                          std::chrono::duration<double>(issued_ * period_));
        const auto now = clock::now();
        if (now < deadline) {
            // Waiting on the cv rather than sleep_until so stop() cannot leave
            // a producer parked for the rest of a long period -- on MemryX this
            // runs inside an SDK callback thread, and a stream being torn down
            // must not find it asleep.
            if (cv_.wait_until(lk, deadline, [this] { return stop_; })) return false;
        } else if (now - deadline > std::chrono::seconds(1)) {
            origin_ = now;                           // cannot keep up; drop the debt
            issued_ = 0;
        }
        issued_ += frames;
        return true;
    }

    // Wake every waiter. Idempotent, and safe to call from a teardown path.
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    double period_ = 0.0;              // seconds per frame; 0 = unpaced
    clock::time_point origin_ = clock::now();
    std::uint64_t issued_ = 0;
    bool stop_ = false;
};
