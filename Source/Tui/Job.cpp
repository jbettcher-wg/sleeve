// SPDX-License-Identifier: MIT
#include "Job.h"

#include <thread>
#include <utility>

namespace Sleeve::Tui {

namespace {
using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}
} // namespace

// The wake callback is invoked with the mutex held. That is deliberate: it is the only
// thing that makes clearing it a real guarantee rather than a race. The callback posts an
// event to ftxui and never comes back into JobShared, so there is nothing to deadlock on.
void JobShared::Nudge() {
  std::lock_guard<std::mutex> lock(mutex);
  if (wake) {
    wake();
  }
}

void JobShared::PostProgress(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex);
  progress = line;
  if (wake) {
    wake();
  }
}

void JobToken::Fail(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->error = message;
  }
  shared_->ok.store(false, std::memory_order_relaxed);
}

Job::~Job() {
  Detach();
}

void Job::Detach() {
  if (!active_) {
    return;
  }
  active_->cancel.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(active_->mutex);
    active_->wake = nullptr;
  }
  active_.reset();
  deliver_ = nullptr;
}

bool Job::Start(Request request) {
  if (phase_ == JobPhase::Running) {
    return false;
  }

  auto shared = std::make_shared<JobShared>();
  shared->ok.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->wake = wake_;
  }

  active_ = shared;
  deliver_ = std::move(request.deliver);
  label_ = std::move(request.label);
  cancel_hint_ = std::move(request.cancel_hint);
  cancellable_ = request.cancellable;
  phase_ = JobPhase::Running;
  started_ = Clock::now();
  frozen_elapsed_ = 0.0;

  // The thread captures the shared block and the work function by value, so it owns
  // everything it touches. It is detached because the Core calls inside `work` cannot be
  // interrupted, and a UI that has to join one of them is the bug this exists to fix.
  std::thread([shared, work = std::move(request.work)]() mutable {
    auto begun = Clock::now();
    JobToken token(shared);
    work(token);
    shared->elapsed_us.store(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begun).count(),
        std::memory_order_relaxed);
    // Publishes everything the worker wrote, including the result block the caller
    // handed it. Collect() pairs this with an acquire load.
    shared->finished.store(true, std::memory_order_release);
    shared->Nudge();
  }).detach();

  return true;
}

std::string Job::Progress() const {
  if (!active_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(active_->mutex);
  return active_->progress;
}

std::string Job::Error() const {
  return error_;
}

double Job::ElapsedSeconds() const {
  if (phase_ == JobPhase::Running) {
    return SecondsSince(started_);
  }
  return frozen_elapsed_;
}

void Job::Cancel() {
  if (phase_ != JobPhase::Running) {
    return;
  }
  frozen_elapsed_ = SecondsSince(started_);
  Detach();
  phase_ = JobPhase::Cancelled;
}

bool Job::Collect() {
  if (phase_ != JobPhase::Running || !active_) {
    return false;
  }
  if (!active_->finished.load(std::memory_order_acquire)) {
    return false;
  }

  auto shared = active_;
  bool ok = shared->ok.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    error_ = shared->error;
    shared->wake = nullptr;
  }
  frozen_elapsed_ =
      static_cast<double>(shared->elapsed_us.load(std::memory_order_relaxed)) / 1e6;

  auto deliver = std::move(deliver_);
  deliver_ = nullptr;
  active_.reset();
  phase_ = ok ? JobPhase::Done : JobPhase::Failed;

  // Runs on the UI thread, with the Job alive and no worker left to race against, so
  // this is the one place a worker's output is copied into AppState.
  if (ok && deliver) {
    deliver();
  }
  return true;
}

bool Job::Shutdown(std::chrono::milliseconds budget) {
  if (phase_ != JobPhase::Running || !active_) {
    return true;
  }

  auto shared = active_;
  shared->cancel.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->wake = nullptr;
  }

  // A detached worker that is still inside a Core call when the process starts running
  // static destructors is reading strings that are being torn down under it. Give it a
  // bounded chance to come back rather than either hanging on a join or racing exit.
  auto deadline = Clock::now() + budget;
  while (!shared->finished.load(std::memory_order_acquire) && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  bool finished = shared->finished.load(std::memory_order_acquire);

  frozen_elapsed_ = SecondsSince(started_);
  active_.reset();
  deliver_ = nullptr;
  phase_ = JobPhase::Cancelled;
  return finished;
}

} // namespace Sleeve::Tui
