# self-pipe wakeup — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-27
- **작업**: self-pipe trick 도입 — 시그널 핸들러에서 self-pipe에 1바이트 write하여 `epoll_wait(-1)`를 즉시 깨우고, check-then-block race를 닫아 graceful shutdown을 견고화
- **결과**: Ubuntu 22.04 컨테이너 빌드 성공(에러 0), 런타임 `SIGTERM` graceful shutdown 검증(0.12s, exit 0) 통과

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| 시그널 처리 | `handle_signal`에서 `g_should_stop = 1` 대입만 | `g_should_stop.store(true, relaxed)` + self-pipe write end에 1바이트 write |
| 시그널 안전성 | atomic 대입 | `errno` 저장/복원 + `memory_order_relaxed` store (async-signal-safe) |
| pipe 설정 | 없음 | `setup_self_pipe()`: `pipe(2)` 생성 + 양단 `O_NONBLOCK` |
| epoll 깨우기 | `epoll_wait`가 `EINTR`로만 깨어남(check-then-block race 취약) | self-pipe read end를 `EPOLLIN`으로 등록, `WAKE_MARKER` 태그로 식별 |
| 이벤트 디스패치 | `data.ptr == nullptr`(listen) / Connection 2분기 | listen / `WAKE_MARKER`(drain) / Connection 3분기 |

**핵심 흐름**
1. `setup_self_pipe()`로 non-blocking pipe 생성 → `sigaction` 등록(순서 중요: pipe가 먼저 준비돼야 핸들러가 write 가능)
2. `EventLoop::register_wake_fd(g_wake_fds[0])`로 read end를 `EPOLLIN`(`data.ptr = WAKE_MARKER`)으로 epoll에 등록
3. `SIGINT`/`SIGTERM` → `handle_signal`이 `g_should_stop=true` 설정 + write end에 `'x'` write
4. read end에 `EPOLLIN` 발생 → `epoll_wait` 즉시 반환 → `WAKE_MARKER` 분기 → `drain_wake_fd()`로 비움
5. 루프 상단 `while (!stop.load())` 재평가 → 종료

---

## 2. 트러블슈팅

### 2-1. `register_wake_fd`에 read end 대신 `epoll_fd`를 전달 — 🔴 빌드/실행 차단(기능)
- **증상**: 빌드·실행은 되지만 self-pipe가 전혀 동작하지 않음. 시그널 발생 시 self-pipe로 `epoll_wait`를 깨우는 경로가 무력화됨.
- **원인**: `main.cpp`에서 `event_loop.register_wake_fd(epoll_fd)`로 호출 — 인자는 self-pipe의 read end(`g_wake_fds[0]`)여야 하는데 `epoll_fd`를 넘김. 내부에서 `::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, epoll_fd, &ev)`가 되어 epoll fd를 자기 자신에 등록 → `EINVAL`로 실패. 반환값 미확인이라 조용히 묻힘. 결과적으로 read end는 epoll에 없고, 핸들러가 write end에 써도 감시 대상이 아니라 깨어나지 않음.
- **해결**: `event_loop.register_wake_fd(g_wake_fds[0])`로 교체(read end 등록).
- **교훈**: self-pipe의 감시 대상은 **read end**다. 그리고 `epoll_ctl` 반환값을 확인하지 않으면 fd 오등록 같은 치명적 실수가 빌드를 통과한 채 런타임에 조용히 묻힌다.

### 2-2. "적용 전/후 종료 지연" 비교 실험이 재현되지 않음 — 🟡 검증 설계
- **증상**: nc로 active 연결을 유지한 뒤 `kill -INT`로 "self-pipe 적용 전에는 다음 입력까지 종료 지연 / 적용 후에는 0 수렴"을 대조하려 했으나, 적용 전에도 즉시 종료되어 차이가 나타나지 않음.
- **원인**: `epoll_wait`는 `SA_RESTART` 설정과 **무관하게** 시그널에 의해 항상 `EINTR`로 반환된다(`man signal(7)`의 재시작되지 않는 syscall 목록). 현재 `run()`은 `if (errno == EINTR) continue;` 후 `while (!stop.load())`를 재평가하므로, self-pipe가 없어도 시그널 즉시 종료된다. 현 코드는 `sa_flags = 0`이기도 하다.
- **해결**: 이번 범위에서 비교 실험은 보류. 의미 있는 대조를 하려면 비교군을 `SA_RESTART` + `EINTR` 재시도(또는 `register_wake_fd` 미호출)로 구성해야 "다음 입력까지 지연"이 재현된다.
- **교훈**: self-pipe trick의 본질은 "대기 중 종료 지연 제거"가 아니라 **check-then-block race 차단**이다. `stop` 확인(①)과 `epoll_wait` 재진입(②) 사이에 도착한 시그널의 `EINTR`는 다음 `epoll_wait`를 깨우지 못하지만, pipe에 남은 바이트는 즉시 `EPOLLIN`을 일으켜 이 틈을 닫는다.

---

## 3. 빌드 · 검증

### 빌드 (Ubuntu 22.04, `builder` 스테이지, `--no-cache`)

```bash
docker build --no-cache --target builder -t http-server-build-fresh .
```

```
[ 17%] Building CXX object src/CMakeFiles/http_server.dir/main.cpp.o
[ 35%] Building CXX object src/CMakeFiles/http_server.dir/event_loop.cpp.o
[ 47%] Built target http_server
[100%] Built target gmock_main
# 컴파일·링크 에러 0, EXIT=0
```

> macOS 호스트 로컬 빌드는 `sys/epoll.h` 부재로 불가(epoll은 Linux 전용). 검증은 Docker(Ubuntu 22.04)에서 수행.

### 런타임 graceful shutdown 검증 (runtime 이미지)

```bash
docker build -t http-server-rt .
cid=$(docker run -d http-server-rt)        # 포트 매핑 없이 기동(시그널 검증엔 불필요)
sleep 1                                      # Running: true 확인
start=$(date +%s.%N); docker stop -t 10 "$cid"; end=$(date +%s.%N)
docker inspect "$cid" --format '{{.State.ExitCode}}'
```

```
running? true
stop took: 0.12s     # SIGTERM(docker stop) → grace 10s 대비 즉시 종료
exit code: 0         # 정상 종료(SIGKILL이면 137)
```

> `epoll_wait` timeout이 `-1`(무한 대기)임에도 `SIGTERM` 후 0.12s 만에 exit 0으로 종료 → 시그널 → write end write → read end `EPOLLIN` → `WAKE_MARKER` drain → `stop` 체크 → 종료의 end-to-end 동작 실증.
>
> 참고: `SIGINT` 단독 실측 및 2-2의 적용 전/후 대조 실험은 수행하지 않음(2-2 사유).

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `epoll_ctl` 반환값 처리 | `register_wake_fd`의 `EPOLL_CTL_ADD` 실패 시 로그/에러 처리 — 2-1 같은 오등록 조기 검출 |
| ⚪ | self-pipe 효과 대조 실험 | 비교군(`SA_RESTART` + `EINTR` 재시도)을 만들어 race/지연 차이를 수치로 입증 (2-2) |
| ⚪ | HTTP 파싱 도입 | 현재는 raw echo. 요청 파싱 → 응답 생성 단계로 확장 |

---

## 5. 핵심 교훈 요약

1. self-pipe의 감시 대상은 **read end**(`g_wake_fds[0]`)다. write end는 핸들러가 쓰는 쪽일 뿐 epoll에 등록하지 않는다.
2. `epoll_ctl` 반환값을 확인하지 않으면 fd 오등록(`EINVAL`)이 빌드를 통과한 채 런타임에 조용히 묻힌다.
3. self-pipe trick의 본질은 "지연 제거"가 아니라 check-then-block **race 차단**이다. `epoll_wait`는 `SA_RESTART`와 무관하게 `EINTR`로 깨어나므로, 단순 종료 지연 비교로는 효과가 드러나지 않는다.
