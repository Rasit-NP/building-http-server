# include <atomic>
# include <cerrno>
# include <sys/socket.h>
# include <csignal>
# include <cstdlib>
# include <cstdio>
# include <socket.h>
# include <stdexcept>
#include <unistd.h>

std::atomic<bool> g_should_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void handle_signal(int) {
    g_should_stop = 1;
}

void handle_client(Socket& client) {
    char buf[1024];
    while (true) {
        ssize_t n = client.read(buf, sizeof(buf));
        if (n > 0) {
            client.write(buf, n);
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK)    continue;
        if (errno == EINTR)                             continue;
        perror("read");
        break;
    }
}

int main(){

    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Socket socket = Socket();
    socket.bind(8080);
    socket.listen();
    socket.set_nonblocking();

    while (!g_should_stop.load(std::memory_order_relaxed)) {
        std::optional<Socket> client = socket.accept();

        if (client == std::nullopt) {
            continue;
        }
        client->set_nonblocking();
        handle_client(*client);
    }
}