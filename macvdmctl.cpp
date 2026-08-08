/* SPDX-License-Identifier: Apache-2.0 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr const char *kDefaultSocket = "/var/run/macvdmd.sock";
constexpr size_t kMaxHeader = 128;

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

void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s {ping|reboot [serial|debugusb]|serial|debugusb|dfu|nop}\n",
            program);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::string request;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find_first_of(" \r\n\0") != std::string::npos) {
            fprintf(stderr, "macvdmctl: invalid argument\n");
            return 2;
        }
        if (!request.empty())
            request += ' ';
        request += arg;
    }
    request += '\n';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("macvdmctl socket");
        return 1;
    }

    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, kDefaultSocket, sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0) {
        fprintf(stderr, "macvdmctl: service unavailable: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (!send_all(fd, request.data(), request.size())) {
        fprintf(stderr, "macvdmctl: failed to send request\n");
        close(fd);
        return 1;
    }

    std::string header;
    char c;
    while (header.size() < kMaxHeader) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            fprintf(stderr, "macvdmctl: invalid service response\n");
            close(fd);
            return 1;
        }
        if (c == '\n')
            break;
        header.push_back(c);
    }

    int status = 1;
    unsigned long long size = 0;
    if (sscanf(header.c_str(), "STATUS %d %llu", &status, &size) != 2 ||
        size > 2 * 1024 * 1024) {
        fprintf(stderr, "macvdmctl: malformed service response\n");
        close(fd);
        return 1;
    }

    char buffer[4096];
    while (size) {
        size_t wanted = size < sizeof(buffer) ? static_cast<size_t>(size) : sizeof(buffer);
        ssize_t n = read(fd, buffer, wanted);
        if (n <= 0) {
            fprintf(stderr, "macvdmctl: incomplete service response\n");
            close(fd);
            return 1;
        }
        fwrite(buffer, 1, static_cast<size_t>(n), stdout);
        size -= static_cast<unsigned long long>(n);
    }
    close(fd);
    return status >= 0 && status <= 255 ? status : 1;
}
