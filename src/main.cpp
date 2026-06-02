# include <cerrno>
# include <sys/socket.h>
# include <cstdlib>
# include <cstdio>
# include <socket.h>
# include <stdexcept>

int main(){
    Socket socket = Socket();
    socket.bind(8080);
    socket.listen();

    while (true) {
        Socket client = socket.accept();

        char buf[1024];
        while (true) {
            ssize_t n = client.read(buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("read");
                break;
            }
            if (!client.write(buf, n)) {
                break;
            }
        }
    }
}