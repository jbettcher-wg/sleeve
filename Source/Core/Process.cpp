// SPDX-License-Identifier: MIT
#include "Process.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <chrono>
#include <vector>
#include <cstring>

namespace Sleeve::Process {

ProcessResult RunCommand(const std::vector<std::string>& args, double timeout_seconds) {
  ProcessOptions opt;
  opt.args = args;
  opt.timeout_seconds = timeout_seconds;
  return RunCommand(opt);
}

ProcessResult RunCommand(const ProcessOptions& options) {
  ProcessResult result;
  if (options.args.empty()) {
    result.exit_code = -1;
    return result;
  }

  auto startTime = std::chrono::steady_clock::now();

  int outPipe[2];
  int errPipe[2];
  if (pipe2(outPipe, O_CLOEXEC) < 0) {
    result.exit_code = -1;
    return result;
  }
  if (pipe2(errPipe, O_CLOEXEC) < 0) {
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    result.exit_code = -1;
    return result;
  }

  pid_t pid = fork();
  if (pid < 0) {
    ::close(outPipe[0]); ::close(outPipe[1]);
    ::close(errPipe[0]); ::close(errPipe[1]);
    result.exit_code = -1;
    return result;
  }

  if (pid == 0) {
    // Child process
    ::dup2(outPipe[1], STDOUT_FILENO);
    ::dup2(errPipe[1], STDERR_FILENO);

    ::close(outPipe[0]); ::close(outPipe[1]);
    ::close(errPipe[0]); ::close(errPipe[1]);

    if (!options.cwd.empty()) {
      if (::chdir(options.cwd.c_str()) != 0) {
        _exit(127);
      }
    }

    for (const auto& [k, v] : options.env) {
      ::setenv(k.c_str(), v.c_str(), 1);
    }

    std::vector<char*> argv;
    for (const auto& a : options.args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  // Parent process
  ::close(outPipe[1]);
  ::close(errPipe[1]);

  int flags = fcntl(outPipe[0], F_GETFL, 0);
  fcntl(outPipe[0], F_SETFL, flags | O_NONBLOCK);
  flags = fcntl(errPipe[0], F_GETFL, 0);
  fcntl(errPipe[0], F_SETFL, flags | O_NONBLOCK);

  pollfd pfds[2];
  pfds[0].fd = outPipe[0];
  pfds[0].events = POLLIN;
  pfds[1].fd = errPipe[0];
  pfds[1].events = POLLIN;

  bool outOpen = true;
  bool errOpen = true;
  char buf[4096];

  while (outOpen || errOpen) {
    if (options.timeout_seconds > 0.0) {
      auto now = std::chrono::steady_clock::now();
      double elapsed = std::chrono::duration<double>(now - startTime).count();
      if (elapsed >= options.timeout_seconds) {
        result.timed_out = true;
        ::kill(pid, SIGTERM);
        // Wait a small grace period then SIGKILL
        ::usleep(50000);
        ::kill(pid, SIGKILL);
        break;
      }
    }

    int pollTimeout = (options.timeout_seconds > 0.0) ? 100 : 500;
    int ret = ::poll(pfds, 2, pollTimeout);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (outOpen && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
      if (n > 0) {
        std::string chunk(buf, n);
        result.stdout_str.append(chunk);
        if (options.on_stdout) options.on_stdout(chunk);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        outOpen = false;
        pfds[0].fd = -1;
      }
    }

    if (errOpen && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = ::read(errPipe[0], buf, sizeof(buf));
      if (n > 0) {
        std::string chunk(buf, n);
        result.stderr_str.append(chunk);
        if (options.on_stderr) options.on_stderr(chunk);
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        errOpen = false;
        pfds[1].fd = -1;
      }
    }
  }

  ::close(outPipe[0]);
  ::close(errPipe[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }

  auto endTime = std::chrono::steady_clock::now();
  result.elapsed_seconds = std::chrono::duration<double>(endTime - startTime).count();

  return result;
}

} // namespace Sleeve::Process
