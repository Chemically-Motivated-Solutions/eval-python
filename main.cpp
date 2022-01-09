#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <pwd.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "args.h"
#include "logging.h"
#include "minijail/scoped_minijail.h"
#include "util.h"

namespace {

constexpr int kLoggingFd = 3;
constexpr int kMetaFd = 4;
constexpr int kSigsysNotificationFd = 5;

const std::map<int, std::string_view> kSignalMap = {
#define ENTRY(x) \
  { x, #x }
    ENTRY(SIGHUP),  ENTRY(SIGINT),    ENTRY(SIGQUIT), ENTRY(SIGILL),
    ENTRY(SIGTRAP), ENTRY(SIGABRT),   ENTRY(SIGBUS),  ENTRY(SIGFPE),
    ENTRY(SIGKILL), ENTRY(SIGUSR1),   ENTRY(SIGSEGV), ENTRY(SIGUSR2),
    ENTRY(SIGPIPE), ENTRY(SIGALRM),   ENTRY(SIGTERM), ENTRY(SIGSTKFLT),
    ENTRY(SIGCHLD), ENTRY(SIGCONT),   ENTRY(SIGSTOP), ENTRY(SIGTSTP),
    ENTRY(SIGTTIN), ENTRY(SIGTTOU),   ENTRY(SIGURG),  ENTRY(SIGXCPU),
    ENTRY(SIGXFSZ), ENTRY(SIGVTALRM), ENTRY(SIGPROF), ENTRY(SIGWINCH),
    ENTRY(SIGIO),   ENTRY(SIGPWR),    ENTRY(SIGSYS)
#undef ENTRY
};

struct InitPayload {
  bool disable_sandboxing = false;
  ScopedMinijail jail;
  std::string comm;
  std::string cgroup_path;
  ssize_t memory_limit_in_bytes;
  size_t vm_memory_size_in_bytes;
  std::vector<ResourceLimit> rlimits;
  struct timespec timeout;
};

extern "C" int pidfd_open(pid_t pid, unsigned int flags) {
#if !defined(__NR_pidfd_open)
  constexpr const int __NR_pidfd_open = 434;
#endif
  return syscall(__NR_pidfd_open, pid, flags);
}

// from minijail/util.h
extern "C" const char* lookup_syscall_name(int nr);

int SetResourceLimits(const std::vector<ResourceLimit>& rlimits) {
  for (const ResourceLimit& rlimit : rlimits) {
    if (prlimit(0, rlimit.resource, &rlimit.rlim, nullptr)) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to set resource limits";
      }
      return -errno;
    }
  }
  return 0;
}

int SetResourceLimits(void* payload) {
  return SetResourceLimits(reinterpret_cast<Args*>(payload)->rlimits);
}

int CloseLoggingFd(void* payload) {
  if (close(kLoggingFd)) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to close the logging fd";
    }
    return -errno;
  }
  return 0;
}

bool MoveToWellKnownFd(struct minijail* j, ScopedFD fd, int well_known_fd) {
  if (fd.get() == well_known_fd) {
    // Leak the FD so the child process can access it.
    fd.release();
  } else {
    if (dup2(fd.get(), well_known_fd) == -1)
      return false;
  }
  int ret = minijail_preserve_fd(j, well_known_fd, well_known_fd);
  if (ret) {
    errno = -ret;
    return false;
  }

  return true;
}

int RemountRootReadOnly(void* payload) {
  if (mount(nullptr, "/", nullptr, MS_RDONLY | MS_REMOUNT | MS_BIND, nullptr)) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to remount root read-only";
    }
    return -errno;
  }
  if (mount(nullptr, "/tmp", nullptr, MS_NODEV | MS_NOSUID | MS_REMOUNT, nullptr)) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to remount tmp as exec-able";
    }
    return -errno;
  }
  return 0;
}

int Chdir(void* payload) {
  const char* dir =
      reinterpret_cast<const char*>(const_cast<const void*>(payload));
  if (chdir(dir)) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to chdir to " << dir;
    }
    return -errno;
  }
  return 0;
}

ScopedFD OpenFile(std::string_view path, bool writable) {
  ScopedFD fd(open(path.data(), O_NOFOLLOW | (writable ? O_WRONLY : O_RDONLY)));
  if (fd || errno != ENXIO)
    return fd;

  // If we got here, it's a muxed stdio socket.
  fd.reset(socket(AF_UNIX, SOCK_SEQPACKET, 0));
  if (!fd)
    return fd;

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.data(), sizeof(addr.sun_path) - 1);
  if (connect(fd.get(), reinterpret_cast<const struct sockaddr*>(&addr),
              sizeof(addr)) == -1) {
    return ScopedFD();
  }
  if (shutdown(fd.get(), writable ? SHUT_RD : SHUT_WR) == -1)
    return ScopedFD();
  return fd;
}

int OpenStdio(std::string_view path, int expected_fd, bool writable) {
  ScopedFD fd = OpenFile(path, writable);
  if (!fd) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to open " << path << " as fd " << expected_fd;
    }
    return -errno;
  }
  if (fd.get() == expected_fd) {
    fd.release();
    return 0;
  }
  if (dup2(fd.get(), expected_fd) == -1) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to dup2 " << path << " as fd " << expected_fd;
    }
    return -errno;
  }
  return 0;
}

int RedirectStdio(void* payload) {
  Args* args = reinterpret_cast<Args*>(payload);
  if (args->disable_sandboxing) {
    if (!args->stdin_redirect.empty()) {
      int ret = OpenStdio(args->stdin_redirect, STDIN_FILENO, false);
      if (ret)
        return ret;
    }
    if (!args->stdout_redirect.empty()) {
      int ret = OpenStdio(args->stdout_redirect, STDOUT_FILENO, true);
      if (ret)
        return ret;
    }
    if (!args->stderr_redirect.empty()) {
      int ret = OpenStdio(args->stderr_redirect, STDERR_FILENO, true);
      if (ret)
        return ret;
      const char* message = "WARNING: Running with --disable-sandboxing\n";
      // This logging is performed in a best-effort basis.
      ignore_result(write(STDERR_FILENO, message, strlen(message)));
    }
    return 0;
  }

  if (!args->stdin_redirect.empty()) {
    int ret = OpenStdio("/mnt/stdio/stdin", STDIN_FILENO, false);
    if (ret)
      return ret;
  }
  if (!args->stdout_redirect.empty()) {
    int ret = OpenStdio("/mnt/stdio/stdout", STDOUT_FILENO, true);
    if (ret)
      return ret;
  }
  if (!args->stderr_redirect.empty()) {
    int ret = OpenStdio("/mnt/stdio/stderr", STDERR_FILENO, true);
    if (ret)
      return ret;
  }
  // Now that the fds are opened in the correct namespace, unmount the parent
  // so that the original paths are not disclosed in /proc/self/mountinfo.
  if (umount2("/mnt/stdio", MNT_DETACH)) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to detach /mnt/stdio";
    }
    return -errno;
  }
  return 0;
}

void InstallStdioRedirectOrDie(struct minijail* j,
                               std::string_view src,
                               std::string_view dest,
                               bool writeable) {
  ScopedFD fd;
  if (writeable) {
    fd.reset(open(src.data(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC, 0644));
  } else {
    fd.reset(open(src.data(), O_RDONLY | O_NOFOLLOW));
  }
  if (!fd && errno != ENXIO)
    PLOG(FATAL) << "Failed to open " << src;
  if (minijail_mount(j, src.data(), dest.data(), "",
                     MS_BIND | (writeable ? 0 : MS_RDONLY))) {
    LOG(FATAL) << "Failed to bind-mount " << src;
  }
}

void TimespecAdd(struct timespec* dst, const struct timespec* src) {
  dst->tv_nsec += src->tv_nsec;
  if (dst->tv_nsec > 1000000000l) {
    dst->tv_nsec -= 1000000000l;
    dst->tv_sec++;
  }
  dst->tv_sec += src->tv_sec;
}

void TimespecSub(struct timespec* dst, const struct timespec* src) {
  dst->tv_nsec -= src->tv_nsec;
  if (dst->tv_nsec < 0) {
    dst->tv_nsec += 1000000000l;
    dst->tv_sec--;
  }
  dst->tv_sec -= src->tv_sec;
}

int TimespecCmp(struct timespec* dst, const struct timespec* src) {
  if (dst->tv_sec < src->tv_sec)
    return -1;
  if (dst->tv_sec > src->tv_sec)
    return 1;
  if (dst->tv_nsec < src->tv_nsec)
    return -1;
  if (dst->tv_nsec > src->tv_nsec)
    return 1;
  return 0;
}

// Receives the exit syscall from the Socket FD.
std::optional<int> ReceiveExitSyscall(ScopedFD sigsys_socket_fd) {
  ScopedFD epoll_fd(epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd) {
    PLOG(ERROR) << "Failed to create epoll fd";
    return std::nullopt;
  }
  if (!AddToEpoll(epoll_fd.get(), sigsys_socket_fd.get())) {
    PLOG(ERROR) << "Failed to add the sigsys socket into epoll";
    return std::nullopt;
  }
  struct epoll_event events[128];
  // TODO: This is handling a small deadlock that's caused by the fact that
  // minijail and this process are both waiting for different things, _and_ if
  // the sandboxed process is killed very early during its lifetime, some
  // leaked FDs will cause those waits to hang forever.
  int nfds = HANDLE_EINTR(
      epoll_wait(epoll_fd.get(), events, array_length(events), 1000));
  if (nfds == -1) {
    PLOG(ERROR) << "Failed to read the exit syscall";
    return std::nullopt;
  }
  if (nfds == 0) {
    LOG(ERROR) << "No file descriptor ready";
    return std::nullopt;
  }
  if (events[0].data.fd != sigsys_socket_fd.get()) {
    LOG(ERROR) << "Unexpected file descriptor was ready";
    return std::nullopt;
  }

  int exitsyscall = 0;
  int read_len = HANDLE_EINTR(recv(sigsys_socket_fd.get(), &exitsyscall,
                                   sizeof(exitsyscall), MSG_DONTWAIT));
  if (read_len < 0) {
    PLOG(ERROR) << "Failed to read the exit syscall";
    return std::nullopt;
  }
  if (read_len == 0) {
    // Nothing to read.
    return std::nullopt;
  }
  if (read_len != sizeof(exitsyscall)) {
    LOG(ERROR) << "Short read";
    return std::nullopt;
  }
  return exitsyscall;
}

int MetaInit(void* raw_payload) {
  InitPayload* payload = reinterpret_cast<InitPayload*>(raw_payload);

  std::unique_ptr<ScopedCgroup> memory_cgroup, unified_cgroup, pid_cgroup;
  if (!payload->cgroup_path.empty()) {
    if (IsCgroupV2()) {
      unified_cgroup = std::make_unique<ScopedCgroup>(payload->cgroup_path);
      if (!*unified_cgroup) {
        {
          ScopedErrnoPreserver preserve_errno;
          PLOG(ERROR) << "Failed to create an omegajail cgroup";
        }
        return -errno;
      }
    } else {
      pid_cgroup = std::make_unique<ScopedCgroup>(payload->cgroup_path);
      if (!*pid_cgroup) {
        {
          ScopedErrnoPreserver preserve_errno;
          PLOG(ERROR) << "Failed to create an omegajail pid cgroup";
        }
        return -errno;
      }
    }
  }

  if (payload->disable_sandboxing) {
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) == -1) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to create an omegajail memory cgroup";
      }
      return -errno;
    }
  } else if (payload->memory_limit_in_bytes >= 0) {
    std::string memory_limit_path;
    if (unified_cgroup) {
      memory_limit_path = PathJoin(unified_cgroup->path(), "memory.max");
    } else {
      memory_cgroup =
          std::make_unique<ScopedCgroup>("/sys/fs/cgroup/memory/omegajail");
      if (!*memory_cgroup) {
        {
          ScopedErrnoPreserver preserve_errno;
          PLOG(ERROR) << "Failed to create an omegajail memory cgroup";
        }
        return -errno;
      }
      memory_limit_path =
          PathJoin(memory_cgroup->path(), "memory.limit_in_bytes");
    }
    if (!WriteFile(memory_limit_path,
                   StringPrintf("%zd", payload->memory_limit_in_bytes))) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to write the cgroup memory limit to "
                    << memory_limit_path;
      }
      return -errno;
    }
    if (chmod(memory_limit_path.c_str(), 0444)) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to make the cgroup memory limit read-only";
      }
      return -errno;
    }
  }

  sigset_t mask;
  sigset_t orig_mask;

  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);

  if (sigprocmask(SIG_BLOCK, &mask, &orig_mask) < 0) {
    {
      ScopedErrnoPreserver preserve_errno;
      PLOG(ERROR) << "Failed to block SIGCHLD";
    }
    return -errno;
  }

  struct timespec t0, t1, t, deadline, timeout;
  clock_gettime(CLOCK_REALTIME, &t0);

  deadline = t0;
  TimespecAdd(&deadline, &payload->timeout);

  pid_t child_pid = fork();
  if (child_pid < 0) {
    _exit(child_pid);
    return -errno;
  }

  if (child_pid == 0) {
    if (payload->disable_sandboxing && setsid() == -1) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to create a new process group";
      }
      return -errno;
    }
    if (!payload->comm.empty())
      prctl(PR_SET_NAME, payload->comm.c_str());
    if (unified_cgroup) {
      std::string procs_path = PathJoin(unified_cgroup->path(), "cgroup.procs");
      if (!WriteFile(procs_path.c_str(), "+2\n", true)) {
        {
          ScopedErrnoPreserver preserve_errno;
          PLOG(ERROR) << "Failed to add the cgroup proc to "
                      << procs_path;
        }
        return -errno;
      }
      unified_cgroup->release();
      if (chmod(procs_path.c_str(), 0444)) {
        {
          ScopedErrnoPreserver preserve_errno;
          PLOG(ERROR) << "Failed to make " << procs_path << " read-only";
        }
        return -errno;
      }
    } else {
      for (auto* cgroup_ptr : {&memory_cgroup, &pid_cgroup}) {
        auto& cgroup = *cgroup_ptr;
        if (!cgroup)
          continue;
        std::string tasks_path = PathJoin(cgroup->path(), "tasks");
        if (!WriteFile(tasks_path.c_str(), "2\n", true)) {
          {
            ScopedErrnoPreserver preserve_errno;
            PLOG(ERROR) << "Failed to write the cgroup task list to "
                        << tasks_path;
          }
          return -errno;
        }
        cgroup->release();
        if (chmod(tasks_path.c_str(), 0444)) {
          {
            ScopedErrnoPreserver preserve_errno;
            PLOG(ERROR) << "Failed to make " << tasks_path << " read-only";
          }
          return -errno;
        }
      }
    }
    if (sigprocmask(SIG_SETMASK, &orig_mask, nullptr) < 0) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to restore signals";
      }
      return -errno;
    }
    if (close(kSigsysNotificationFd) < 0) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to close the sigsys_tracer FD";
      }
      return -errno;
    }
    if (close(kMetaFd) < 0) {
      {
        ScopedErrnoPreserver preserve_errno;
        PLOG(ERROR) << "Failed to close the meta FD";
      }
      return -errno;
    }
    return SetResourceLimits(payload->rlimits);
  }

  // From here on, returns mean nothing. We should try as hard as possible to
  // keep going.

  prctl(PR_SET_NAME, "minijail-init");

  // Send the pidfd of the child process to the sigsys detector.
  ScopedFD sigsys_socket_fd(kSigsysNotificationFd);
  ScopedFD child_pid_fd(pidfd_open(child_pid, 0));
  if (!child_pid_fd) {
    PLOG(ERROR) << "Failed to open pidfd";
  } else if (!SendFD(sigsys_socket_fd.get(), std::move(child_pid_fd))) {
    PLOG(ERROR) << "Failed to write the child pid";
    sigsys_socket_fd.reset();
  }
  shutdown(sigsys_socket_fd.get(), SHUT_WR);

  // Jail this process, too.
  minijail_enter(payload->jail.get());

  pid_t pid;
  bool init_exited = false;
  int status, init_exitstatus = 0;
  int init_exitsyscall = -1;
  int init_exitsignal = -1;
  struct rusage usage = {}, init_usage = {};
  siginfo_t info;
  t = t0;
  bool attached = false;

  do {
    timeout = deadline;
    TimespecSub(&timeout, &t);
    if (HANDLE_EINTR(sigtimedwait(&mask, &info, &timeout)) == -1) {
      clock_gettime(CLOCK_REALTIME, &t);
      break;
    }

    while ((pid = wait3(&status, __WALL | WNOHANG, &usage)) > 0) {
      if (WIFSTOPPED(status)) {
        if (!attached) {
          if (ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                     PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL) == -1) {
            PLOG(ERROR) << "Failed to PTRACE_SETOPTIONS";
          }
          attached = true;
        }
        int stop_signal = WSTOPSIG(status);
        switch (stop_signal) {
          case SIGSYS:
            // For the SIGSYS case we want to get the syscall that caused it.
            if (ptrace(PTRACE_GETSIGINFO, pid, nullptr, &info) == -1)
              PLOG(ERROR) << "Failed to PTRACE_GETSIGINFO";
            init_exitsyscall = info.si_syscall;
            kill(pid, SIGKILL);
            break;

          case SIGXCPU:
          case SIGXFSZ:
            // Signals that are delivered due to exceeding a resource limit will
            // terminate the process.
            init_exitsignal = stop_signal;
            kill(pid, SIGKILL);
            break;

          case SIGSTOP:
          case SIGTRAP:
            // If the signal is SIGSTOP (the one we sent before the process
            // started) or SIGTRAP (a signal injected by ptrace(2)), stop
            // delivery of the signal.
            if (ptrace(PTRACE_CONT, pid, nullptr, 0) == -1)
              PLOG(ERROR) << "Failed to continue process";
            break;

          default:
            // Any other signal will be delivered normally.
            if (ptrace(PTRACE_CONT, pid, nullptr, stop_signal) == -1)
              PLOG(ERROR) << "Failed to continue process";
        }
        continue;
      }

      if (pid == child_pid) {
        init_exitstatus = status;
        init_usage = usage;
        init_exited = true;
      }
    }
    clock_gettime(CLOCK_REALTIME, &t);
  } while (!init_exited && TimespecCmp(&t, &deadline) < 0);

  if (TimespecCmp(&t, &deadline) >= 0)
    init_exitsignal = SIGXCPU;

  kill(payload->disable_sandboxing ? -child_pid : -1, SIGKILL);
  while ((pid = wait3(&status, 0, &usage)) > 0) {
    if (init_exited || pid != child_pid)
      continue;
    init_exitstatus = status;
    init_usage = usage;
    init_exited = true;
  }

  clock_gettime(CLOCK_REALTIME, &t1);
  TimespecSub(&t1, &t0);

  if (sigsys_socket_fd) {
    const std::optional<int> exitsyscall =
        ReceiveExitSyscall(std::move(sigsys_socket_fd));
    if (exitsyscall.has_value()) {
      init_exitsyscall = exitsyscall.value();
    }
  }

  if (memory_cgroup) {
    // When limiting the memory with a cgroup, we need to check if the memory
    // usage was exceeded at the cgroup level. Otherwise, the max RSS might
    // have a significantly lower value and the verdict might not be correct.
    uint64_t failcnt = 0;
    if (ReadUint64(
            StringPrintf("%s/memory.failcnt", memory_cgroup->path().data()),
            &failcnt) &&
        failcnt > 0) {
      init_usage.ru_maxrss = payload->memory_limit_in_bytes;
    }
  }

  memory_cgroup.reset();
  pid_cgroup.reset();

  size_t max_rss = init_usage.ru_maxrss * 1024;
  if (max_rss >= payload->vm_memory_size_in_bytes) {
    max_rss -= payload->vm_memory_size_in_bytes;
  } else {
    max_rss = 0;
  }

  FILE* meta_file = fdopen(kMetaFd, "w");
  fprintf(meta_file, "time:%ld\ntime-sys:%ld\ntime-wall:%ld\nmem:%ld\n",
          1000000 * init_usage.ru_utime.tv_sec + init_usage.ru_utime.tv_usec,
          1000000 * init_usage.ru_stime.tv_sec + init_usage.ru_stime.tv_usec,
          (1000000000L * t1.tv_sec + t1.tv_nsec) / 1000L, max_rss);
  int ret = 0;

  if (init_exitsyscall != -1) {
    const char* syscall_name = lookup_syscall_name(init_exitsyscall);
    if (syscall_name)
      fprintf(meta_file, "signal:SIGSYS\nsyscall:%s\n", syscall_name);
    else
      fprintf(meta_file, "signal:SIGSYS\nsyscall:#%d\n", init_exitsyscall);
    ret = SIGSYS;
  } else if (WIFSIGNALED(init_exitstatus) || init_exitsignal != -1) {
    if (init_exitsignal == -1)
      init_exitsignal = WTERMSIG(init_exitstatus);
    const auto& signal_name = kSignalMap.find(init_exitsignal);
    if (signal_name == kSignalMap.end())
      fprintf(meta_file, "signal_number:%d\n", init_exitsignal);
    else
      fprintf(meta_file, "signal:%s\n", signal_name->second.data());
    ret = init_exitsignal;
  } else if (WIFEXITED(init_exitstatus)) {
    fprintf(meta_file, "status:%d\n", WEXITSTATUS(init_exitstatus));
    ret = WEXITSTATUS(init_exitstatus);
  }
  fclose(meta_file);

  _exit(ret);
}

}  // namespace

int main(int argc, char* argv[]) {
  // We would really like to avoid running as root. If invoked from sudo, the
  // target program will be run as the user invoking sudo.
  bool from_sudo = false;
  char* caller = getenv("SUDO_USER");
  uid_t uid;
  gid_t gid;
  if (caller == nullptr) {
    uid = getuid();
    gid = getgid();
  } else {
    from_sudo = true;
    struct passwd* passwd = getpwnam(caller);
    if (passwd == nullptr)
      LOG(FATAL) << "User " << caller << "not found.";
    uid = passwd->pw_uid;
    gid = passwd->pw_gid;
  }

  if (from_sudo) {
    // Temporarily drop privileges to redirect files.
    if (setegid(gid))
      PLOG(FATAL) << "setegid";
    if (seteuid(uid))
      PLOG(FATAL) << "seteuid";
  }

  // Set a minimalistic environment
  clearenv();
  setenv("HOME", "/home", 1);
  setenv("LANG", "en_US.UTF-8", 1);
  setenv("PATH", "/usr/bin", 1);
  setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);

  // Set the processor affinity mask to a single core. If this process already
  // has an affinity mask set with more than one core set, limit it to the
  // first one in the set.
  // This is effectively a no-op on the runner machines since they are
  // single-core, but this helps avoid some amount of noise on multi-core
  // machines.
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  if (sched_getaffinity(getpid(), sizeof(cpu_set), &cpu_set) == -1) {
    PLOG(ERROR) << "Failed to get the processor affinity";
    return 1;
  }
  if (CPU_COUNT(&cpu_set) > 1) {
    for (int i = 0; i < CPU_SETSIZE; ++i) {
      if (CPU_ISSET(i, &cpu_set)) {
        CPU_ZERO(&cpu_set);
        CPU_SET(i, &cpu_set);
        break;
      }
    }
    if (sched_setaffinity(getpid(), sizeof(cpu_set), &cpu_set) == -1) {
      PLOG(ERROR) << "Failed to setup the processor affinity";
      return 1;
    }
  }

  ScopedMinijail j(minijail_new());

  // Redirect all logging to stderr.
  if (dup2(STDERR_FILENO, kLoggingFd) == -1) {
    PLOG(ERROR) << "Failed to setup the logging fd";
    return 1;
  }
  logging::Init(kLoggingFd, ERROR);
  minijail_log_to_fd(kLoggingFd, LOG_WARNING);
  int ret = minijail_preserve_fd(j.get(), kLoggingFd, kLoggingFd);
  if (ret) {
    LOG(ERROR) << "Failed to set up stderr redirect: " << strerror(-ret);
    return 1;
  }

  Args args;
  if (!args.Parse(argc, argv, j.get()))
    return 1;

  minijail_close_open_fds(j.get());
  if (!args.disable_sandboxing) {
    if (from_sudo) {
      // Change credentials to the original user so this never runs as root.
      minijail_change_uid(j.get(), uid);
      minijail_change_gid(j.get(), gid);
    } else {
      // Enter a user namespace. The current user will be user 1000.
      minijail_namespace_user(j.get());
      minijail_namespace_user_disable_setgroups(j.get());
      constexpr uid_t kTargetUid = 1000;
      constexpr gid_t kTargetGid = 1000;
      minijail_change_uid(j.get(), kTargetUid);
      minijail_change_gid(j.get(), kTargetGid);
      minijail_uidmap(j.get(),
                      StringPrintf("%d %d 1", kTargetUid, uid).c_str());
      minijail_gidmap(j.get(),
                      StringPrintf("%d %d 1", kTargetGid, gid).c_str());
    }

    // Perform some basic setup to tighten security as much as possible by
    // default.
    minijail_mount_tmp(j.get());
    minijail_namespace_cgroups(j.get());
    minijail_namespace_ipc(j.get());
    minijail_namespace_net(j.get());
    minijail_namespace_pids(j.get());
    minijail_namespace_uts(j.get());
    minijail_namespace_set_hostname(j.get(), "omegajail");
    minijail_namespace_vfs(j.get());
    minijail_no_new_privs(j.get());
    minijail_set_ambient_caps(j.get());
    minijail_use_caps(j.get(), 0);
    minijail_reset_signal_mask(j.get());
    minijail_run_as_init(j.get());
    if (minijail_mount(j.get(), "proc", "/proc", "proc",
                       MS_RDONLY | MS_NOEXEC | MS_NODEV | MS_NOSUID)) {
      LOG(ERROR) << "Failed to mount /proc";
      return 1;
    }
    if (minijail_mount_with_data(j.get(), "none", "/mnt/stdio", "tmpfs",
                                 MS_NOEXEC | MS_NODEV | MS_NOSUID,
                                 "size=4096,mode=555")) {
      LOG(ERROR) << "Failed to mount /mnt/stdio";
      return 1;
    }
    if (minijail_add_hook(j.get(), RemountRootReadOnly, nullptr,
                          MINIJAIL_HOOK_EVENT_PRE_DROP_CAPS)) {
      PLOG(ERROR) << "Failed to add a hook to remount / read-only";
      return 1;
    }

    if (!args.stdin_redirect.empty()) {
      InstallStdioRedirectOrDie(j.get(), args.stdin_redirect,
                                "/mnt/stdio/stdin", false);
    }
    if (!args.stdout_redirect.empty()) {
      InstallStdioRedirectOrDie(j.get(), args.stdout_redirect,
                                "/mnt/stdio/stdout", true);
    }
    if (!args.stderr_redirect.empty()) {
      InstallStdioRedirectOrDie(j.get(), args.stderr_redirect,
                                "/mnt/stdio/stderr", true);
    }

    if (args.memory_limit_in_bytes >= 0 &&
        !IsCgroupV2() &&
        minijail_mount(j.get(), "/sys/fs/cgroup/memory/omegajail",
                       "/sys/fs/cgroup/memory/omegajail", "", MS_BIND)) {
      LOG(ERROR) << "Failed to mount /sys/fs/cgroup/memory";
      return 1;
    }
  } else {
    LOG(WARN) << "Running with --disable-sandboxing";
    if (args.stdin_redirect.empty() &&
        minijail_preserve_fd(j.get(), STDIN_FILENO, STDIN_FILENO)) {
      PLOG(ERROR) << "Failed to preserve stdin";
      return 1;
    }
    if (!args.stdout_redirect.empty()) {
      ScopedFD fd(open(args.stdout_redirect.data(),
                       O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC, 0644));
    } else if (minijail_preserve_fd(j.get(), STDOUT_FILENO, STDOUT_FILENO)) {
      PLOG(ERROR) << "Failed to preserve stdout";
      return 1;
    }
    if (!args.stderr_redirect.empty()) {
      ScopedFD fd(open(args.stderr_redirect.data(),
                       O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC, 0644));
    } else if (minijail_preserve_fd(j.get(), STDERR_FILENO, STDERR_FILENO)) {
      PLOG(ERROR) << "Failed to preserve stderr";
      return 1;
    }
  }

  if (!args.chdir.empty()) {
    minijail_add_hook(j.get(), Chdir, const_cast<char*>(args.chdir.c_str()),
                      MINIJAIL_HOOK_EVENT_PRE_DROP_CAPS);
  }

  std::string cgroup_path;
  if (!args.script_basename.empty()) {
    if (IsCgroupV2()) {
      cgroup_path = "/sys/fs/cgroup/omegajail";
      if (access(cgroup_path.c_str(), W_OK) != 0) {
        cgroup_path.clear();
      } else {
        cgroup_path =
            PathJoin("/sys/fs/cgroup/omegajail", args.script_basename);
        if (mkdir(cgroup_path.c_str(), 0775) == 0) {
          const std::string subtree_control_path =
              PathJoin(cgroup_path, "cgroup.subtree_control");
          if (!WriteFile(subtree_control_path, "+memory")) {
            {
              ScopedErrnoPreserver preserve_errno;
              PLOG(ERROR) << "Failed to write the cgroup subtree control "
                          << subtree_control_path;
            }
            return -errno;
          }
        } else if (errno != EEXIST) {
          LOG(ERROR) << "Failed to create " << cgroup_path;
          return 1;
        }
        if (!args.disable_sandboxing &&
            minijail_mount(j.get(), "/sys/fs/cgroup/omegajail",
                           "/sys/fs/cgroup/omegajail", "", MS_BIND)) {
          LOG(ERROR) << "Failed to mount /sys/fs/cgroup/omegajail";
          return 1;
        }
      }
    } else {
      cgroup_path =
          PathJoin("/sys/fs/cgroup/pids/omegajail", args.script_basename);
      if (access(cgroup_path.c_str(), W_OK) != 0) {
        cgroup_path.clear();
      } else if (!args.disable_sandboxing &&
                 minijail_mount(j.get(), "/sys/fs/cgroup/pids/omegajail",
                                "/sys/fs/cgroup/pids/omegajail", "", MS_BIND)) {
        LOG(ERROR) << "Failed to mount /sys/fs/cgroup/pids";
        return 1;
      }
    }
  }

  InitPayload payload{
      .disable_sandboxing = args.disable_sandboxing,
      .comm = args.comm,
      .cgroup_path = cgroup_path,
      .memory_limit_in_bytes = args.memory_limit_in_bytes,
      .vm_memory_size_in_bytes = args.vm_memory_size_in_bytes,
      .rlimits = args.rlimits,
      .timeout =
          {
              .tv_sec = static_cast<time_t>(args.wall_time_limit_msec / 1000),
              .tv_nsec = static_cast<long>((args.wall_time_limit_msec % 1000) *
                                           1000000l),
          },
  };

  ScopedFD sigsys_socket_fd;

  if (!args.meta.empty()) {
    ScopedFD meta_fd(open(args.meta.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!meta_fd) {
      PLOG(ERROR) << "Failed to open meta file " << args.meta;
      return 1;
    }
    if (!MoveToWellKnownFd(j.get(), std::move(meta_fd), kMetaFd)) {
      PLOG(ERROR) << "Failed to dup meta fd";
      return 1;
    }

    int sigsys_socket_fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sigsys_socket_fds)) {
      PLOG(ERROR) << "Failed to open the sigsys pipe";
      return 1;
    }
    if (!MoveToWellKnownFd(j.get(), ScopedFD(sigsys_socket_fds[0]),
                           kSigsysNotificationFd)) {
      PLOG(ERROR) << "Failed to dup sigys notification fd";
      return 1;
    }
    sigsys_socket_fd.reset(sigsys_socket_fds[1]);

    // Setup init's jail
    payload.jail.reset(minijail_new());
    if (from_sudo) {
      minijail_change_uid(payload.jail.get(), uid);
      minijail_change_gid(payload.jail.get(), gid);
    }
    if (!args.disable_sandboxing) {
      minijail_no_new_privs(payload.jail.get());
      minijail_set_ambient_caps(payload.jail.get());
      minijail_use_caps(payload.jail.get(), 0);
    }

    // Run MetaInit() as the container's init.
    ret = minijail_add_hook(j.get(), MetaInit, &payload,
                            MINIJAIL_HOOK_EVENT_PRE_DROP_CAPS);
    if (ret) {
      LOG(ERROR) << "Failed to add hook: " << strerror(-ret);
      return 1;
    }
    minijail_run_as_init(j.get());
  } else {
    minijail_add_hook(j.get(), SetResourceLimits, &args,
                      MINIJAIL_HOOK_EVENT_PRE_DROP_CAPS);
  }

  // This must be the last pre-drop caps hook to be run.
  if (!args.stdin_redirect.empty() || !args.stdout_redirect.empty() ||
      !args.stderr_redirect.empty()) {
    minijail_add_hook(j.get(), RedirectStdio, &args,
                      MINIJAIL_HOOK_EVENT_PRE_DROP_CAPS);
  }

  // This must be added last to ensure that no other hooks are added
  // afterwards.
  minijail_add_hook(j.get(), CloseLoggingFd, nullptr,
                    MINIJAIL_HOOK_EVENT_PRE_EXECVE);

  if (from_sudo) {
    // Become root again to set the jail up.
    if (seteuid(0))
      PLOG(FATAL) << "seteuid";
    if (setegid(0))
      PLOG(FATAL) << "setegid";
  }

  ret = minijail_run_no_preload(
      j.get(), args.program.c_str(),
      const_cast<char* const*>(args.program_args.get()));
  if (ret < 0) {
    LOG(ERROR) << "Failed to run minijail: " << strerror(-ret);
    return 1;
  }

  if (sigsys_socket_fd) {
    ScopedFD user_notification_fd(
        minijail_seccomp_filter_user_notification_fd(j.get()));
    if (!user_notification_fd) {
      LOG(ERROR) << "User notification FD missing";
    } else {
      SigsysPipeThread sigsys_pipe_thread(std::move(sigsys_socket_fd),
                                          std::move(user_notification_fd));
      sigsys_pipe_thread.join();
    }
  }

  return minijail_wait(j.get());
}
