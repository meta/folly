/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Subprocess library, modeled after Python's subprocess module
 * (http://docs.python.org/2/library/subprocess.html)
 *
 * This library defines one class (Subprocess) which represents a child
 * process.  Subprocess has two constructors: one that takes a vector<string>
 * and executes the given executable without using the shell, and one
 * that takes a string and executes the given command using the shell.
 * Subprocess allows you to redirect the child's standard input, standard
 * output, and standard error to/from child descriptors in the parent,
 * or to create communication pipes between the child and the parent.
 *
 * The simplest example is a thread-safe [1] version of the system() library
 * function:
 *    Subprocess(cmd).wait();
 * which executes the command using the default shell and waits for it
 * to complete, returning the exit status.
 *
 * A thread-safe [1] version of popen() (type="r", to read from the child):
 *    Subprocess proc(cmd, Subprocess::Options().pipeStdout());
 *    // read from proc.stdoutFd()
 *    proc.wait();
 *
 * A thread-safe [1] version of popen() (type="w", to write to the child):
 *    Subprocess proc(cmd, Subprocess::Options().pipeStdin());
 *    // write to proc.stdinFd()
 *    proc.wait();
 *
 * If you want to redirect both stdin and stdout to pipes, you can, but note
 * that you're subject to a variety of deadlocks.  You'll want to use
 * nonblocking I/O, like the callback version of communicate().
 *
 * The string or IOBuf-based variants of communicate() are the simplest way
 * to communicate with a child via its standard input, standard output, and
 * standard error.  They buffer everything in memory, so they are not great
 * for large amounts of data (or long-running processes), but they are much
 * simpler than the callback version.
 *
 * == A note on thread-safety ==
 *
 * [1] "thread-safe" refers ONLY to the fact that Subprocess is very careful
 * to fork in a way that does not cause grief in multithreaded programs.
 *
 * Caveat: If your system does not have the atomic pipe2 system call, it is
 * not safe to concurrently call Subprocess from different threads.
 * Therefore, it is best to have a single thread be responsible for spawning
 * subprocesses.
 *
 * A particular instances of Subprocess is emphatically **not** thread-safe.
 * If you need to simultaneously communicate via the pipes, and interact
 * with the Subprocess state, your best bet is to:
 *  - takeOwnershipOfPipes() to separate the pipe I/O from the subprocess.
 *  - Only interact with the Subprocess from one thread at a time.
 *
 * The current implementation of communicate() cannot be safely interrupted.
 * To do so correctly, one would need to use EventFD, or open a dedicated
 * pipe to be messaged from a different thread -- in particular, kill() will
 * not do, since a descendant may keep the pipes open indefinitely.
 *
 * So, once you call communicate(), you must wait for it to return, and not
 * touch the pipes from other threads.  closeParentFd() is emphatically
 * unsafe to call concurrently, and even sendSignal() is not a good idea.
 * You can perhaps give the Subprocess's PID to a different thread before
 * starting communicate(), and use that PID to send a signal without
 * accessing the Subprocess object.  In that case, you will need a mutex
 * that ensures you don't wait() before you sent said signal.  In a
 * nutshell, don't do this.
 *
 * In fact, signals are inherently concurrency-unsafe on Unix: if you signal
 * a PID, while another thread is in waitpid(), the signal may fire either
 * before or after the process is reaped.  This means that your signal can,
 * in pathological circumstances, be delivered to the wrong process (ouch!).
 * To avoid this, you should only use non-blocking waits (i.e. poll()), and
 * make sure to serialize your signals (i.e. kill()) with the waits --
 * either wait & signal from the same thread, or use a mutex.
 */

#pragma once

#ifndef _WIN32

#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <chrono>
#include <compare>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/Exception.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/Function.h>
#include <folly/MapUtil.h>
#include <folly/Optional.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/container/sorted_vector_types.h>
#include <folly/gen/String.h>
#include <folly/io/IOBufQueue.h>
#include <folly/portability/SysResource.h>

namespace folly {

namespace detail {

/// SubprocessFdActionsList
///
/// A sorted vector-map with a binary search. Declared in the header so that the
/// binary search can be unit-tested.
///
/// Not using a library container type since this binary search is done in the
/// child process after a vfork(), including in sanitized builds. Relevant
/// member functions are explicitly marked non-sanitized (under clang).
class SubprocessFdActionsList {
 private:
  using value_type = std::pair<int, int>;

  value_type const* begin_;
  value_type const* end_;

 public:
  explicit SubprocessFdActionsList(std::span<value_type const> rep) noexcept;

  value_type const* begin() const noexcept;
  value_type const* end() const noexcept;

  int const* find(int fd) const noexcept;
};

} // namespace detail

/**
 * Class to wrap a process return code.
 */
class Subprocess;
class ProcessReturnCode {
 public:
  enum State {
    // Subprocess starts in the constructor, so this state designates only
    // default-initialized or moved-out ProcessReturnCodes.
    NOT_STARTED,
    RUNNING,
    EXITED,
    KILLED,
  };

  static ProcessReturnCode makeNotStarted() {
    return ProcessReturnCode(RV_NOT_STARTED);
  }

  static ProcessReturnCode makeRunning() {
    return ProcessReturnCode(RV_RUNNING);
  }

  static ProcessReturnCode make(int status);

  // Default-initialized for convenience. Subprocess::returnCode() will
  // never produce this value.
  ProcessReturnCode() : rawStatus_(RV_NOT_STARTED) {}

  // Trivially copyable
  ProcessReturnCode(const ProcessReturnCode& p) = default;
  ProcessReturnCode& operator=(const ProcessReturnCode& p) = default;
  // Non-default move: In order for Subprocess to be movable, the "moved
  // out" state must not be "running", or ~Subprocess() will abort.
  ProcessReturnCode(ProcessReturnCode&& p) noexcept;
  ProcessReturnCode& operator=(ProcessReturnCode&& p) noexcept;

  /**
   * Process state.  One of:
   * NOT_STARTED: process hasn't been started successfully
   * RUNNING: process is currently running
   * EXITED: process exited (successfully or not)
   * KILLED: process was killed by a signal.
   */
  State state() const;

  /**
   * Helper wrappers around state().
   */
  bool notStarted() const { return state() == NOT_STARTED; }
  bool running() const { return state() == RUNNING; }
  bool exited() const { return state() == EXITED; }
  bool killed() const { return state() == KILLED; }

  /**
   * Exit status.  Only valid if state() == EXITED; throws otherwise.
   */
  int exitStatus() const;

  /**
   * Signal that caused the process's termination.  Only valid if
   * state() == KILLED; throws otherwise.
   */
  int killSignal() const;

  /**
   * Was a core file generated?  Only valid if state() == KILLED; throws
   * otherwise.
   */
  bool coreDumped() const;

  /**
   * Process exited normally with a zero exit status
   */
  bool succeeded() const;

  /**
   * String representation; one of
   * "not started"
   * "running"
   * "exited with status <status>"
   * "killed by signal <signal>"
   * "killed by signal <signal> (core dumped)"
   */
  std::string str() const;

  /**
   * Helper function to enforce a precondition based on this.
   * Throws std::logic_error if in an unexpected state.
   */
  void enforce(State expected) const;

 private:
  explicit ProcessReturnCode(int rv) : rawStatus_(rv) {}
  static constexpr int RV_NOT_STARTED = -2;
  static constexpr int RV_RUNNING = -1;

  int rawStatus_;
};

/**
 * Base exception thrown by the Subprocess methods.
 */
class FOLLY_EXPORT SubprocessError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/**
 * Exception thrown by *Checked methods of Subprocess.
 */
class FOLLY_EXPORT CalledProcessError : public SubprocessError {
 public:
  explicit CalledProcessError(ProcessReturnCode rc);
  ~CalledProcessError() noexcept override = default;
  ProcessReturnCode returnCode() const { return returnCode_; }

 private:
  ProcessReturnCode returnCode_;
};

/**
 * Exception thrown if the subprocess cannot be started.
 */
class FOLLY_EXPORT SubprocessSpawnError : public SubprocessError {
 public:
  SubprocessSpawnError(const char* executable, int errCode, int errnoValue);
  ~SubprocessSpawnError() noexcept override = default;
  int errnoValue() const { return errnoValue_; }

 private:
  int errnoValue_;
};

/**
 * Subprocess.
 */
class Subprocess {
 public:
  using TimeoutDuration = std::chrono::milliseconds;

  // removed CLOSE = -1
  static const int PIPE = -2;
  static const int PIPE_IN = -3;
  static const int PIPE_OUT = -4;
  static const int DEV_NULL = -5;
  static const int NO_CLOEXEC = -6;

  /**
   * Class representing various options: file descriptor behavior, and
   * whether to use $PATH for searching for the executable,
   *
   * By default, we don't use $PATH, file descriptors are closed if
   * the close-on-exec flag is set (fcntl FD_CLOEXEC) and inherited
   * otherwise.
   */
  class Options {
    friend class Subprocess;

   public:
    // digits10 is the maximum number of decimal digits such that any number
    // up to this many decimal digits can always be represented in the given
    // integer type
    // but we need to have storage for the decimal representation of any
    // integer, so +1, and we need to have storage for the terminal null, so
    // again +1.
    static inline constexpr size_t kPidBufferMinSize =
        std::numeric_limits<pid_t>::digits10 + 2;

    Options() {} // E.g. https://gcc.gnu.org/bugzilla/show_bug.cgi?id=58328

    /**
     * Change action for file descriptor fd.
     *
     * "action" may be another file descriptor number (dup2()ed before the
     * child execs), or one of CLOSE, PIPE_IN, and PIPE_OUT.
     *
     * CLOSE: close the file descriptor in the child
     * PIPE_IN: open a pipe *from* the child
     * PIPE_OUT: open a pipe *to* the child
     *
     * PIPE is a shortcut; same as PIPE_IN for stdin (fd 0), same as
     * PIPE_OUT for stdout (fd 1) or stderr (fd 2), and an error for
     * other file descriptors.
     */
    Options& fd(int fd, int action);

    /**
     * Shortcut to change the action for standard input.
     */
    Options& stdinFd(int action) { return fd(STDIN_FILENO, action); }

    /**
     * Shortcut to change the action for standard output.
     */
    Options& stdoutFd(int action) { return fd(STDOUT_FILENO, action); }

    /**
     * Shortcut to change the action for standard error.
     * Note that stderr(1) will redirect the standard error to the same
     * file descriptor as standard output; the equivalent of bash's "2>&1"
     */
    Options& stderrFd(int action) { return fd(STDERR_FILENO, action); }

    Options& pipeStdin() { return fd(STDIN_FILENO, PIPE_IN); }
    Options& pipeStdout() { return fd(STDOUT_FILENO, PIPE_OUT); }
    Options& pipeStderr() { return fd(STDERR_FILENO, PIPE_OUT); }

    /**
     * Close all other fds (other than standard input, output, error,
     * and file descriptors explicitly specified with fd()).
     *
     * This is potentially slow; it's generally a better idea to
     * set the close-on-exec flag on all file descriptors that shouldn't
     * be inherited by the child.
     *
     * Even with this option set, standard input, output, and error are
     * not closed; use stdin(CLOSE), stdout(CLOSE), stderr(CLOSE) if you
     * desire this.
     */
    Options& closeOtherFds() {
      closeOtherFds_ = true;
      return *this;
    }

    /**
     * Use the search path ($PATH) when searching for the executable.
     */
    Options& usePath() {
      usePath_ = true;
      return *this;
    }

    /**
     * Change the child's working directory, after the vfork.
     */
    Options& chdir(const std::string& dir) {
      childDir_ = dir;
      return *this;
    }

#if defined(__linux__)
    /**
     * Child will receive a signal when the parent *thread* exits.
     *
     * This is especially important when this option is used but the calling
     * thread does not block for the duration of the subprocess. If the original
     * thread that created the subprocess ends then the subprocess will
     * terminate. For example, thread pool executors which can reap unused
     * threads may trigger this behavior.
     */
    Options& parentDeathSignal(int sig) {
      parentDeathSignal_ = sig;
      return *this;
    }
#endif

    /**
     * Child will be made a process group leader when it starts. Upside: one
     * can reliably kill all its non-daemonizing descendants.  Downside: the
     * child will not receive Ctrl-C etc during interactive use.
     */
    Options& processGroupLeader() {
      processGroupLeader_ = true;
      return *this;
    }

    /**
     * Detach the spawned process, to allow destroying the Subprocess object
     * without waiting for the child process to finish.
     *
     * This causes the code to vfork twice before executing the command. The
     * intermediate child process will exit immediately after execve, causing
     * the process running the executable to be reparented to init (pid 1).
     *
     * Subprocess objects created with detach() enabled will already be in an
     * "EXITED" state when the constructor returns.  The caller should not call
     * wait() or poll() on the Subprocess, and pid() will return -1.
     */
    Options& detach() {
      detach_ = true;
      return *this;
    }

    /**
     * If the Subprocess object is destroyed while the process is still running,
     * automatically kill the child with SIGKILL and wait on the pid.
     */
    Options& killChildOnDestruction() {
      destroyBehavior_ = 0;
      return *this;
    }

    /**
     * If the Subprocess object is destroyed while the process is still running,
     * use terminateOrKill() to stop it and wait for it to exit.
     *
     * Beware that this may cause the Subprocess destructor to block while
     * waiting on the child process to exit.
     */
    Options& terminateChildOnDestruction(TimeoutDuration timeout) {
      destroyBehavior_ = std::max(TimeoutDuration::rep(0), timeout.count());
      return *this;
    }

    /**
     * By default, if Subprocess is destroyed while the child process is
     * still RUNNING, the destructor will log a fatal.  You can skip this
     * behavior by setting it to true here.
     *
     * Note that detach()ed processes are never in RUNNING state, so this
     * setting does not impact such processes.
     *
     * BEWARE: setting this flag can leave zombie processes behind on the system
     * after the folly::Subprocess is destroyed.  In general you should avoid
     * using this setting.  In general, prefer using one of the following
     * options instead:
     * - If you do not care about monitoring the child process or waiting for it
     *   to complete, use detach().
     * - If you want to automatically clean up the child process when the
     *   Subprocess is destroyed, use killChildOnDestruction() or
     *   terminateChildOnDestruction()
     * - If you want to allow the parent process to exit without waiting on the
     *   child, prefer simply leaking the folly::Subprocess object when the
     *   parent process exits.  You could exit with _exit(), or you could
     *   explicitly leak the Subprocess using std::unique_ptr::release() or
     *   similar mechanisms.
     */
    Options& allowDestructionWhileProcessRunning(bool val) {
      destroyBehavior_ = val ? DestroyBehaviorLeak : DestroyBehaviorFatal;
      return *this;
    }

#if defined(__linux__)
    Options& setCpuSet(
        const cpu_set_t& cpuSet, std::shared_ptr<int> errout = nullptr) {
      cpuSet_ = AttrWithMeta<cpu_set_t>{cpuSet, std::move(errout)};
      return *this;
    }

    /*
     * setLinuxCGroup*
     * Takes a fd or a path to the cgroup dir. Only one may be provided.
     * Note that the cgroup filesystem may be mounted at any arbitrary point in
     * the filesystem hierarchy, and that different distributions may have their
     * own standard points. So just taking a cgroup name would be non-portable.
     */
    Options& setLinuxCGroupFd(
        int cgroupFd, std::shared_ptr<int> errout = nullptr);
    Options& setLinuxCGroupPath(
        const std::string& cgroupPath, std::shared_ptr<int> errout = nullptr);

    /**
     * Ask the kernel to create the child directly inside the cgroup configured
     * by setLinuxCGroup*, using clone3(CLONE_INTO_CGROUP), instead of letting
     * the child migrate itself by writing to cgroup.procs after the fork.
     *
     * Migrating an existing task takes the write side of the kernel's
     * cgroup_threadgroup_rwsem while cgroup_mutex is held; on a percpu-rwsem
     * that costs an RCU grace period, and it serializes other cgroup operations
     * that take cgroup_mutex. Creating the task in place needs only the read
     * side that fork already takes.
     *
     * If clone3 fails before creating a child, the spawn transparently falls
     * back to vfork plus the cgroup.procs write, preserving the legacy
     * placement and errout contract. Once clone3 succeeds there is no retry. A
     * task born in a cgroup can be observed or treated differently from one
     * migrated there after fork, so enable this only for targets that support
     * born-in-place execution. Use spawnedIntoLinuxCGroup() to find out which
     * path a given spawn took.
     *
     * Requires Linux 5.7+ and, currently, x86-64 or AArch64.
     */
    Options& setLinuxCGroupUseClone3(bool useClone3 = true) {
      linuxCGroupUseClone3_ = useClone3;
      return *this;
    }
#endif

    Options& setUid(uid_t uid, std::shared_ptr<int> errout = nullptr) {
      uid_ = AttrWithMeta<uid_t>{uid, std::move(errout)};
      return *this;
    }
    Options& setGid(gid_t gid, std::shared_ptr<int> errout = nullptr) {
      gid_ = AttrWithMeta<gid_t>{gid, std::move(errout)};
      return *this;
    }
    Options& setEUid(uid_t uid, std::shared_ptr<int> errout = nullptr) {
      euid_ = AttrWithMeta<uid_t>{uid, std::move(errout)};
      return *this;
    }
    Options& setEGid(gid_t gid, std::shared_ptr<int> errout = nullptr) {
      egid_ = AttrWithMeta<gid_t>{gid, std::move(errout)};
      return *this;
    }

    Options& setSignalMask(sigset_t sigmask) {
      sigmask_ = sigmask;
      return *this;
    }

    /// Write the child's PID into buf before execve. The exec'd process
    /// will see the written value (e.g. via an environment variable backed
    /// by this buffer). On platforms where vfork does not share address
    /// space (e.g. Apple), the write may not be visible to the parent.
    Options& addPrintPidToBuffer(std::span<char> buf);

    Options& addRLimit(
        int resource, rlimit limit, std::shared_ptr<int> errout = nullptr);

   private:
    template <typename T>
    struct AttrWithMeta {
      T value{};

      /// nullptr if required, ptr if optional to report failure
      std::shared_ptr<int> erroutLifetime_{}; // do not access in child
      int* errout{erroutLifetime_.get()};
    };

    using FdMap = folly::sorted_vector_map<int, int>;
    FdMap fdActions_;
    bool closeOtherFds_{false};
    bool usePath_{false};
    bool processGroupLeader_{false};
    bool detach_{false};
    // The behavior to take if the Subprocess destructor is invoked while the
    // child process is still running.  This is either
    // DestroyBehaviorFatal, DestroyBehaviorLeak, or a timeout value to pass to
    // terminateOrKill() to kill the child process.
    TimeoutDuration::rep destroyBehavior_{DestroyBehaviorFatal};
    std::string childDir_; // "" keeps the parent's working directory
    AttrWithMeta<int> linuxCGroupFd_{-1, nullptr}; // -1 means no cgroup
    AttrWithMeta<std::string> linuxCGroupPath_{}; // empty means no cgroup
#if defined(__linux__)
    bool linuxCGroupUseClone3_{false};
    int parentDeathSignal_{0};
    Optional<AttrWithMeta<cpu_set_t>> cpuSet_;
#endif
    Optional<AttrWithMeta<uid_t>> uid_;
    Optional<AttrWithMeta<gid_t>> gid_;
    Optional<AttrWithMeta<uid_t>> euid_;
    Optional<AttrWithMeta<gid_t>> egid_;
    Optional<sigset_t> sigmask_;
    std::unordered_set<char*> setPrintPidToBuffer_;
    std::unordered_map<int, AttrWithMeta<rlimit>> rlimits_;
  };

  // Non-copyable, but movable
  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;
  Subprocess(Subprocess&&) = default;
  Subprocess& operator=(Subprocess&&) = default;

  /**
   * Create an uninitialized subprocess.
   *
   * In this state it can only be destroyed, or assigned to using the move
   * assignment operator.
   */
  Subprocess();

  /**
   * Create a subprocess from the given arguments.  argv[0] must be listed.
   * If not-null, executable must be the actual executable
   * being used (otherwise it's the same as argv[0]).
   *
   * If env is not-null, it must contain name=value strings to be used
   * as the child's environment; otherwise, we inherit the environment
   * from the parent.  env must be null if options.usePath is set.
   */
  explicit Subprocess(
      const std::vector<std::string>& argv,
      const Options& options = Options(),
      const char* executable = nullptr,
      const std::vector<std::string>* env = nullptr);
  ~Subprocess();

  /**
   * Create a Subprocess object for an existing child process ID.
   *
   * The process ID must refer to an immediate child process of the current
   * process.  This allows using the poll() and wait() APIs on a process ID
   * that was not originally spawned by Subprocess.
   */
  static Subprocess fromExistingProcess(pid_t pid);

  /**
   * Create a subprocess run as a shell command (as shell -c 'command')
   *
   * The shell to use is taken from the environment variable $SHELL,
   * or /bin/sh if $SHELL is unset.
   */
  // clang-format off
  [[deprecated(
      "Prefer not running in a shell or use `shellify`.")]]
  explicit Subprocess(
      const std::string& cmd,
      const Options& options = Options(),
      const std::vector<std::string>* env = nullptr);
  // clang-format on

  ////
  //// The methods below only manipulate the process state, and do not
  //// affect its communication pipes.
  ////

  /**
   * Return the child's pid, or -1 if the child wasn't successfully spawned
   * or has already been wait()ed upon.
   */
  pid_t pid() const;

#if defined(__linux__)
  /**
   * Whether the kernel created this child directly inside the cgroup requested
   * by Options::setLinuxCGroup*, as asked for by setLinuxCGroupUseClone3().
   *
   * False means the child was placed the ordinary way, by writing cgroup.procs
   * after the fork -- either because clone3(CLONE_INTO_CGROUP) was not
   * requested, or because it was unavailable and the spawn fell back. It says
   * nothing about whether the child reached exec or survived target-cgroup
   * policy. Legacy placement errors are still reported through the
   * setLinuxCGroup* errout.
   */
  bool spawnedIntoLinuxCGroup() const noexcept {
    return spawnedIntoLinuxCGroup_;
  }
#endif

  /**
   * Return the child's status (as per wait()) if the process has already
   * been waited on, -1 if the process is still running, or -2 if the
   * process hasn't been successfully started.  NOTE that this does not call
   * waitpid() or Subprocess::poll(), but simply returns the status stored
   * in the Subprocess object.
   */
  ProcessReturnCode returnCode() const { return returnCode_; }

  /**
   * Poll the child's status and return it. Return the exit status if the
   * subprocess had quit, or RUNNING otherwise.  Throws an std::logic_error
   * if called on a Subprocess whose status is no longer RUNNING.  No other
   * exceptions are possible.  Aborts on egregious violations of contract,
   * e.g. if you wait for the underlying process without going through this
   * Subprocess instance.
   */
  ProcessReturnCode poll(struct rusage* ru = nullptr);

  /**
   * Poll the child's status.  If the process is still running, return false.
   * Otherwise, return true if the process exited with status 0 (success),
   * or throw CalledProcessError if the process exited with a non-zero status.
   * Note: this should only be called for processes in the RUNNING state. If
   * another trigger has caused the process to register as exited, or the
   * process is yet to start before this is called, this will throw a
   * std::logic_error.
   */
  bool pollChecked();

  /**
   * Wait for the process to terminate and return its status.  Like poll(),
   * the only exception this can throw is std::logic_error if you call this
   * on a Subprocess whose status is not RUNNING.  Aborts on egregious
   * violations of contract, like an out-of-band waitpid(p.pid(), 0, 0).
   */
  ProcessReturnCode wait();

  /**
   * Wait for the process to terminate and return its status and rusage.  Like
   * poll(), the only exception this can throw is std::logic_error if you call
   * this on a Subprocess whose status is not RUNNING.  Aborts on egregious
   * violations of contract, like an out-of-band wait4(p.pid(), 0, 0, nullptr).
   */
  ProcessReturnCode waitAndGetRusage(struct rusage* ru);

  /**
   * Wait for the process to terminate, throw if unsuccessful.
   */
  void waitChecked();

  /**
   * Call `waitpid` non-blockingly up to `timeout`. Throws std::logic_error if
   * called on a Subprocess whose status is not RUNNING.
   *
   * The return code will be running() if waiting timed out.
   */
  ProcessReturnCode waitTimeout(TimeoutDuration timeout);

  /**
   * Send a signal to the child.  Shortcuts for the commonly used Unix
   * signals are below.
   */
  void sendSignal(int signal);
  void terminate() { sendSignal(SIGTERM); }
  void kill() { sendSignal(SIGKILL); }

  /**
   * Call `waitpid` non-blockingly up to `waitTimeout`. If the process hasn't
   * terminated after that, fall back on `terminateOrKill` with
   * `sigtermTimeoutSeconds`.
   */
  ProcessReturnCode waitOrTerminateOrKill(
      TimeoutDuration waitTimeout, TimeoutDuration sigtermTimeout);

  /**
   * Send the SIGTERM to terminate the process, poll `waitpid` non-blockingly
   * several times up to `sigtermTimeout`. If the process hasn't terminated
   * after that, send SIGKILL to kill the process and call `waitpid` blockingly.
   * Return the exit code of process.
   *
   * If sigtermTimeout is 0 or negative, this will immediately send SIGKILL
   * without first sending SIGTERM.
   */
  ProcessReturnCode terminateOrKill(TimeoutDuration sigtermTimeout);

  ////
  //// The methods below only affect the process's communication pipes, but
  //// not its return code or state (they do not poll() or wait()).
  ////

  /**
   * Communicate with the child until all pipes to/from the child are closed.
   *
   * The input buffer is written to the process' stdin pipe, and data is read
   * from the stdout and stderr pipes.  Non-blocking I/O is performed on all
   * pipes simultaneously to avoid deadlocks.
   *
   * The stdin pipe will be closed after the full input buffer has been written.
   * An error will be thrown if a non-empty input buffer is supplied but stdin
   * was not configured as a pipe.
   *
   * Returns a pair of buffers containing the data read from stdout and stderr.
   * If stdout or stderr is not a pipe, an empty IOBuf queue will be returned
   * for the respective buffer.
   *
   * Note that communicate() and communicateIOBuf() both return when all
   * pipes to/from the child are closed; the child might stay alive after
   * that, so you must still wait().
   *
   * communicateIOBuf() uses IOBufQueue for buffering (which has the
   * advantage that it won't try to allocate all data at once), but it does
   * store the subprocess's entire output in memory before returning.
   *
   * communicate() uses strings for simplicity.
   */
  std::pair<IOBufQueue, IOBufQueue> communicateIOBuf(
      IOBufQueue input = IOBufQueue());

  std::pair<std::string, std::string> communicate(std::string_view input = {});

  /**
   * Communicate with the child until all pipes to/from the child are closed.
   *
   * == Semantics ==
   *
   * readCallback(pfd, cfd) will be called whenever there's data available
   * on any pipe *from* the child (PIPE_OUT).  pfd is the file descriptor
   * in the parent (that you use to read from); cfd is the file descriptor
   * in the child (used for identifying the stream; 1 = child's standard
   * output, 2 = child's standard error, etc)
   *
   * writeCallback(pfd, cfd) will be called whenever a pipe *to* the child is
   * writable (PIPE_IN).  pfd is the file descriptor in the parent (that you
   * use to write to); cfd is the file descriptor in the child (used for
   * identifying the stream; 0 = child's standard input, etc)
   *
   * The read and write callbacks must read from / write to pfd and return
   * false during normal operation.  Return true to tell communicate() to
   * close the pipe.  For readCallback, this might send SIGPIPE to the
   * child, or make its writes fail with EPIPE, so you should generally
   * avoid returning true unless you've reached end-of-file.
   *
   * communicate() returns when all pipes to/from the child are closed; the
   * child might stay alive after that, so you must still wait().
   * Conversely, the child may quit long before its pipes are closed, since
   * its descendants can keep them alive forever.
   *
   * Most users won't need to use this callback version; the simpler version
   * of communicate (which buffers data in memory) will probably work fine.
   *
   * == Things you must get correct ==
   *
   * 1) You MUST consume all data passed to readCallback (or return true to
   * close the pipe).  Similarly, you MUST write to a writable pipe (or
   * return true to close the pipe).  To do otherwise is an error that can
   * result in a deadlock.  You must do this even for pipes you are not
   * interested in.
   *
   * 2) pfd is nonblocking, so be prepared for read() / write() to return -1
   * and set errno to EAGAIN (in which case you should return false).  Use
   * readNoInt() from FileUtil.h to handle interrupted reads for you.
   *
   * 3) Your callbacks MUST NOT call any of the Subprocess methods that
   * manipulate the pipe FDs.  Check the docblocks, but, for example,
   * neither closeParentFd (return true instead) nor takeOwnershipOfPipes
   * are safe.  Stick to reading/writing from pfd, as appropriate.
   *
   * == Good to know ==
   *
   * 1) See ReadLinesCallback for an easy way to consume the child's output
   * streams line-by-line (or tokenized by another delimiter).
   *
   * 2) "Wait until the descendants close the pipes" is usually the behavior
   * you want, since the descendants may have something to say even if the
   * immediate child is dead.  If you need to be able to force-close all
   * parent FDs, communicate() will NOT work for you.  Do it your own way by
   * using takeOwnershipOfPipes().
   *
   * Why not? You can return "true" from your callbacks to sever active
   * pipes, but inactive ones can remain open indefinitely.  It is
   * impossible to safely close inactive pipes while another thread is
   * blocked in communicate().  This is BY DESIGN.  Racing communicate()'s
   * read/write callbacks can result in wrong I/O and data corruption.  This
   * class would need internal synchronization and timeouts, a poor and
   * expensive implementation choice, in order to make closeParentFd()
   * thread-safe.
   */
  using FdCallback = folly::Function<bool(int, int)>;
  void communicate(FdCallback readCallback, FdCallback writeCallback);

  /**
   * A readCallback for Subprocess::communicate() that helps you consume
   * lines (or other delimited pieces) from your subprocess's file
   * descriptors.  Use the readLinesCallback() helper to get template
   * deduction.  For example:
   *
   *   subprocess.communicate(
   *     Subprocess::readLinesCallback(
   *       [](int fd, folly::StringPiece s) {
   *         std::cout << fd << " said: " << s;
   *         return false;  // Keep reading from the child
   *       }
   *     ),
   *     [](int pdf, int cfd){ return true; }  // Don't write to the child
   *   );
   *
   * If a file line exceeds maxLineLength, your callback will get some
   * initial chunks of maxLineLength with no trailing delimiters.  The final
   * chunk of a line is delimiter-terminated iff the delimiter was present
   * in the input.  In particular, the last line in a file always lacks a
   * delimiter -- so if a file ends on a delimiter, the final line is empty.
   *
   * Like a regular communicate() callback, your fdLineCb() normally returns
   * false.  It may return true to tell Subprocess to close the underlying
   * file descriptor.  The child process may then receive SIGPIPE or get
   * EPIPE errors on writes.
   */
  template <class Callback>
  class ReadLinesCallback {
   private:
    // Binds an FD to the client-provided FD+line callback
    struct StreamSplitterCallback {
      StreamSplitterCallback(Callback& cb, int fd) : cb_(cb), fd_(fd) {}
      // The return value semantics are inverted vs StreamSplitter
      bool operator()(StringPiece s) { return !cb_(fd_, s); }
      Callback& cb_;
      int fd_;
    };
    using LineSplitter = gen::StreamSplitter<StreamSplitterCallback>;

   public:
    /// Param buf is a caller-provided buffer to use for fd reads. If empty,
    /// this function uses its own stack-allocated scratch buffer. Otherwise, no
    /// stack-allocated scratch buffer is created.
    explicit ReadLinesCallback(
        Callback&& fdLineCb,
        uint64_t maxLineLength = 0, // No line length limit by default
        char delimiter = '\n',
        std::span<char> buf = {})
        : fdLineCb_(std::forward<Callback>(fdLineCb)),
          maxLineLength_(maxLineLength),
          delimiter_(delimiter),
          buf_(buf) {}

    bool operator()(int pfd, int cfd) {
      if (!buf_.empty()) {
        return readAndSplit(pfd, cfd, buf_);
      }
      constexpr size_t kDefaultBufSize = 1024;
      char stackBuf[kDefaultBufSize];
      return readAndSplit(pfd, cfd, std::span<char>(stackBuf, kDefaultBufSize));
    }

   private:
    bool readAndSplit(int pfd, int cfd, std::span<char> buf) {
      // Make a splitter for this cfd if it doesn't already exist
      auto it = fdToSplitter_.find(cfd);
      auto& splitter = (it != fdToSplitter_.end())
          ? it->second
          : fdToSplitter_
                .emplace(
                    cfd,
                    LineSplitter(
                        delimiter_,
                        StreamSplitterCallback(fdLineCb_, cfd),
                        maxLineLength_))
                .first->second;
      // Read as much as we can from this FD
      while (true) {
        ssize_t ret = readNoInt(pfd, buf.data(), buf.size());
        if (ret == -1 && errno == EAGAIN) { // No more data for now
          return false;
        }
        checkUnixError(ret, "read");
        if (ret == 0) { // Reached end-of-file
          splitter.flush(); // Ignore return since the file is over anyway
          return true;
        }
        if (!splitter(StringPiece(buf.data(), ret))) {
          return true; // The callback told us to stop
        }
      }
    }

    Callback fdLineCb_;
    const uint64_t maxLineLength_;
    const char delimiter_;
    const std::span<char> buf_;
    // We lazily make splitters for all cfds that get used.
    std::unordered_map<int, LineSplitter> fdToSplitter_;
  };

  // Helper to enable template deduction
  template <class Callback>
  static auto readLinesCallback(
      Callback&& fdLineCb,
      uint64_t maxLineLength = 0, // No line length limit by default
      char delimiter = '\n',
      std::span<char> buf = {})
      -> ReadLinesCallback<typename std::decay<Callback>::type> {
    return ReadLinesCallback<typename std::decay<Callback>::type>(
        std::forward<Callback>(fdLineCb), maxLineLength, delimiter, buf);
  }

  /**
   * communicate() callbacks can use this to temporarily enable/disable
   * notifications (callbacks) for a pipe to/from the child.  By default,
   * all are enabled.  Useful for "chatty" communication -- you want to
   * disable write callbacks until you receive the expected message.
   *
   * Disabling a pipe does not free you from the requirement to consume all
   * incoming data.  Failing to do so will easily create deadlock bugs.
   *
   * Throws if the childFd is not known.
   */
  void enableNotifications(int childFd, bool enabled);

  /**
   * Are notifications for one pipe to/from child enabled?  Throws if the
   * childFd is not known.
   */
  bool notificationsEnabled(int childFd) const;

  ////
  //// The following methods are meant for the cases when communicate() is
  //// not suitable.  You should not need them when you call communicate(),
  //// and, in fact, it is INHERENTLY UNSAFE to use closeParentFd() or
  //// takeOwnershipOfPipes() from a communicate() callback.
  ////

  /**
   * Close the parent file descriptor given a file descriptor in the child.
   * DO NOT USE from communicate() callbacks; make them return true instead.
   */
  void closeParentFd(int childFd);

  /**
   * Set all pipes from / to child to be non-blocking.  communicate() does
   * this for you.
   */
  void setAllNonBlocking();

  /**
   * Get parent file descriptor corresponding to the given file descriptor
   * in the child.  Throws if childFd isn't a pipe (PIPE_IN / PIPE_OUT).
   * Do not close() the returned file descriptor; use closeParentFd, above.
   */
  int parentFd(int childFd) const {
    return pipes_[findByChildFd(childFd)].pipe.fd();
  }
  int stdinFd() const { return parentFd(0); }
  int stdoutFd() const { return parentFd(1); }
  int stderrFd() const { return parentFd(2); }

  /**
   * The child's pipes are logically separate from the process metadata
   * (they may even be kept alive by the child's descendants).  This call
   * lets you manage the pipes' lifetime separately from the lifetime of the
   * child process.
   *
   * After this call, the Subprocess instance will have no knowledge of
   * these pipes, and the caller assumes responsibility for managing their
   * lifetimes.  Pro-tip: prefer to explicitly close() the pipes, since
   * folly::File would otherwise silently suppress I/O errors.
   *
   * No, you may NOT call this from a communicate() callback.
   */
  struct ChildPipe {
    ChildPipe(int fd, folly::File&& ppe) : childFd(fd), pipe(std::move(ppe)) {}
    int childFd;
    folly::File pipe; // Owns the parent FD
  };
  std::vector<ChildPipe> takeOwnershipOfPipes();

 private:
  struct LibcReal;
  struct SpawnRawArgs;
  struct ChildErrorInfo;
  struct SharedErrorData;

  // spawn() sets up a pipe to read errors from the child,
  // then calls spawnInternal() to do the bulk of the work.  Once
  // spawnInternal() returns it reads the error pipe to see if the child
  // encountered any errors.
  void spawn(
      std::unique_ptr<const char*[]> argv,
      const char* executable,
      const Options& options,
      const std::vector<std::string>* env);
  void spawnInternal(
      std::unique_ptr<const char*[]> argv,
      const char* executable,
      Options& options,
      const std::vector<std::string>* env,
      ChildErrorInfo* err,
      int syncPipeFd = -1);

  static pid_t spawnInternalDoFork(SpawnRawArgs const& args);
#if defined(__linux__)
  // Returns the child pid, or -1 if the caller should use the fork path.
  pid_t spawnInternalTryCloneIntoCGroup(
      SpawnRawArgs& args, Options const& options);
  // Returns the child pid, or a negative errno value. Never throws: the caller
  // decides whether to fall back to spawnInternalDoFork().
  static pid_t spawnInternalDoClone3(SpawnRawArgs const& args, int cgroupFd);
#endif
  [[noreturn]] static void spawnInternalChild(SpawnRawArgs const& args);
  [[noreturn]] static void childError(
      SpawnRawArgs const& args, int errCode, int errnoValue);

  // Actions to run in child.
  // Note that this runs after vfork(), so tread lightly.
  // Returns 0 on success, or an errno value on failure.
  static int prepareChild(SpawnRawArgs const& args);
  static int prepareChildDoOptionalError(int* errout);
  static int prepareChildDoLinuxCGroup(SpawnRawArgs const& args);
  static int runChild(SpawnRawArgs const& args);

  // Closes fds inherited from parent in child process
  static void closeInheritedFds(const SpawnRawArgs& args);

  template <auto Mem>
  static int** erroutOf(Options& o) {
    return (o.*Mem) ? &(o.*Mem)->errout : nullptr;
  }

  using ErroutAccessor = int** (*)(Options&);

  /**
   * Read from the error pipe, and throw SubprocessSpawnError if the child
   * failed before calling exec().
   */
  void readChildErrorNum(ChildErrorInfo err, const char* executable);

  // Returns an index into pipes_. Throws std::invalid_argument if not found.
  size_t findByChildFd(const int childFd) const;

  static constexpr TimeoutDuration::rep DestroyBehaviorFatal = -1;
  static constexpr TimeoutDuration::rep DestroyBehaviorLeak = -2;

  pid_t pid_{-1};
  ProcessReturnCode returnCode_;
  TimeoutDuration::rep destroyBehavior_ = DestroyBehaviorFatal;
#if defined(__linux__)
  bool spawnedIntoLinuxCGroup_{false};
#endif

  /**
   * Represents a pipe between this process, and the child process (or its
   * descendant).  To interact with these pipes, you can use communicate(),
   * or use parentFd() and related methods, or separate them from the
   * Subprocess instance entirely via takeOwnershipOfPipes().
   */
  struct Pipe {
    folly::File pipe; // Our end of the pipe, wrapped in a File to auto-close.
    int childFd = -1; // Identifies the pipe: what FD is this in the child?
    int direction = PIPE_IN; // one of PIPE_IN / PIPE_OUT
    bool enabled = true; // Are notifications enabled in communicate()?

    std::strong_ordering operator<=>(const Pipe& other) const {
      return childFd <=> other.childFd;
    }
    bool operator==(const Pipe& other) const {
      return childFd == other.childFd;
    }
  };

  // Populated at process start according to fdActions, empty after
  // takeOwnershipOfPipes().  Sorted by childFd.  Can only have elements
  // erased, but not inserted, after being populated.
  //
  // The number of pipes between parent and child is assumed to be small,
  // so we're happy with a vector here, even if it means linear erase.
  std::vector<Pipe> pipes_;
};

} // namespace folly

#endif // _WIN32
