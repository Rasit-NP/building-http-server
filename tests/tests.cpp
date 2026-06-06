# include <gtest/gtest.h>
# include <fcntl.h>
# include <cerrno>
# include "socket.h"

static bool is_fd_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

TEST(SocketTest, ConstructorCreatesValidFd) {
    Socket socket;
    EXPECT_GE(socket.fd(), 0);
    EXPECT_TRUE(is_fd_open(socket.fd()));
}

TEST(SocketTest, DestructorClosesFd) {
    int saved_fd = -1;
    {
        Socket socket;
        saved_fd = socket.fd();
        ASSERT_TRUE(is_fd_open(saved_fd));
    }
    EXPECT_FALSE(is_fd_open(saved_fd));
}

TEST(SocketTest, MoveConstructorResetsSource) {
    Socket src;
    Socket dst = std::move(src);
    EXPECT_EQ(src.fd(), -1);
}

TEST(SocketTest, MoveAssignmentResetsSource) {
    Socket src;
    Socket dst;
    dst = std::move(src);
    EXPECT_EQ(src.fd(), -1);
}

TEST(SocketTest, MoveTransfersOwnership) {
    int original_fd = -1;
    {
        Socket dst;
        {
            Socket src;
            original_fd = src.fd();
            dst = std::move(src);
            EXPECT_EQ(dst.fd(), original_fd);
            EXPECT_TRUE(is_fd_open(original_fd));
        }
        EXPECT_TRUE(is_fd_open(original_fd));
    }
    EXPECT_FALSE(is_fd_open(original_fd));
}

TEST(SocketTest, BindAndListenSucceed) {
    Socket socket;
    EXPECT_NO_THROW({
        socket.bind(0);
        socket.listen();
    });
}
