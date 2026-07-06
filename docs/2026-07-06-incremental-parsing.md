# 증분 파싱(incremental parsing) 도입 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-07-06
- **작업**: 호출마다 상태를 잃던 `HttpRequestParser`를 **내부 버퍼(`buffer_`)에 누적**하는
  stateful 증분 파서로 리팩토링 (07-05에 남긴 "`Incomplete` 실동작" 과제 해소)
- **결과**: macOS 로컬(Apple clang 21.0.0) `ctest` 파서 6/6 통과 · 파서 코드 `-Wall -Wextra` 경고 0

---

## 1. 작업 개요

| 항목 | Before (07-05) | After (07-06) |
|------|----------------|---------------|
| 파싱 모델 | 호출마다 `offset=0` 초기화 → 호출 간 상태 유실 | `buffer_`에 append 누적, `offset_` 멤버로 진행 위치 유지 → 진짜 증분 |
| 파서 시그니처 | `parseRequestLine(data, len, offset)` | 인자 없이 멤버(`buffer_`/`offset_`) 사용 |
| `parse()` 구조 | 단일 `switch` 1회 dispatch | `buffer_.append` 후 `while(true)` 상태 루프 |
| `find_crlf` | `(const char* data, size_t size, size_t begin)` | `(const std::string& buf, size_t start)` |
| 상한 | 없음 | `kMaxBufferBytes = 8*1024` 초과 시 `Error` |
| 단위 테스트 | 5종 | + `incremental`(5조각 분할 급식) = 6종 |

### 설계 · 인터페이스

- **버퍼 누적**: `parse(data, len)`이 먼저 `buffer_.append(data, len)`으로 이어붙인 뒤,
  `while(true)` 루프가 `state_`에 따라 `parseRequestLine`/`parseHeaders`를 반복 구동한다.
  CRLF가 아직 안 온 조각은 각 파서가 `Incomplete`를 반환하고, 다음 호출이 이어받는다.
- **하위 파서의 상태 소유**: `parseRequestLine`/`parseHeaders`가 스스로 `state_`를
  전이시키고(`Headers`/`Done`/`Error`), `parse()` 루프는 그 상태를 읽어 dispatch만 한다.
- **`incremental` 테스트**: `"GET /path H"` → `"TTP/1.1\r\n"` → `"Host: localhost:8080"` →
  `"\r\nUser-Agent: curl/8.0\r"` → `"\n\r\n"` 처럼 **토큰/CRLF 중간을 쪼갠** 5조각을 순차 급식,
  마지막에 `Ok` + method/path/version/headers 정확 추출을 단언.

---

## 2. 트러블슈팅

발생(발견) 순서대로 기록. 코드는 사용자가 직접 수정, Claude는 분석·검증만 수행.

### 2-1. `parse()`가 성공 상태를 덮어써 완결 요청도 `Incomplete` 🔴

- **증상**: 빌드는 성공하나 `ctest`에서 `normal`·`extract`·`incremental` 3종 실패.
  완결된 요청도 `Result::Ok`가 아니라 `Incomplete` 반환(`Actual: false`).
- **원인**: `parse()` 루프의 각 case가 하위 파서 성공(`Ok`) 뒤 `state_ = State::Headers`를
  **무조건 재대입**했다. `parseRequestLine`이 내부에서 `parseHeaders`까지 돌려 이미
  `state_ = Done`을 세팅했는데, 루프가 이를 `Headers`로 되돌림 → 재진입한 `parseHeaders`가
  `offset_`(버퍼 끝)에서 아무것도 못 읽고 `Incomplete` 반환. 하위 파서와 외부 루프가
  **상태를 이중 관리**하며 충돌.
- **해결**: 성공 후 `state_` 재대입 제거. 루프는 `break`로 하위 파서가 세팅한 상태를 그대로
  읽고, `case State::Done: return Result::Ok;`에서 종결.
- **교훈**: 상태 전이의 **소유권을 한 곳**에 둔다. 하위 파서가 `state_`를 책임지면
  dispatch 루프는 그것을 **덮어쓰지 말고 읽기만** 해야 한다(단일 진리 출처).

### 2-2. `find_crlf` 빈 버퍼 size 언더플로 🟡

- **증상**: 첫 `parse(data, 0)`(빈 조각)처럼 `buffer_`가 비어 있는 상태에서 `find_crlf` 진입 시
  범위 밖 접근 위험(UB). 테스트 실패로는 드러나지 않는 잠복 결함.
- **원인**: `size_t len = buf.size() - 1;` — `size()==0`이면 `0 - 1`이 `SIZE_MAX`로 언더플로,
  `for (i; i<len; ...)`가 거의 무한 순회하며 `buf[i+1]`을 읽는다.
  (초기 `int len` 버전도 `-1` 절단으로 동일 취약.)
- **해결**: 함수 첫머리에 `if (buf.size() < 2) return std::string::npos;` 가드 추가.
  `size()==1`은 `len=0`이라 루프 미실행으로 이미 안전, `size()==0`만 이 가드로 차단.
- **교훈**: `unsigned`에서 `size() - k`는 항상 **언더플로 가드**를 동반한다.

### 2-3. 버퍼 상한 초과 시 `state_` 미고정 🟡

- **증상**: `buffer_.size() > kMaxBufferBytes`에서 `Result::Error`만 반환하고 상태를
  갱신하지 않아, 이후 `parse()` 재호출이 계속 처리될 소지.
- **원인**: early return에 `state_ = State::Error` 세팅 누락.
- **해결**: 상한 초과 분기에서 `state_ = State::Error;` 후 `Error` 반환하여 이후 호출을
  `case State::Error`로 흡수.

### 2-4. 리팩토링 잔재 `kNpos` 死코드 ⚪

- **증상**: `find_crlf`가 `std::string::npos`를 쓰게 되며 파일 스코프 `kNpos` 상수가 미사용.
- **해결**: `constexpr size_t kNpos` 선언 제거.

---

## 3. 빌드 · 검증

이번 세션도 **macOS 로컬(Apple clang 21.0.0)** 에서 파서 단위 테스트를 검증했다.
`epoll` 의존 부분(`http_server`/`integration.*`)은 Darwin에서 빌드 불가라 제외했다.

```bash
# 파서 단위 테스트 (macOS 로컬)
cmake --build build --target tests            # 파서 코드 경고 0
ctest --test-dir build -R HttpRequestParserTest --output-on-failure

# 파서 단독 재컴파일로 경고 확인
c++ -std=c++17 -Wall -Wextra -Iinclude -c src/http/HttpRequestParser.cpp -o /dev/null   # exit 0
```

수정 전 3종 실패 → 2-1 수정 후 전량 통과:

```
100% tests passed, 0 tests failed out of 6
  HttpRequestParserTest.normal ......... Passed
  HttpRequestParserTest.threeSP ........ Passed
  HttpRequestParserTest.isEmptyToken ... Passed
  HttpRequestParserTest.noSP ........... Passed
  HttpRequestParserTest.extract ........ Passed
  HttpRequestParserTest.incremental .... Passed
Total Test time (real) = 0.02s
```

> **macOS 제약**: `src/main.cpp`의 `sys/epoll.h`(Linux 전용) 부재로 `http_server` 타깃과
> `integration.*` 5종은 이 환경에서 빌드/실행 불가. 파서 단위 테스트는 플랫폼 독립이라
> 로컬에서 통과. 통합 테스트 포함 전체(16종) 재확인은 Ubuntu/CI 대기(아래 남은 과제).

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | Ubuntu 22.04/CI 검증 | macOS 로컬 검증만 수행. 컨테이너 `ctest` 전체(16종)로 `integration.*` 포함 재확인 |
| 🟡 | 파서를 서버 흐름에 편입 | 누적 파서를 `Connection` read 버퍼와 연동(현재 단위 테스트로만 구동) |
| 🟡 | 중복 헤더 처리 | 같은 name 재등장 시 `map[...] =` 덮어쓰기. 스펙상 결합(`,`) 정책 검토(07-05 이월) |
| ⚪ | request line substr 기준 | `parseRequestLine`의 `buffer_.substr(0, crlf)`를 `offset_` 기준으로 정리(현재 무해) |
| ⚪ | 재귀·루프 이중화 정리 | `parseRequestLine`이 `parseHeaders`를 직접 호출 + 외부 루프도 dispatch → 하나로 단일화 |

---

## 5. 핵심 교훈 요약

1. 상태 전이의 **소유권을 한 곳**에 둔다 — 하위 파서가 `state_`를 책임지면 dispatch 루프는
   덮어쓰지 말고 읽기만 해야 한다(2-1, 3종 회귀의 근본 원인).
2. `unsigned` 산술 `size() - k`는 **언더플로 가드**를 동반한다(2-2).
3. 조기 반환(early return) 경로에서도 **상태(state)를 일관되게 고정**해 이후 호출이 오염되지
   않게 한다(2-3).
