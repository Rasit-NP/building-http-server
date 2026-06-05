# Graceful Shutdown 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-05
- **작업**: echo 서버에 `SIGINT`/`SIGTERM` 핸들러를 추가해 **graceful shutdown**을 구현. `Socket::accept()` 반환형을 `Socket` → `std::optional<Socket>` 로 바꿔 `accept()`가 `EINTR`(시그널로 중단)일 때 예외 대신 `std::nullopt` 로 정상 탈출하도록 변경
- **결과**: `gcc:12`(GCC 12.5.0) 및 macOS 로컬(CLion 번들 cmake) 모두 `-std=c++17 -Wall -Wextra -Wpedantic` 빌드 통과(경고/에러 0건). 런타임상 **idle 상태 종료는 정상 동작**하나, **활성 연결 중 종료는 미보장**임을 측정으로 확인 (§2-4)

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| `accept()` 반환형 | `Socket` | `std::optional<Socket>` |
| `accept()` 의 `EINTR` | `throw std::runtime_error("accept() failed")` | `errno == EINTR` 이면 `return std::nullopt` |
| 종료 방식 | `while (true)` 무한 루프 (Ctrl+C 시 강제 종료) | `volatile sig_atomic_t g_should_stop` 플래그 + `while (!g_should_stop)` |
| 시그널 처리 | 없음 (기본 동작 = 프로세스 즉시 종료) | `sigaction` 으로 `SIGINT`/`SIGTERM` 핸들러 등록 |
| 호출부 | `Socket client = socket.accept();` | `auto client = socket.accept(); if (!client) break;` (`client->read/write`) |

### 핵심 설계 의도

- 서버가 `accept()`에서 새 연결을 기다리며 블로킹 중일 때, `SIGINT`/`SIGTERM` 이 오면:
  1. 핸들러가 `g_should_stop = 1` 설정
  2. 블로킹 중이던 `accept()`가 `EINTR` 로 깨어나 `std::nullopt` 반환
  3. `main`의 `if (!client) break;` 로 루프 탈출 → `Socket` 소멸자(RAII)가 listen fd 정리 → `return 0`
- `sigaction` 의 `sa_flags = 0` (의도적으로 `SA_RESTART` 미설정) → 시스템 콜이 자동 재시작되지 않고 `EINTR` 로 반환되게 함.

---

## 2. 트러블슈팅 (발생 순서대로)

### 2-1. 검증 환경 — `cmake` 가 PATH에 없음 ⚪
- **증상**: `cmake -S . -B ...` 실행 시 `command not found: cmake`
- **원인**: 시스템에 standalone `cmake` 미설치. CLion 번들 cmake만 존재
- **해결**: `/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin` (및 번들 `ninja`)를 `PATH` 앞에 추가해 빌드 수행
- **교훈**: 로컬 검증 시 IDE 번들 toolchain 경로를 활용할 수 있음. (CI는 별도로 ubuntu + 시스템 cmake 사용)

### 2-2. 검증 오염 — 포트 8080을 Docker 컨테이너가 선점 🔴
- **증상**: 로컬 빌드 바이너리를 실행하자마자 `terminating due to uncaught exception of type std::runtime_error: bind() failed` 로 abort(exit 134). 그런데도 `nc` echo 응답은 정상적으로 돌아옴
- **원인**: `docker-compose.yml` 로 띄운 `building-http-server-server-1` 컨테이너가 이미 `0.0.0.0:8080` 을 LISTEN 중. `lsof -nP -iTCP:8080 -sTCP:LISTEN` 결과 `com.docke`(Docker) 점유 확인. echo 응답은 우리 바이너리가 아니라 **그 컨테이너**가 준 것이었고, exit 134는 graceful shutdown이 아니라 bind 실패 abort였음
- **해결**: 런타임 검증 전에 `docker compose stop server` 로 포트를 비우고, 검증 후 `docker compose start server` 로 원복
- **교훈**: **검증 대상이 실제로 우리가 만든 프로세스인지 먼저 확인**할 것. 포트 충돌 시 엉뚱한 프로세스의 응답을 우리 코드의 동작으로 오인할 수 있다. (`lsof -iTCP:<port>` + 우리 pid 대조)

### 2-3. graceful shutdown — idle 상태 `SIGINT`/`SIGTERM` 정상 동작 ✅ (검증 통과)
- **증상(기대 동작)**: 서버가 `accept()`에서 대기 중일 때 `SIGINT`/`SIGTERM` 으로 깔끔히 종료되어야 함
- **검증**: 포트를 비운 뒤 서버 실행 → `kill -INT`/`kill -TERM` 전송
- **결과**: 두 시그널 모두 **exit code 0** 으로 정상 종료(abort 아님). 종료 후 `lsof -iTCP:8080` 에 잔여 소켓 없음, `pgrep http_server` 잔여 프로세스 없음 → `accept()`의 `EINTR → nullopt` 분기와 RAII fd 정리가 의도대로 동작함을 확인

### 2-4. graceful shutdown — **활성 연결 중에는 즉시 종료 안 됨** 🟡 (설계 한계)
- **증상**: 클라이언트 연결이 열려 있어 서버가 `client->read()` 에서 블로킹 중일 때 `SIGINT` 를 보내면, **3초+ 동안 종료되지 않다가** 연결이 닫힌 뒤에야 종료됨. 측정값 **3.27초**(= 의도적으로 3초 폴링 후 연결을 닫자 종료). 서버 stderr에 `read` 관련 출력(perror)도 전혀 없었음
- **원인**: `EINTR → std::nullopt` 처리가 **`accept()` 에만** 존재하고, `main.cpp` 의 inner echo 루프(`client->read()`)에는 (a) `g_should_stop` 검사도, (b) `read()`의 `EINTR` 처리도 없음. 즉 graceful shutdown은 **"새 연결 대기(`accept` 블로킹)" 상태에서만 보장**되고, **"기존 연결 서비스(`read` 블로킹)" 중**은 커버하지 못함. 시그널 핸들러가 `g_should_stop=1` 을 세팅해도, 다음 번 `accept()` 진입 전까지는 그 플래그가 평가되지 않음
- **해결**: 현재는 단일 연결 순차 처리 echo 서버라 **⚪ 향후 과제**로 분류(아래 §3 남은 과제). 멀티 클라이언트로 확장 시 inner 루프에도 종료 신호 처리(예: `read()` 의 `EINTR` 시 `g_should_stop` 검사 후 break, 또는 `epoll`+self-pipe/`signalfd` 로 시그널과 I/O를 단일 이벤트 루프에서 처리)를 넣어야 함
- **교훈**: 시그널 기반 graceful shutdown은 **"블로킹할 수 있는 모든 지점"** 에서 종료 신호를 평가해야 완전하다. 한 곳(`accept`)만 처리하면 다른 블로킹 지점(`read`)에서 신호가 묻힌다

### 2-5. 검증 함정 — zsh는 `/dev/tcp` 미지원 ⚪
- **증상**: 활성 연결을 만들려고 `( exec 3<>/dev/tcp/127.0.0.1/8080; ... )` 사용 → `no such file or directory: /dev/tcp/...`. 연결이 실제로 안 열렸는데도 서버가 종료되어 "정상"으로 오판할 뻔함
- **원인**: `/dev/tcp` 의사 장치는 **bash 전용 기능**. zsh에는 없음. 결국 서버는 여전히 idle(`accept` 블로킹) 상태였고 §2-3 과 같은 경로로 종료된 것
- **해결**: `nc` + named pipe(`mkfifo`)로 연결을 실제로 열어 유지한 뒤 재검증 → §2-4 의 정확한 측정 확보
- **교훈**: 셸 의존 기능(`/dev/tcp`)은 인터프리터에 따라 조용히 실패할 수 있다. 검증은 **연결이 실제로 수립됐는지**(`lsof ... ESTABLISHED` 카운트)까지 확인해야 신뢰할 수 있다

---

## 3. 빌드 · 검증 (실제 실행)

### 3-1. 빌드 — gcc:12 컨테이너 (CI 환경 기준)

```bash
docker run --rm -v "$PWD:/src" -w /src gcc:12 bash -c '\
  g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -c src/socket.cpp -o /tmp/socket.o && \
  g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -c src/main.cpp   -o /tmp/main.o   && \
  g++ /tmp/socket.o /tmp/main.o -o /tmp/http_server && echo "=== BUILD OK ==="'
```

```
=== BUILD OK ===
g++ (GCC) 12.5.0
# 경고 0건, 에러 0건
```

### 3-2. 빌드 — macOS 로컬 (CLion 번들 cmake/ninja, AppleClang)

```bash
export PATH="/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin:\
/Applications/CLion.app/Contents/bin/ninja/mac/aarch64:$PATH"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release      # CONFIG OK
cmake --build build --target http_server -j         # 경고 0건
```

```
[ 80%] Building CXX object src/CMakeFiles/http_server.dir/main.cpp.o
[ 80%] Building CXX object src/CMakeFiles/http_server.dir/socket.cpp.o
[100%] Linking CXX executable http_server
```

### 3-3. 런타임 검증 (포트 8080을 Docker에서 회수한 뒤)

```bash
docker compose stop server        # 8080 비우기 (검증 후 start로 원복)
./build/src/http_server &
```

| # | 검증 | 명령(요지) | 결과 |
|---|------|-----------|------|
| 1 | LISTEN 확인 | `lsof -iTCP:8080 -sTCP:LISTEN \| grep <pid>` | ✅ 우리 pid가 8080 LISTEN |
| 2 | echo 동작 | `printf 'hello-echo-123' \| nc -w1 127.0.0.1 8080` | ✅ `hello-echo-123` 그대로 반환 |
| 3 | idle `SIGINT` | `kill -INT <pid>` | ✅ **exit 0** (abort 아님) |
| 4 | idle `SIGTERM` | `kill -TERM <pid>` | ✅ **exit 0** |
| 5 | 종료 후 정리 | `lsof -iTCP:8080`, `pgrep http_server` | ✅ 소켓·프로세스 잔여 없음 (RAII) |
| 6 | **활성 연결 중 `SIGINT`** | `nc`로 연결 유지 후 `kill -INT` | 🟡 3초+ 미종료, 연결 닫힌 뒤 종료(측정 **3.27s**), perror 출력 없음 → §2-4 |

```bash
docker compose start server       # 컨테이너 원복 (8080 재점유 확인 완료)
```

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | 활성 연결 중 종료 | inner `read()` 루프에도 종료 신호 처리. `read()`의 `EINTR` 시 `g_should_stop` 검사 후 break, 또는 진행 중 연결을 graceful drain |
| ⚪ | 이벤트 루프화 | `epoll`/`kqueue` + `signalfd`(또는 self-pipe trick)로 시그널과 소켓 I/O를 단일 루프에서 처리 → 모든 블로킹 지점에서 종료 신호 일관 평가 |
| ⚪ | 종료 코드 정책 | 현재 정상 종료 exit 0. 시그널별 종료 코드/로그(예: "shutting down (SIGTERM)") 정립 |

---

## 5. 핵심 교훈 요약

1. **graceful shutdown은 모든 블로킹 지점에서 종료 신호를 평가해야 완전하다.** `accept()` 한 곳만 `EINTR` 처리하면, `read()` 블로킹 중에는 신호가 묻힌다 (§2-4).
2. **검증 대상이 진짜 우리 프로세스인지 먼저 확인하라.** 포트를 다른 프로세스(Docker)가 선점하면 엉뚱한 응답을 우리 코드의 동작으로 오인한다 (§2-2).
3. **`sigaction(sa_flags=0)` = `SA_RESTART` 미설정** → 시스템 콜이 `EINTR` 로 반환되어야 시그널 기반 탈출이 성립한다.
4. **셸 의존 검증 기법은 조용히 실패할 수 있다.** `/dev/tcp` 는 bash 전용 → zsh에서 미동작. 연결 수립 여부(`ESTABLISHED`)까지 확인해야 신뢰 가능 (§2-5).
5. **`std::optional<Socket>` 반환**은 "정상이지만 결과 없음"(시그널로 중단된 accept)을 예외 없이 표현하는 적절한 도구.
