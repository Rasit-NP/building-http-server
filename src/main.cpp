# include <cerrno>
# include <sys/socket.h>
# include <csignal>
# include <cstdlib>
# include <cstdio>
# include <socket.h>
# include <stdexcept>

volatile sig_atomic_t g_should_stop = 0;

void handle_signal(int) {
    g_should_stop = 1;
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

    while (!g_should_stop) {
        auto client = socket.accept();
        if (!client)
            break;

        char buf[1024];
        while (true) {
            ssize_t n = client->read(buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("read");
                break;
            }
            if (!client->write(buf, n)) {
                break;
            }
        }
    }
}