#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────
// 서버 수명 관리
// ─────────────────────────────────────────────────────────────

struct ServerHandle {
    pid_t    pid  = -1;
    uint16_t port = 0;
};

// 서버를 fork+execv로 띄우고, stdout 파이프에서 "PORT <n>"을 읽어 포트를 얻는다.
// 실패 시 pid==-1 반환.
static ServerHandle start_server() {
    int pipefd[2];
    if (::pipe(pipefd) < 0) {
        perror("pipe");
        return {};
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        perror("fork");
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }

    if (pid == 0) {
        // 자식: stdout을 파이프 write end로 연결한 뒤 서버 실행
        ::close(pipefd[0]);                    // read end 불필요
        if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(127);
        }
        ::close(pipefd[1]);                    // dup2 후 원본 불필요

        // 포트 0 → 서버가 OS 할당 포트를 bind하고 PORT로 출력
        char* const argv[] = { const_cast<char*>(SERVER_BINARY_PATH),
                               const_cast<char*>("0"),
                               nullptr };
        ::execv(SERVER_BINARY_PATH, argv);
        perror("execv");                       // execv 성공 시 여기 도달 안 함
        _exit(127);
    }

    // 부모: write end 닫고 read end에서 "PORT <n>\n" 파싱
    ::close(pipefd[1]);

    std::string line;
    char        c;
    bool        got_newline = false;
    while (true) {
        ssize_t r = ::read(pipefd[0], &c, 1);
        if (r > 0) {
            if (c == '\n') { got_newline = true; break; }
            line.push_back(c);
            if (line.size() > 64) break;       // 폭주 방지
        } else if (r == 0) {
            break;                             // EOF: 서버가 출력 전에 죽음
        } else {
            if (errno == EINTR) continue;
            perror("read(port)");
            break;
        }
    }
    ::close(pipefd[0]);

    ServerHandle h;
    h.pid = pid;
    if (got_newline) {
        unsigned int p = 0;
        if (std::sscanf(line.c_str(), "PORT %u", &p) == 1) {
            h.port = static_cast<uint16_t>(p);
        }
    }

    if (h.port == 0) {
        std::fprintf(stderr, "failed to read port (line=\"%s\")\n", line.c_str());
        // 서버는 떠 있을 수 있으니 정리
        ::kill(pid, SIGKILL);
        int st; ::waitpid(pid, &st, 0);
        h.pid = -1;
    }
    return h;
}

// SIGTERM → waitpid. 종료까지 걸린 시간(초)을 out_elapsed에 기록(원하면 nullptr).
static void stop_server(pid_t pid, double* out_elapsed = nullptr) {
    if (pid <= 0) return;

    struct timespec t0{}, t1{};
    ::clock_gettime(CLOCK_MONOTONIC, &t0);

    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);

    ::clock_gettime(CLOCK_MONOTONIC, &t1);
    if (out_elapsed) {
        *out_elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    }
}

// ─────────────────────────────────────────────────────────────
// 소켓 헬퍼
// ─────────────────────────────────────────────────────────────

// 서버가 listen할 때까지 connect 재시도. 성공 시 fd, 실패 시 -1.
static int connect_with_retry(uint16_t port, int max_retries = 50) {
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);

        // ECONNREFUSED 등: 아직 listen 전. 짧게 대기 후 재시도.
        struct timespec ts{ 0, 10 * 1000 * 1000 };  // 10ms
        ::nanosleep(&ts, nullptr);
    }
    return -1;
}

static bool send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool recv_exact(int fd, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n > 0) { got += static_cast<size_t>(n); continue; }
        if (n == 0) return false;              // peer가 일찍 닫음
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

// 한 연결에서 payload를 보내고 같은 바이트가 돌아오는지 검증.
static bool echo_roundtrip(uint16_t port, const std::string& payload) {
    int fd = connect_with_retry(port);
    if (fd < 0) return false;

    bool ok = send_all(fd, payload.data(), payload.size());
    if (ok) {
        std::string back(payload.size(), '\0');
        ok = recv_exact(fd, back.data(), back.size()) && (back == payload);
    }
    ::close(fd);
    return ok;
}

// ─────────────────────────────────────────────────────────────
// idle CPU 측정 (Linux 전용: /proc/<pid>/stat의 utime+stime)
// ─────────────────────────────────────────────────────────────

// utime(14번째)+stime(15번째) 필드 합을 clock tick 단위로 반환. 실패 시 -1.
static long read_proc_cpu_ticks(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;

    // comm 필드가 괄호+공백을 포함할 수 있으므로 ')'를 기준으로 자른다.
    char   buf[4096];
    size_t r = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (r == 0) return -1;
    buf[r] = '\0';

    char* after = std::strrchr(buf, ')');
    if (!after) return -1;
    after += 1;  // ") S ppid ..." — ')' 다음부터 필드 4번(state)이 시작

    // after를 공백으로 토큰화: index 0 = state, ... utime은 stat 전체 14번째 필드.
    // ')' 이후 첫 토큰이 state(field 3), 그러므로 utime은 ')' 이후 12번째 토큰,
    // stime은 13번째 토큰 (1-based로 세어 맞춘다).
    long   utime = -1, stime = -1;
    int    idx   = 0;
    char*  tok   = std::strtok(after, " ");
    while (tok) {
        idx += 1;            // field 3(state)=idx1, field4(ppid)=idx2, ...
        if (idx == 12) utime = std::strtol(tok, nullptr, 10);  // field 14
        if (idx == 13) { stime = std::strtol(tok, nullptr, 10); break; }  // field 15
        tok = std::strtok(nullptr, " ");
    }
    if (utime < 0 || stime < 0) return -1;
    return utime + stime;
}

// ─────────────────────────────────────────────────────────────
// 시나리오
// ─────────────────────────────────────────────────────────────

static int scenario_single_echo() {
    ServerHandle s = start_server();
    if (s.pid < 0) return 1;
    bool ok = echo_roundtrip(s.port, "hello-echo-123");
    stop_server(s.pid);
    return ok ? 0 : 1;
}

static int scenario_concurrent_echo() {
    ServerHandle s = start_server();
    if (s.pid < 0) return 1;

    constexpr int     N = 8;
    std::vector<bool> results(N, false);
    std::vector<std::thread> ths;
    for (int i = 0; i < N; ++i) {
        ths.emplace_back([&, i] {
            // 연결마다 고유 payload → 연결 간 데이터 섞임 검출
            std::string payload = "conn-" + std::to_string(i) + "-" + std::string(200, 'a' + (i % 26));
            results[i] = echo_roundtrip(s.port, payload);
        });
    }
    for (auto& t : ths) t.join();

    stop_server(s.pid);
    for (bool r : results) if (!r) return 1;
    return 0;
}

static int scenario_large_payload() {
    ServerHandle s = start_server();
    if (s.pid < 0) return 1;

    // send buffer 초과 크기 → 서버의 EPOLLOUT 경로 강제
    std::string payload(4 * 1024 * 1024, 'Z');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>('A' + (i % 26));

    bool ok = echo_roundtrip(s.port, payload);
    stop_server(s.pid);
    return ok ? 0 : 1;
}

static int scenario_idle_cpu() {
    ServerHandle s = start_server();
    if (s.pid < 0) return 1;

    // 연결 0인 상태로 일정 구간 동안 CPU tick 델타 측정.
    long before = read_proc_cpu_ticks(s.pid);
    if (before < 0) { stop_server(s.pid); return 77; }  // /proc 없음(비-Linux) → skip 의미의 코드

    struct timespec ts{ 2, 0 };  // 2초 idle
    ::nanosleep(&ts, nullptr);

    long after = read_proc_cpu_ticks(s.pid);
    if (after < 0) { stop_server(s.pid); return 1; }
    stop_server(s.pid);

    long ticks_per_sec = ::sysconf(_SC_CLK_TCK);   // 보통 100
    double cpu_seconds = static_cast<double>(after - before) / ticks_per_sec;

    // 2초 idle 동안 CPU 사용이 임계값 미만이어야 함(busy-spin이면 ~2초 누적).
    // 여유롭게 0.2초(=10%) 미만으로 판정.
    std::fprintf(stderr, "idle cpu_seconds=%.3f over 2s\n", cpu_seconds);
    return (cpu_seconds < 0.2) ? 0 : 1;
}

static int scenario_signal_shutdown() {
    ServerHandle s = start_server();
    if (s.pid < 0) return 1;

    // active 연결을 열어둔 채 SIGTERM → 즉시 종료되는지 시간 측정.
    int fd = connect_with_retry(s.port);
    if (fd < 0) { stop_server(s.pid); return 1; }
    // 연결 유지(데이터는 보내도 안 보내도 됨). 여기선 한 번 왕복만.
    send_all(fd, "alive", 5);
    char tmp[5];
    recv_exact(fd, tmp, 5);

    double elapsed = 0.0;
    stop_server(s.pid, &elapsed);   // 연결 열린 상태로 SIGTERM 전송
    ::close(fd);

    std::fprintf(stderr, "shutdown elapsed=%.3fs\n", elapsed);
    // 2주차 3.27초 한계가 해소됐으므로 1초 이내 종료여야 함(CI 여유 포함).
    return (elapsed < 1.0) ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 2;
    }
    std::string sc = argv[1];

    if (sc == "single_echo")      return scenario_single_echo();
    if (sc == "concurrent_echo")  return scenario_concurrent_echo();
    if (sc == "large_payload")    return scenario_large_payload();
    if (sc == "idle_cpu")         return scenario_idle_cpu();
    if (sc == "signal_shutdown")  return scenario_signal_shutdown();

    std::fprintf(stderr, "unknown scenario: %s\n", sc.c_str());
    return 2;
}