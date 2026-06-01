#include <cerrno>
# include <sys/socket.h>
# include <netinet/in.h>
# include <unistd.h>
# include <cstring>
# include <cstdio>
# include <cstdlib>
using namespace std;

int main(){
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0){
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0){
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char buf[1024];
        while (true) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("read");
                break;
            }

            ssize_t written = write(client_fd, buf, n);
            if (written< 0) {
                perror("write");
                break;
            }
        }
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}