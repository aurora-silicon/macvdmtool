/* SPDX-License-Identifier: Apache-2.0 */

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kDefaultSocket = "/var/run/macvdmd.sock";
constexpr const char *kDefaultTool = "/usr/local/libexec/macvdmtool";
constexpr size_t kMaxRequest = 128;
constexpr size_t kMaxOutput = 1024 * 1024;
constexpr int kCommandTimeoutSeconds = 90;

volatile sig_atomic_t g_stop = 0;
std::string g_socket_path;

void on_signal(int)
{
    g_stop = 1;
}

bool send_all(int fd, const void *data, size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

std::vector<std::string> split_request(const std::string &request)
{
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < request.size()) {
        while (pos < request.size() && request[pos] == ' ')
            ++pos;
        if (pos == request.size())
            break;
        size_t end = request.find(' ', pos);
        if (end == std::string::npos)
            end = request.size();
        words.emplace_back(request.substr(pos, end - pos));
        pos = end;
    }
    return words;
}

bool command_allowed(const std::vector<std::string> &args)
{
    if (args.size() == 1) {
        return args[0] == "ping" || args[0] == "reboot" || args[0] == "serial" ||
               args[0] == "debugusb" || args[0] == "dfu" || args[0] == "nop" ||
               args[0] == "help" || args[0] == "-h" || args[0] == "--help";
    }
    return args.size() == 2 && args[0] == "reboot" &&
           (args[1] == "serial" || args[1] == "debugusb");
}

struct CommandResult {
    int status;
    std::string output;
};

CommandResult run_tool(const std::string &tool, const std::vector<std::string> &args)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return {127, std::string("pipe failed: ") + strerror(errno) + "\n"};

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {127, std::string("fork failed: ") + strerror(errno) + "\n"};
    }

    if (child == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(tool.c_str()));
        for (const auto &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execv(tool.c_str(), argv.data());
        dprintf(STDERR_FILENO, "execv failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

    CommandResult result{1, ""};
    bool child_done = false;
    bool pipe_done = false;
    int wait_status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kCommandTimeoutSeconds);

    while (!child_done || !pipe_done) {
        char buffer[4096];
        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            size_t room = kMaxOutput - result.output.size();
            result.output.append(buffer, static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room);
        } else if (n == 0) {
            pipe_done = true;
        } else if (errno != EAGAIN && errno != EINTR) {
            pipe_done = true;
        }

        if (!child_done) {
            pid_t waited = waitpid(child, &wait_status, WNOHANG);
            child_done = waited == child;
        }

        if (!child_done && std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &wait_status, 0);
            child_done = true;
            result.output += "macvdmd: command timed out\n";
        }

        if ((!child_done || !pipe_done) && !g_stop) {
            struct pollfd pfd = {pipefd[0], POLLIN | POLLHUP, 0};
            poll(&pfd, 1, 100);
        }
    }
    close(pipefd[0]);

    if (WIFEXITED(wait_status))
        result.status = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status))
        result.status = 128 + WTERMSIG(wait_status);
    else
        result.status = 1;

    if (result.output.size() == kMaxOutput)
        result.output += "\nmacvdmd: output truncated\n";
    return result;
}

void respond(int fd, int status, const std::string &output)
{
    std::string header = "STATUS " + std::to_string(status) + " " +
                         std::to_string(output.size()) + "\n";
    send_all(fd, header.data(), header.size());
    send_all(fd, output.data(), output.size());
}

void handle_client(int fd, uid_t allowed_uid, const std::string &tool)
{
    uid_t peer_uid = static_cast<uid_t>(-1);
    gid_t peer_gid = static_cast<gid_t>(-1);
    if (getpeereid(fd, &peer_uid, &peer_gid) != 0 ||
        (peer_uid != 0 && peer_uid != allowed_uid)) {
        respond(fd, 126, "macvdmd: unauthorized client\n");
        return;
    }

    std::string request;
    char c;
    while (request.size() <= kMaxRequest) {
        ssize_t n = read(fd, &c, 1);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            respond(fd, 1, "macvdmd: failed to read request\n");
            return;
        }
        if (c == '\n')
            break;
        if (c == '\r' || c == '\0') {
            respond(fd, 2, "macvdmd: invalid request\n");
            return;
        }
        request.push_back(c);
    }

    if (request.size() > kMaxRequest) {
        respond(fd, 2, "macvdmd: request too long\n");
        return;
    }

    auto args = split_request(request);
    if (!command_allowed(args)) {
        respond(fd, 2, "macvdmd: command not allowed\n");
        return;
    }
    if (args[0] == "ping") {
        respond(fd, 0, "macvdmd ready\n");
        return;
    }
    if (args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
        respond(fd, 0,
                "Usage: macvdmctl <command>\n"
                "Commands: ping, reboot [serial|debugusb], serial, debugusb, "
                "dfu, nop, help\n");
        return;
    }

    CommandResult result = run_tool(tool, args);
    respond(fd, result.status, result.output);
}

void usage(const char *program)
{
    fprintf(stderr, "Usage: %s --user USER [--socket PATH] [--tool PATH]\n", program);
}

} // namespace

int main(int argc, char **argv)
{
    const char *user = nullptr;
    std::string tool = kDefaultTool;
    g_socket_path = kDefaultSocket;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--user") == 0 && i + 1 < argc)
            user = argv[++i];
        else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            g_socket_path = argv[++i];
        else if (strcmp(argv[i], "--tool") == 0 && i + 1 < argc)
            tool = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "macvdmd must run as root\n");
        return 1;
    }
    if (!user) {
        usage(argv[0]);
        return 2;
    }

    struct passwd *pw = getpwnam(user);
    if (!pw || pw->pw_uid == 0) {
        fprintf(stderr, "macvdmd: invalid non-root service user\n");
        return 1;
    }

    struct stat tool_stat;
    if (stat(tool.c_str(), &tool_stat) != 0 || tool_stat.st_uid != 0 ||
        (tool_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0 || !S_ISREG(tool_stat.st_mode)) {
        fprintf(stderr, "macvdmd: tool must be a root-owned, non-group-writable regular file\n");
        return 1;
    }

    struct stat socket_stat;
    if (lstat(g_socket_path.c_str(), &socket_stat) == 0) {
        if (!S_ISSOCK(socket_stat.st_mode) ||
            (socket_stat.st_uid != 0 && socket_stat.st_uid != pw->pw_uid)) {
            fprintf(stderr, "macvdmd: refusing to replace unsafe socket path\n");
            return 1;
        }
        if (unlink(g_socket_path.c_str()) != 0) {
            perror("unlink socket");
            return 1;
        }
    } else if (errno != ENOENT) {
        perror("lstat socket");
        return 1;
    }

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (g_socket_path.size() >= sizeof(address.sun_path)) {
        fprintf(stderr, "macvdmd: socket path is too long\n");
        close(server);
        return 1;
    }
    strncpy(address.sun_path, g_socket_path.c_str(), sizeof(address.sun_path) - 1);

    mode_t old_umask = umask(077);
    int bind_result = bind(server, reinterpret_cast<struct sockaddr *>(&address), sizeof(address));
    umask(old_umask);
    if (bind_result != 0) {
        perror("bind");
        close(server);
        return 1;
    }
    if (chown(g_socket_path.c_str(), pw->pw_uid, pw->pw_gid) != 0 ||
        chmod(g_socket_path.c_str(), 0600) != 0) {
        perror("secure socket");
        close(server);
        unlink(g_socket_path.c_str());
        return 1;
    }
    if (listen(server, 4) != 0) {
        perror("listen");
        close(server);
        unlink(g_socket_path.c_str());
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    while (!g_stop) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        handle_client(client, pw->pw_uid, tool);
        close(client);
    }

    close(server);
    unlink(g_socket_path.c_str());
    return 0;
}
