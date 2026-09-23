// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Sleeve::Tui {

// One long-running action, run off the UI thread.
//
// The threading contract lives here because every other file depends on getting it
// right:
//
//  * A worker thread touches exactly one object it did not create itself: the
//    JobShared block, which it co-owns through a shared_ptr. It never sees AppState,
//    never sees an ftxui object, and never sees the Job. That is what makes it safe
//    for a worker to outlive the Job and the screen -- which it has to be, because
//    the Core calls it makes (Scanner::RunScan, Health::RunCheck) cannot be
//    interrupted once they have started.
//
//  * Results travel the same way. The caller allocates a shared_ptr<T>; the work
//    function (worker thread) fills it and the deliver function (UI thread) copies
//    out of it. Nothing else is shared between the two.
//
//  * `deliver` is held by the Job, not by the shared block, and runs only inside
//    Collect(), on the UI thread, while the Job is alive. That is the single place
//    where a worker's output is written into AppState.
//
//  * The handoff is release/acquire on JobShared::finished: the worker publishes
//    everything it wrote by storing `true`, and Collect() reads it with acquire
//    before it looks at anything. The progress line, the error string and the wake
//    callback are guarded by JobShared::mutex instead, because those are written
//    and read while the worker is still running.
//
//  * The wake callback is stored in the shared block and cleared under the mutex
//    when the UI stops listening (Cancel, Collect, ~Job). An abandoned worker's
//    notifications then go nowhere, so it can never post an event to a screen that
//    has been destroyed.

enum class JobPhase {
  Idle,
  Running,
  Done,
  Cancelled,
  Failed,
};

struct JobShared {
  std::mutex mutex;
  std::string progress;             // guarded by mutex
  std::string error;                // guarded by mutex
  std::function<void()> wake;       // guarded by mutex; cleared when the UI lets go
  std::atomic<bool> cancel {false};
  std::atomic<bool> finished {false};
  std::atomic<bool> ok {false};
  std::atomic<long long> elapsed_us {0};

  // Publish a line of progress and nudge the UI. Safe to call from the worker.
  void PostProgress(const std::string& line);
  void Nudge();
};

// The worker's half of a running job.
class JobToken {
public:
  explicit JobToken(std::shared_ptr<JobShared> shared) : shared_(std::move(shared)) {}

  // True once the UI has stopped waiting. Work that can check this between steps
  // should, and should return early when it does.
  bool Cancelled() const { return shared_->cancel.load(std::memory_order_relaxed); }

  void Progress(const std::string& line) { shared_->PostProgress(line); }

  // Recorded as the failure reason and turns the job's phase into Failed.
  void Fail(const std::string& message);

private:
  friend class Job;
  std::shared_ptr<JobShared> shared_;
};

class Job {
public:
  using Work = std::function<void(JobToken&)>;
  using Deliver = std::function<void()>;

  struct Request {
    std::string label;          // "Scanning for arm64 programs"
    std::string cancel_hint;    // what [esc] does here, in the user's words
    bool cancellable {true};
    Work work;
    Deliver deliver;
  };

  Job() = default;
  ~Job();

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  // Wired once, by whoever owns the screen. Called from worker threads.
  void SetWake(std::function<void()> wake) { wake_ = std::move(wake); }

  // Starts the request unless something is already running, in which case this
  // returns false and does nothing at all. That is what keeps a second press of
  // the same key from starting a second run.
  bool Start(Request request);

  // --- UI thread only, all of it ---

  bool Running() const { return phase_ == JobPhase::Running; }
  JobPhase Phase() const { return phase_; }
  const std::string& Label() const { return label_; }
  const std::string& CancelHint() const { return cancel_hint_; }
  bool Cancellable() const { return cancellable_; }
  std::string Progress() const;
  std::string Error() const;
  double ElapsedSeconds() const;

  // Stops waiting. The worker is told to give up, but if it is inside a Core call
  // that cannot be interrupted it keeps going until that call returns; whatever it
  // produces afterwards is dropped on the floor. No-op unless a job is running.
  void Cancel();

  // Picks up a finished run and hands its result to `deliver`. Returns true exactly
  // once per run, on the event that completes it.
  bool Collect();

  // Called before the screen goes away. Cancels, then gives the worker a bounded
  // chance to return so that it is not still inside Core while the process tears
  // its statics down. Returns true if the worker finished within the budget.
  bool Shutdown(std::chrono::milliseconds budget);

private:
  void Detach();

  std::function<void()> wake_;
  std::shared_ptr<JobShared> active_;
  Deliver deliver_;
  std::string label_;
  std::string cancel_hint_;
  std::string error_;
  bool cancellable_ {true};
  JobPhase phase_ {JobPhase::Idle};
  std::chrono::steady_clock::time_point started_;
  double frozen_elapsed_ {0.0};
};

} // namespace Sleeve::Tui
