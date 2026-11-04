#include "tts/process_runner.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tts {

    namespace {

        constexpr std::size_t kMaxCapturedStderr = 64U * 1024U;

        void capture(std::string& target, const char* data, const std::size_t size) {
            if (target.size() >= kMaxCapturedStderr) {
                return;
            }
            const std::size_t room = kMaxCapturedStderr - target.size();
            target.append(data, std::min(size, room));
        }

        /// 把文本写入匿名内存文件，作为子进程 stdin；避免管道写满或 SIGPIPE。
        int make_stdin_file(const std::string_view text) {
            const int fd = ::memfd_create("tts-stdin", MFD_CLOEXEC);
            if (fd < 0) {
                return -1;
            }
            std::size_t offset = 0;
            while (offset < text.size()) {
                const ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    ::close(fd);
                    return -1;
                }
                offset += static_cast<std::size_t>(written);
            }
            if (::lseek(fd, 0, SEEK_SET) < 0) {
                ::close(fd);
                return -1;
            }
            return fd;
        }

        void kill_process_group(const pid_t pid) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
        }

        std::string describe_status(const int status) {
            if (WIFSIGNALED(status)) {
                return "terminated by signal " + std::to_string(WTERMSIG(status));
            }
            return "exit code " + std::to_string(WEXITSTATUS(status));
        }

    } // namespace

    std::expected<ProcessOutcome, std::string>
    run_process(const std::vector<std::string>& argv,
                const std::string_view stdin_text,
                const std::atomic_bool* cancel,
                const std::chrono::milliseconds timeout) {
        if (argv.empty()) {
            return std::unexpected("run_process requires a non-empty argv");
        }

        const int stdin_fd = make_stdin_file(stdin_text);
        if (stdin_fd < 0) {
            return std::unexpected(std::string{"cannot prepare stdin: "} + std::strerror(errno));
        }

        int stderr_pipe[2] = {-1, -1};
        if (::pipe2(stderr_pipe, O_CLOEXEC) < 0) {
            const std::string message = std::string{"cannot create stderr pipe: "} + std::strerror(errno);
            ::close(stdin_fd);
            return std::unexpected(message);
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            const std::string message = std::string{"fork failed: "} + std::strerror(errno);
            ::close(stdin_fd);
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);
            return std::unexpected(message);
        }

        if (pid == 0) {
            // 子进程：只调用 async-signal-safe 接口，然后 exec。
            ::setpgid(0, 0);
            ::dup2(stdin_fd, STDIN_FILENO);
            ::dup2(stderr_pipe[1], STDERR_FILENO);

            std::vector<char*> arguments;
            arguments.reserve(argv.size() + 1);
            for (const auto& argument : argv) {
                arguments.push_back(const_cast<char*>(argument.c_str()));
            }
            arguments.push_back(nullptr);
            ::execvp(arguments[0], arguments.data());

            char message[512] = {};
            const int length = std::snprintf(message, sizeof(message), "execvp(%s) failed: %s\n",
                                             argv[0].c_str(), std::strerror(errno));
            if (length > 0) {
                [[maybe_unused]] const auto ignored = ::write(STDERR_FILENO, message,
                                                              static_cast<std::size_t>(length));
            }
            ::_exit(127);
        }

        ::close(stdin_fd);
        ::close(stderr_pipe[1]);

        const int stderr_fd = stderr_pipe[0];
        const int flags = ::fcntl(stderr_fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(stderr_fd, F_SETFL, flags | O_NONBLOCK);
        }

        ProcessOutcome outcome;
        bool exited = false;
        bool stderr_eof = false;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (!exited) {
            if (!stderr_eof) {
                pollfd descriptor{stderr_fd, POLLIN, 0};
                const int ready = ::poll(&descriptor, 1, 10);
                if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    char buffer[4096];
                    const ssize_t count = ::read(stderr_fd, buffer, sizeof(buffer));
                    if (count > 0) {
                        capture(outcome.stderr_text, buffer, static_cast<std::size_t>(count));
                    } else if (count == 0) {
                        stderr_eof = true;
                    } else if (errno != EAGAIN && errno != EINTR) {
                        stderr_eof = true;
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }

            if (cancel != nullptr && cancel->load()) {
                kill_process_group(pid);
                ::waitpid(pid, &status, 0);
                outcome.canceled = true;
                break;
            }
            if (timeout.count() > 0 && std::chrono::steady_clock::now() >= deadline) {
                kill_process_group(pid);
                ::waitpid(pid, &status, 0);
                outcome.timed_out = true;
                break;
            }

            const pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                exited = true;
            } else if (waited < 0 && errno != EINTR) {
                ::close(stderr_fd);
                return std::unexpected(std::string{"waitpid failed: "} + std::strerror(errno));
            }
        }

        // 子进程退出后把 stderr 排空，保证错误信息完整。
        if (!stderr_eof) {
            for (;;) {
                char buffer[4096];
                const ssize_t count = ::read(stderr_fd, buffer, sizeof(buffer));
                if (count > 0) {
                    capture(outcome.stderr_text, buffer, static_cast<std::size_t>(count));
                    continue;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }
        ::close(stderr_fd);

        if (!outcome.canceled && !outcome.timed_out) {
            outcome.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            if (!WIFEXITED(status)) {
                if (!outcome.stderr_text.empty() && outcome.stderr_text.back() != '\n') {
                    outcome.stderr_text.push_back('\n');
                }
                outcome.stderr_text += describe_status(status);
            }
        }
        return outcome;
    }

} // namespace tts
