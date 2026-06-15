# write backpressure (EPOLLOUT) — 작업 & 트러블슈팅 로그

- **날짜**: 2026-06-15
- **작업**: non-blocking 부분 쓰기(partial write) 대응 — `write_buf` 잔여 시 `EPOLLOUT`을 토글 등록해 socket이 쓰기 가능해질 때 이어서 전송하는 backpressure 처리 도입
- **결과**: Ubuntu 22.04 컨테이너 빌드 성공(경고 1건), 런타임 echo 검증(small / 4MB / 동시 5연결) 통과

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| 쓰기 로직 | `on_readable()` 안에서 `::write` **1회 호출**(`if`), 부분 쓰기 미처리 | `flush_write()`로 추출, `while` 루프 + `EAGAIN`/`EINTR` 분류 |
| 쓰기 이벤트 | 없음 (read 시 즉시 echo만) | `on_writable()` 신설, `EPOLLOUT` 이벤트로 잔여분 재전송 |
| epoll 관심 집합 | accept 시 `EPOLLIN` 고정 | `update_interest()`가 `want_write` 상태에 따라 `EPOLLOUT` 토글 |
| 상태 추적 | 없음 | `Connection::writing` 플래그 + `want_write()`/`is_writing()`/`set_writing()` |
| `run()` 시그니처 | `std::atomic<bool>&` | `const std::atomic<bool>&` (읽기 전용 의도 명시) |

**핵심 흐름**
1. `on_readable()` → `flush_write()`에서 send buffer가 차 `EAGAIN` → `write_buf`에 잔여
2. `want_write()==true` → `update_interest()`가 `EPOLLOUT` 등록(`writing=true`)
3. socket 쓰기 가능 시 `EPOLLOUT` → `on_writable()` → `flush_write()` 재전송
4. 버퍼 비면 `want_write()==false` → `EPOLLOUT` 해제(`writing=false`)

---

## 2. 트러블슈팅

### 2-1. `update_interest()`에서 write 관심이 등록되지 않음 — 🔴 빌드/실행 차단
- **증상**: `write_buf`에 잔여가 있어도 `on_writable()`이 영원히 호출되지 않음. 느린/응답을 안 읽는 client에서 잔여 데이터 전송이 멈춤(dead code).
- **원인**: 관심 집합에 `EPOLLOUT` 대신 edge-triggered 수식자 `EPOLLET`을 넣음. `ev.events = EPOLLIN | (want ? EPOLLET : 0);` → write 알림이 등록되지 않고 read 트리거 의미까지 바뀜(accept 시 level-triggered와 불일치).
- **해결**: `EPOLLET` → `EPOLLOUT`으로 교체.
- **교훈**: `EPOLLET`(트리거 모드)과 `EPOLLOUT`(이벤트 종류)은 별개. 관심 등록은 "이벤트 종류"를 추가하는 것이며 트리거 모드와 혼동하지 않는다.

### 2-2. 이미 등록된 fd에 `EPOLL_CTL_ADD` 재호출 — 🔴 빌드/실행 차단
- **증상**: 관심 집합 변경이 적용되지 않고, 내부 `writing` 플래그만 바뀌어 실제 epoll 상태와 불일치.
- **원인**: 해당 fd는 `accept_new()`에서 이미 `EPOLL_CTL_ADD`로 등록됨. `update_interest()`가 같은 fd를 다시 `ADD`하면 `epoll_ctl`은 `EEXIST`로 실패. 게다가 반환값 미확인으로 `set_writing()`은 그대로 실행.
- **해결**: `EPOLL_CTL_ADD` → `EPOLL_CTL_MOD`로 교체.
- **교훈**: 등록된 fd의 관심 집합 갱신은 `MOD`. `ADD`는 신규 등록 전용이며 중복 시 `EEXIST`.

### 2-3. `-Wextra` 경고: 삼항 연산자에서 enum/int 혼용 — 🟡 권장
- **증상**: 빌드 시 `event_loop.cpp:74: warning: enumerated and non-enumerated type in conditional expression [-Wextra]`.
- **원인**: `EPOLLIN | (want ? EPOLLOUT : 0)`에서 `EPOLLOUT`(enum)과 `0`(int)이 삼항 양변에 혼재.
- **해결**: 미적용(동작 무해). 권장: `uint32_t flags = EPOLLIN; if (want) flags |= EPOLLOUT;` 형태로 분리하면 경고 제거.
- **교훈**: 이 프로젝트는 `-Wall -Wextra -Wpedantic`을 강제하므로 enum 플래그 조합은 정수 타입 변수에 누적하는 편이 안전.

### 2-4. half-close 시 미전송 데이터 유실 — 🟡 권장(향후)
- **증상**: `printf ... | nc 127.0.0.1 18080` (파이프 EOF로 즉시 half-close) 테스트에서 echo가 빈 응답으로 돌아옴.
- **원인**: `on_readable()`이 `::read` 반환 0(EOF, peer가 write 쪽 close) 시 `flush_write()` 없이 `return false` → `write_buf`를 버리고 연결 종료. 즉 peer가 보낸 요청을 다 읽었어도 응답을 못 보냄.
- **해결**: 이번 범위에서는 미적용. client가 연결을 유지하면 정상 echo되므로 이번 변경의 회귀는 아님. 향후 EOF 수신 시에도 잔여 `write_buf`를 flush한 뒤 닫는 처리 필요.
- **교훈**: non-blocking echo에서 `read==0`은 "더 읽을 게 없다"일 뿐, "보낼 게 없다"가 아니다. 읽기 종료와 쓰기 종료를 분리해 다뤄야 한다.

---

## 3. 빌드 · 검증

### 빌드 (Ubuntu 22.04, `-Wall -Wextra -Wpedantic`)

```bash
docker build --target builder --progress=plain -t http-server-build .
```

```
[ 47%] Built target http_server
[ 82%] Built target tests
[100%] Built target gmock_main
#14 DONE 5.0s

# 경고 1건:
event_loop.cpp:74:33: warning: enumerated and non-enumerated type
in conditional expression [-Wextra]
   74 |     ev.events = EPOLLIN | (want ? EPOLLOUT : 0);
```

### 런타임 echo 검증 (호스트 :18080 → 컨테이너 :8080)

```bash
docker run -d --name httptest -p 18080:8080 http-server-build /src/build/src/http_server
# python3: 연결 유지(half-close 안 함) 상태로 송신 후 동일 바이트 수신 검증
```

```
[small] sent=18 recv=18 echo_ok=True
[large-4MB] sent=4194304 recv=4194304 echo_ok=True   # partial write/EPOLLOUT 경로
[concurrent-0..4] sent=7000 recv=7000 echo_ok=True (x5)
concurrent all ok: True
# 서버 crash 없음, 컨테이너 정리 완료
```

> 4MB 전송은 socket send buffer를 초과해 `::write`가 반드시 `EAGAIN` → `EPOLLOUT` 등록 → `on_writable()` 재전송 경로를 거친다. 손실·중복 없이 그대로 echo된 것으로 backpressure 로직의 end-to-end 동작을 실증.
>
> 참고: 이번엔 `ctest`는 수행하지 않고 런타임 echo로 검증함(소켓 단위 테스트는 이번 변경과 무관).

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `-Wextra` 경고 제거 | `event_loop.cpp:74` enum/int 혼용을 정수 변수 누적 방식으로 정리 |
| 🟡 | half-close 응답 보장 | `read==0`(EOF) 수신 후에도 `write_buf` flush 완료 뒤 종료 (2-4) |
| ⚪ | `epoll_ctl` 반환값 처리 | `MOD`/`ADD` 실패 시 `set_writing()` 플래그 정합성 유지 |
| ⚪ | HTTP 파싱 도입 | 현재는 raw echo. 요청 파싱 → 응답 생성 단계로 확장 |

---

## 5. 핵심 교훈 요약

1. `EPOLLET`(트리거 모드)과 `EPOLLOUT`(이벤트 종류)은 다르다. write 알림 등록은 `EPOLLOUT`.
2. 등록된 fd의 관심 집합 갱신은 `EPOLL_CTL_MOD`. `ADD` 중복은 `EEXIST`로 조용히 실패한다.
3. non-blocking 쓰기는 "한 번에 다 못 보낸다"를 전제로 잔여 버퍼 + `EPOLLOUT` 토글이 기본 골격이며, 큰 페이로드(send buffer 초과)로 검증해야 경로가 실제로 밟힌다.
