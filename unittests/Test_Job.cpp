// SPDX-License-Identifier: MIT
//
// The TUI's background-work layer. None of this needs a terminal: a Job is just the
// handshake between the UI thread and one worker, and that handshake is the part that
// decides whether a shortcut freezes the screen, starts twice, or writes into state
// somebody else is reading.
//
// Everything a work function touches here is reached through a shared_ptr, never through
// a reference to a test's stack. That is not fussiness: a Job's worker is detached and
// may still be running after the test that started it has returned, which is the whole
// point of the class. Capturing a stack local would be the exact bug these tests exist
// to rule out, and ThreadSanitizer says so.

#include <catch2/catch_test_macros.hpp>

#include "Job.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace Sleeve::Tui;
using namespace std::chrono_literals;

namespace {

// A gate the test opens when it wants the worker to proceed, so a test can hold a job in
// the "still running" state for as long as it needs without sleeping.
class Gate {
public:
  void Open() {
    {
      std::lock_guard<std::mutex> lock(m_);
      open_ = true;
    }
    cv_.notify_all();
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait_for(lock, 5s, [this] { return open_; });
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  bool open_ {false};
};

using GatePtr = std::shared_ptr<Gate>;
using FlagPtr = std::shared_ptr<std::atomic<bool>>;
using CountPtr = std::shared_ptr<std::atomic<int>>;
using TextPtr = std::shared_ptr<std::string>;

GatePtr MakeGate() { return std::make_shared<Gate>(); }
FlagPtr MakeFlag() { return std::make_shared<std::atomic<bool>>(false); }
CountPtr MakeCount() { return std::make_shared<std::atomic<int>>(0); }

// Spins Collect() the way the event loop does, until it reports a finished run.
bool CollectWithin(Job& job, std::chrono::milliseconds budget) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (job.Collect()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

bool WaitFor(const FlagPtr& flag, std::chrono::milliseconds budget) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (flag->load()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return flag->load();
}

bool WaitForAtLeast(const CountPtr& count, int target, std::chrono::milliseconds budget) {
  auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (count->load() >= target) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return count->load() >= target;
}

} // namespace

TEST_CASE("A job runs its work off the calling thread and delivers once", "[job]") {
  Job job;
  auto result = std::make_shared<std::string>();
  auto on_worker = MakeFlag();
  int delivered = 0;
  std::thread::id caller = std::this_thread::get_id();

  Job::Request req;
  req.label = "Scanning";
  req.work = [result, on_worker, caller](JobToken&) {
    on_worker->store(std::this_thread::get_id() != caller);
    *result = "eight apps";
  };
  req.deliver = [&delivered, result]() {
    ++delivered;
    REQUIRE(*result == "eight apps");
  };

  REQUIRE(job.Start(std::move(req)));
  REQUIRE(job.Label() == "Scanning");

  REQUIRE(CollectWithin(job, 5s));
  CHECK(on_worker->load());
  CHECK(delivered == 1);
  CHECK(job.Phase() == JobPhase::Done);
  CHECK_FALSE(job.Running());

  // The event loop calls Collect on every event; only the event that completes the run
  // may deliver.
  CHECK_FALSE(job.Collect());
  CHECK(delivered == 1);
}

TEST_CASE("A second start while one is running is refused, not queued", "[job]") {
  Job job;
  auto gate = MakeGate();
  auto runs = MakeCount();

  auto make = [gate, runs](const std::string& label) {
    Job::Request req;
    req.label = label;
    req.work = [gate, runs](JobToken&) {
      ++*runs;
      gate->Wait();
    };
    return req;
  };

  REQUIRE(job.Start(make("first")));
  REQUIRE(job.Running());
  // This is the second press of the same key while its action is running.
  CHECK_FALSE(job.Start(make("second")));
  CHECK(job.Label() == "first");

  gate->Open();
  REQUIRE(CollectWithin(job, 5s));
  CHECK(runs->load() == 1);

  // Once it has finished the slot is free again.
  Job::Request again;
  again.label = "third";
  again.work = [](JobToken&) {};
  CHECK(job.Start(std::move(again)));
  REQUIRE(CollectWithin(job, 5s));
  CHECK(job.Label() == "third");
}

TEST_CASE("Cancelling stops the wait, drops the result and frees the slot", "[job]") {
  Job job;
  auto gate = MakeGate();
  auto saw_cancel = MakeFlag();
  auto finished = MakeFlag();
  int delivered = 0;

  Job::Request req;
  req.label = "Scanning";
  req.work = [gate, saw_cancel, finished](JobToken& token) {
    gate->Wait();
    // Work that can notice is expected to; work stuck inside an uninterruptible Core
    // call simply runs to completion and has its result dropped.
    saw_cancel->store(token.Cancelled());
    finished->store(true);
  };
  req.deliver = [&delivered]() { ++delivered; };
  REQUIRE(job.Start(std::move(req)));

  job.Cancel();
  CHECK(job.Phase() == JobPhase::Cancelled);
  CHECK_FALSE(job.Running());

  gate->Open();
  REQUIRE(WaitFor(finished, 5s));
  CHECK(saw_cancel->load());

  // The abandoned worker's result never reaches the UI.
  CHECK_FALSE(job.Collect());
  CHECK(delivered == 0);

  // The slot is free straight away, even though the old worker only just returned:
  // pressing the key again after cancelling has to work.
  Job::Request next;
  next.label = "Scanning again";
  next.work = [](JobToken&) {};
  CHECK(job.Start(std::move(next)));
  REQUIRE(CollectWithin(job, 5s));
}

TEST_CASE("Cancel unhooks the wake so an orphan cannot post to a dead screen", "[job]") {
  Job job;
  auto gate = MakeGate();
  auto wakes = MakeCount();
  auto finished = MakeFlag();

  job.SetWake([wakes]() { ++*wakes; });

  Job::Request req;
  req.label = "Health check";
  req.work = [gate, finished](JobToken& token) {
    token.Progress("running");
    gate->Wait();
    token.Progress("still running");
    finished->store(true);
  };
  REQUIRE(job.Start(std::move(req)));

  // The first progress line woke the loop.
  REQUIRE(WaitForAtLeast(wakes, 1, 5s));
  int before = wakes->load();

  job.Cancel();
  gate->Open();
  REQUIRE(WaitFor(finished, 5s));
  // Give the worker's post-cancel Progress and its finishing Nudge every chance to land.
  std::this_thread::sleep_for(50ms);
  CHECK(wakes->load() == before);
}

TEST_CASE("Progress is readable while the worker runs", "[job]") {
  Job job;
  auto gate = MakeGate();
  auto posted = MakeFlag();

  Job::Request req;
  req.label = "Writing";
  req.work = [gate, posted](JobToken& token) {
    token.Progress("writing ~/.local/bin/code");
    posted->store(true);
    gate->Wait();
  };
  REQUIRE(job.Start(std::move(req)));

  REQUIRE(WaitFor(posted, 5s));
  CHECK(job.Progress() == "writing ~/.local/bin/code");
  CHECK(job.ElapsedSeconds() >= 0.0);

  gate->Open();
  REQUIRE(CollectWithin(job, 5s));
}

TEST_CASE("Failed work reports why and delivers nothing", "[job]") {
  Job job;
  int delivered = 0;

  Job::Request req;
  req.label = "Writing";
  req.work = [](JobToken& token) { token.Fail("could not write ~/.local/bin/code"); };
  req.deliver = [&delivered]() { ++delivered; };
  REQUIRE(job.Start(std::move(req)));

  REQUIRE(CollectWithin(job, 5s));
  CHECK(job.Phase() == JobPhase::Failed);
  CHECK(job.Error() == "could not write ~/.local/bin/code");
  CHECK(delivered == 0);
}

TEST_CASE("A job can be marked as not cancellable", "[job]") {
  Job job;
  auto gate = MakeGate();

  Job::Request req;
  req.label = "Writing 3 file(s)";
  req.cancel_hint = "writing cannot be interrupted";
  req.cancellable = false;
  req.work = [gate](JobToken&) { gate->Wait(); };
  REQUIRE(job.Start(std::move(req)));

  CHECK_FALSE(job.Cancellable());
  CHECK(job.CancelHint() == "writing cannot be interrupted");

  gate->Open();
  REQUIRE(CollectWithin(job, 5s));
}

TEST_CASE("Shutdown waits for a worker but only up to its budget", "[job]") {
  {
    Job job;
    auto gate = MakeGate();
    auto started = MakeFlag();

    Job::Request req;
    req.label = "Health check";
    req.work = [gate, started](JobToken&) {
      started->store(true);
      gate->Wait();
    };
    REQUIRE(job.Start(std::move(req)));
    REQUIRE(WaitFor(started, 5s));

    auto begun = std::chrono::steady_clock::now();
    CHECK_FALSE(job.Shutdown(120ms));
    // Quitting must not wait on a Core call that cannot be interrupted.
    CHECK(std::chrono::steady_clock::now() - begun < 3s);

    gate->Open();
  }

  {
    Job job;
    auto done = MakeFlag();
    Job::Request req;
    req.label = "Theme";
    req.work = [done](JobToken&) { done->store(true); };
    REQUIRE(job.Start(std::move(req)));
    CHECK(job.Shutdown(5s));
    CHECK(done->load());
  }
}

TEST_CASE("Destroying a job while a worker runs does not take the worker with it", "[job]") {
  auto gate = MakeGate();
  auto ran_to_completion = MakeFlag();
  auto result = std::make_shared<std::string>();

  {
    Job job;
    Job::Request req;
    req.label = "Scanning";
    req.work = [gate, ran_to_completion, result](JobToken&) {
      gate->Wait();
      // Everything the worker touches is co-owned through a shared_ptr, so this is still
      // valid memory even though the Job it belonged to is gone.
      *result = "done";
      ran_to_completion->store(true);
    };
    req.deliver = []() { FAIL("deliver must not run for an abandoned job"); };
    REQUIRE(job.Start(std::move(req)));
  }

  gate->Open();
  REQUIRE(WaitFor(ran_to_completion, 5s));
  CHECK(*result == "done");
}
