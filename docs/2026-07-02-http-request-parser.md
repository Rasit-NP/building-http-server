# HTTP request line parser 도입 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-07-02
- **작업**: echo 서버 위에 첫 HTTP 계층으로 `HttpRequest`/`HttpRequestParser`(request line 파싱) 도입
- **결과**: Ubuntu 22.04 컨테이너 빌드 성공(신규 http 파일 경고 0) · `ctest` 15/15 통과(2.03s)

---

## 1. 작업 개요

| 항목 | Before | After |
|------|--------|-------|
| HTTP 계층 | 없음 (raw echo) | `HttpRequest` struct + `HttpRequestParser` 도입 (request line만) |
| 파서 결과 계약 | 없음 | `Result{Ok, Incomplete, Error}` + `State{RequestLine, Headers, Done, Error}` 골격 |
| 단위 테스트 | Socket 6종 | + `HttpRequestParser` 4종 |
| 빌드 구성 | `http_server` 소스 목록 | `http/HttpRequestParser.cpp` 추가, 테스트용 `parsers` 라이브러리 분리·링크 |

### 설계 · 인터페이스

- `HttpRequest`(`include/http/HttpRequest.h`): `method`/`path`/`version` 3필드 struct.
  `isValid()` = **세 필드 모두 non-empty** 여부만 판정.
- `HttpRequestParser`(`include/http/HttpRequestParser.h`):
  - `Result parse(const char* data, size_t len)` — 상태머신 진입점 (현재 `RequestLine`만 구현).
  - `const HttpRequest& request() const` — 파싱 결과 접근자.
- `parseRequestLine()`(`src/http/HttpRequestParser.cpp`):
  `\r\n`로 request line 분리 → 공백 2개 기준 **정확히 3토큰**으로 분해.
  3번째 공백·토큰 부족·빈 토큰은 모두 `Error`로 거부.
- **범위 한정**: 아직 헤더/바디는 미구현, 파서는 `EventLoop`/`Connection` 경로에 **미연결**(단위 테스트로만 구동).

---

## 2. 트러블슈팅

### 2-1. partial-read를 `Error`로 고정하던 `noCRLF` 테스트 제거 🟡

- **증상**: request line 중간까지만 수신된 정상 스트리밍 상황(예: `"GET / HT"`)을
  파서가 `Error`로 반환. 기존 `noCRLF` 테스트가 **"CRLF 없음 == `Error`"** 계약을
  단정(`EXPECT` `Result::Error`)해 이 동작을 못박고 있었다.
- **원인**: 이 서버는 non-blocking + epoll이라 `read()`가 request line을 여러 조각으로
  넘길 수 있는데, 파서는 `Result::Incomplete`/`State::Headers`/`State::Done`을
  **선언만 하고 미사용** 상태였다. no-CRLF 분기가 "더 받아야 함(`Incomplete`)"이 아니라
  곧바로 `Error`를 반환한다.
- **해결**: 잘못된 계약을 고정하던 `noCRLF` 테스트를 **제거**해, `Incomplete` 재설계를
  서버 통합 시점으로 연기. 파서 로직은 "완결된 단일 버퍼" 가정을 유지(테스트 4종:
  `normal`/`threeSP`/`isEmptyToken`/`noSP`).
- **교훈**: 테스트가 **잘못된 동작을 고정**하면 이후 올바른 변경(`Incomplete` 도입)을
  막는다. 스트리밍 계약이 확정되기 전엔 "불완전한 데이터"와 "잘못된 데이터"를 구분하지
  못하는 케이스를 테스트로 못박지 않는다.

---

## 3. 빌드 · 검증

프로젝트 관례(Linux epoll · Ubuntu 22.04)에 맞춰 컨테이너에서 실행.

```bash
docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 bash -c '
  apt-get update -qq && apt-get install -y -qq build-essential cmake git
  cmake -S . -B /tmp/build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/build -j
  cd /tmp/build && ctest --output-on-failure'
```

```
[config] OK
[build]  OK
  경고: src/event_loop.cpp:114  enum/int conditional [-Wextra]  (기존 1건, 오늘 파일 무관)
         신규 http/* 파일 경고 0

100% tests passed, 0 tests failed out of 15
  - SocketTest ................ 6/6
  - HttpRequestParserTest ..... 4/4  (normal/threeSP/isEmptyToken/noSP)
  - integration ............... 5/5
Total Test time (real) = 2.03s
```

추가로 `HttpRequestParser.cpp` 단독 `-Wall -Wextra -Wpedantic` 컴파일 → **경고 0**.
경계 입력 probe로 확인된 실제 동작:

```
valid+headers   => Ok      method='GET' path='/' version='HTTP/1.1'
tab-separated   => Error   (SP만 구분자로 인식)
version=garbage => Ok      version='FOO'   ← 형식 미검증 (아래 남은 과제)
leading space   => Error   (빈 method 거부)
```

> macOS 로컬 `ctest`는 멀티컨피그 제너레이터가 `echo_client`를 `Debug/` 하위에 두어
> `integration.*` 5건이 `Not Run`이 된다. 단위 테스트는 로컬에서도 통과.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | version 형식 미검증 | `isValid()`가 non-empty만 확인 → `"FOO"` 같은 임의 문자열도 `Ok`로 false-accept. `HTTP/x.y` 검증 필요 |
| 🟡 | `Incomplete` 실동작 | 부분 read 대응. no-CRLF를 `Error`가 아닌 `Incomplete`로, `Connection` read 버퍼 누적과 연동 |
| 🟡 | 헤더 정리 | `HttpRequest.h`의 `using namespace std;` → 전역 오염. `std::` 한정 권장 |
| ⚪ | `State::Headers` 파싱 | 헤더 라인 루프 + 빈 줄 종료, 이후 파서를 `EventLoop`/`Connection` 흐름에 편입 |

---

## 5. 핵심 교훈 요약

1. `Result`/`State` enum을 미리 넓게 선언해도, **미사용 값(`Incomplete`)** 은 계약을
   오도할 수 있다 — 실제 소비처(서버 통합)를 확인하기 전에 테스트로 고정하지 않는다.
2. `isValid()`의 "비어있지 않음"은 **형식 유효성과 다르다** — version false-accept 주의.
3. 검증은 Ubuntu 22.04 컨테이너 `ctest`로 근거화 (macOS 로컬은 멀티컨피그 경로 문제로
   `integration.*`가 `Not Run`).
