# Socket 리팩토링 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-02
- **작업**: `main.cpp`에 인라인으로 있던 단일 echo 소켓 로직을 **RAII `Socket` 클래스**(`include/socket.h` + `src/socket.cpp`)로 분리하고, `read`/`write` 메서드를 추가
- **결과**: `gcc:12` 컨테이너에서 `-std=c++17 -Wall -Wextra -Wpedantic` 빌드 통과 (경고/에러 0건)

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| 소켓 로직 | `main.cpp`에 `socket()`/`bind()`/`listen()`/`accept()`/`read()`/`write()` 직접 호출 | `Socket` 클래스로 캡슐화 (RAII, move-only) |
| fd 수명 관리 | 수동 `close()` | 소멸자에서 `close()`, move 시 소유권 이전 |
| I/O | 전역 `::read`/`::write` | `Socket::read(buf, len)` / `Socket::write(buf, len)` 메서드 |
| 빌드 대상 | `main.cpp` | `main.cpp` + `socket.cpp` |

### 최종 `Socket` 인터페이스

```cpp
class Socket {
public:
    Socket();                       // socket() + SO_REUSEADDR
    explicit Socket(int fd);        // 기존 fd 래핑 (accept 결과용)
    ~Socket() noexcept;             // close()

    Socket(const Socket&) = delete; // 복사 금지
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept;      // 소유권 이전
    Socket& operator=(Socket&&) noexcept;

    void   bind(uint16_t port);
    void   listen(int backlog = 10);
    Socket accept();                // 클라이언트 연결을 새 Socket으로 반환
    ssize_t read(char* buf, size_t len);
    bool    write(const char* buf, size_t len);
    int     fd() const;

private:
    int fd_ = -1;
};
```

### 설계 원칙

- **bind/listen/accept** = 서버 셋업·연결 수립 단계 → 실패 시 **예외(`throw`)**. 서버가 시작조차 못 하는 치명적 상황이므로 종료가 타당.
- **read/write** = 연결 단위 hot path → 예외 대신 **반환값(`ssize_t`/`bool`)** 으로 결과 전달. 클라이언트 하나의 I/O 에러가 서버 전체를 죽이면 안 되므로, 호출부에서 `break`로 그 연결만 정리.

---

## 2. 트러블슈팅 (발생 순서대로)

### 2-1. 컴파일 에러 — 오타 `unit16_t`
- **증상**: `socket.h`의 `void bind(unit16_t port);`
- **원인**: `uint16_t` 오타
- **해결**: `uint16_t`로 수정

### 2-2. 컴파일 에러 — accessor가 잘못된 멤버 반환
- **증상**: `int fd() const { return fd; }` — 멤버는 `fd_`인데 `fd` 반환
- **해결**: `return fd_;`

### 2-3. 설계 결함 — `accept()`의 책임 과다 + 반환 누락
- **증상**: `accept()`가 `Socket` 반환 선언이지만, 내부에서 echo 루프(read/write)를 돌고 아무것도 반환하지 않음
- **원인**: "연결 수립"과 "데이터 처리" 책임이 한 메서드에 섞임
- **해결**: `accept()`는 `return Socket(client_fd);` 로 **클라이언트 연결만 반환**. echo 루프는 `main`으로 분리. (이미 만들어 둔 `explicit Socket(int fd)` 생성자 활용)

### 2-4. 컴파일 에러 — `Socket`을 `int fd` 자리에 전달
- **증상**: `read(client, buf, ...)` — 전역 `::read`는 첫 인자로 `int`를 받는데 `Socket` 객체를 넘김 (암시적 변환 없음)
- **해결**: `read(client.fd(), ...)` 로 fd를 꺼내 전달. 이후 `client.read(...)` 메서드 호출로 대체

### 2-5. 링커 에러 — `socket.cpp` 미등록
- **증상**: `undefined reference to Socket::...`
- **원인**: `src/CMakeLists.txt`가 `add_executable(http_server main.cpp)` 로 `socket.cpp`를 빌드 대상에 포함 안 함
- **해결**: `add_executable(http_server main.cpp socket.cpp)`
- **교훈**: 새 `.cpp` 추가 시 빌드 스크립트 등록을 잊지 말 것. (헤더만 추가하면 컴파일은 되지만 링크 단계에서 정의 누락으로 터짐)

### 2-6. 런타임/설계 — 연결 단위 에러에 `throw` → 서버 전체 종료
- **증상**: read/write 에러 시 `throw std::runtime_error(...)` 인데 `try/catch`가 어디에도 없음 → 미처리 예외 → `std::terminate()` → 프로세스 사망
- **영향**: 클라이언트 하나가 연결을 험하게 끊기만 해도(RST 등) 서버 전체가 다운 → 사실상 DoS
- **해결 방향**:
  - 연결 단위 에러는 `throw`가 아니라 `break`(해당 연결만 닫고 다음 클라이언트 `accept`)
  - 또는 accept 루프 본문을 `try/catch`로 감싸 한 연결의 예외가 서버를 못 죽이게
  - `n == 0`(정상 EOF)의 `break`는 올바른 동작
- **함정**: 끊긴 소켓에 `write` 시 **SIGPIPE로 즉사** 가능 → `MSG_NOSIGNAL` 또는 `signal(SIGPIPE, SIG_IGN)` 필요 (※ 미적용, 향후 과제)

### 2-7. 치명 버그 — `sizeof(buf)` on `char*` = 포인터 크기
- **증상**: `bool read(char* buf)` 안에서 `::read(fd_, buf, sizeof(buf))`
- **원인**: 매개변수 `buf`는 `char*`라서 `sizeof(buf)`는 1024가 아니라 **포인터 크기(8바이트)**. 배열이 함수 인자로 넘어가며 decay → 크기 정보 소실
- **해결**: 시그니처에 길이 인자 추가 → `read(char* buf, size_t len)`, 내부에서 `len` 사용
- **교훈**: 배열 크기는 호출부만 안다. 포인터를 받는 함수에 `sizeof`로 버퍼 크기를 알아내려 하면 안 됨 → **길이를 인자로 명시 전달**

### 2-8. 치명 버그 — 읽은 바이트 수 유실로 echo 불가
- **증상**: `read`가 `bool`만 반환 → 몇 바이트 읽었는지(`n`) 사라짐 → `write`가 보낼 길이를 모름
- **원인**: echo = "받은 만큼 그대로 전송"인데 그 개수가 전달 안 됨
- **해결**: `read`는 `ssize_t`(읽은 바이트 수, EOF=0, 에러=-1) 반환, `write`는 그 개수를 `len`으로 받아 전송

### 2-9. 버그 — EINTR에 `return true` → 쓰레기 데이터 echo
- **증상**: read 내부에서 `if (errno == EINTR) return true;`
- **원인**: EINTR는 "다시 읽어라"는 신호인데 성공으로 처리 → 호출부가 아직 안 채워진 `buf`를 `write` → 쓰레기/직전 값 echo
- **해결**: EINTR는 **재시도**해야 함. (현재 구현은 `read`를 얇게 두고 `main`의 루프에서 `errno == EINTR` 시 `continue`로 재시도 — 동작상 동일하게 처리)

### 2-10. 컴파일 — `errno`/`EINTR`에 `<cerrno>` 누락
- **해결**: `<cerrno>` 추가 (transitive include에 의존하지 말 것)

### 2-11. 컴파일 에러 — `ssize_t` 미정의 (헤더 self-contained 위반)
- **증상**: `socket.h`가 `<cstdint>`만 include한 채 `ssize_t read(...)` 선언. `socket.cpp`는 `"socket.h"`를 **맨 먼저** include하므로, 시스템 헤더가 들어오기 전에 `ssize_t` 사용 → `unknown type name 'ssize_t'`
- **원인**: `ssize_t`는 표준 C++ 타입이 아니라 **POSIX(`<sys/types.h>`)** 타입. `<cstdint>`에 없음. `main.cpp`는 우연히 `<sys/socket.h>`를 먼저 include해 통과했지만(순서 의존), `socket.cpp`는 그 행운이 없었음
- **해결**: 타입을 쓰는 **헤더(`socket.h`) 자신**이 `<sys/types.h>`를 include. (처음엔 실수로 `socket.cpp`의 socket.h *뒤*에 넣어 효과 없었음 → 헤더로 이동해야 함)
- **교훈**: **헤더는 self-contained** 해야 한다. 자기가 쓰는 모든 타입의 정의 헤더를 직접 include하고, include 순서에 의존하지 말 것

### 2-12. 컴파일 — `perror`에 `<cstdio>` 누락
- **증상**: `socket.cpp`/`main.cpp`가 `perror` 사용
- **해결**: 두 파일에 `<cstdio>` 추가 (`perror`는 `<cstdio>` 선언)

### 2-13. 컴파일 에러(회귀) — `<cstdint>`를 빼버려 `uint16_t` 미정의
- **증상**: 2-11 해결 중 `<cstdint>`를 `<sys/types.h>`로 **교체**(추가가 아니라 대체)함 → `bind(uint16_t port)`의 `uint16_t`가 미정의
- **원인**: `<sys/types.h>`는 `ssize_t`/`size_t`/`int16_t`는 주지만 **C99 `uint16_t`는 안 줌**(glibc는 BSD식 `u_int16_t`만). `uint16_t`의 정의처는 `<cstdint>`/`<stdint.h>`
- **해결**: `<cstdint>` **복원**. 두 헤더를 **모두** 유지:
  - `<cstdint>` → `uint16_t`, `size_t`
  - `<sys/types.h>` → `ssize_t`
- **교훈**: 헤더 정리 시 "교체"가 아니라 각 타입의 정의처를 확인하고 **필요한 것을 모두 유지**

---

## 3. 최종 빌드 검증

```bash
docker run --rm -v "<repo>:/src" -w /src gcc:12 bash -c "\
  g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -c src/socket.cpp -o /tmp/socket.o && \
  g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -c src/main.cpp   -o /tmp/main.o   && \
  g++ /tmp/socket.o /tmp/main.o -o /tmp/http_server"
# => === BUILD OK ===  (경고 0건, 에러 0건)
```

---

## 4. 남은 과제 (현재 빌드/동작에는 지장 없음)

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `writeAll` | `write`가 단일 `::write` 1회 → 부분 전송(partial write) 대비, 전부 보낼 때까지 루프 |
| 🟡 | SIGPIPE | 끊긴 소켓 write 시 프로세스 즉사 방지 → `MSG_NOSIGNAL` / `signal(SIGPIPE, SIG_IGN)` |
| 🟡 | 에러 처리 정책 | accept 루프/연결 루프의 예외가 서버를 죽이지 않도록 일관된 `break`/`try-catch` 정립 |
| ⚪ | 동시성 | 현재 단일 연결만 순차 처리 → 멀티스레드/`epoll`로 동시 접속 (다음 단계) |
| ⚪ | HTTP | echo → 실제 HTTP 요청 파싱/응답 (`readLine`/`readUntil("\r\n")` 등) |

---

## 5. 핵심 교훈 요약

1. **헤더는 self-contained** — 쓰는 타입의 정의 헤더를 직접 include, 순서 의존 금지 (`ssize_t`→`<sys/types.h>`, `uint16_t`→`<cstdint>`).
2. **`sizeof`는 포인터에 쓰지 말 것** — 배열이 함수 인자로 decay되면 크기를 잃는다. 길이는 인자로 전달.
3. **계층별 에러 정책 분리** — 셋업 실패는 예외, 연결 단위 I/O 에러는 반환값+`break`. 미처리 예외 하나가 서버 전체를 죽인다.
4. **책임 분리** — `accept()`는 연결만, I/O는 별도 메서드.
5. **새 `.cpp`는 빌드 스크립트에 등록** — 안 하면 링커 단계에서 `undefined reference`.
