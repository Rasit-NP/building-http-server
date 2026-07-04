# HTTP 헤더 파싱 도입 — 작업 & 트러블슈팅 로그

- **날짜**: 2026-07-05
- **작업**: request line만 파싱하던 `HttpRequestParser`를 **헤더 파싱**(`State::Headers`)까지 확장
- **결과**: macOS 로컬(Apple clang 21) `ctest` 파서 단위 테스트 5/5 통과 · 신규 파서 코드 경고 0 · Ubuntu/CI 검증은 대기(아래 남은 과제)

---

## 1. 작업 개요

| 항목 | Before (~07-02) | After (07-05) |
|------|-----------------|---------------|
| HTTP 파싱 범위 | request line만 → `Done` | request line + 헤더, `\r\n\r\n`(빈 줄)로 완결 |
| `HttpRequest` 필드 | `method`/`path`/`version` | + `unordered_map<string,string> headers` |
| 파서 내부 시그니처 | `parseRequestLine(data, len)` | + `size_t& offset` 공유, `parseHeaders(data, len, offset)` 신설 |
| 헬퍼 | 없음 | `http::to_lower`·`http::trim_ows` (신규 `HttpUtil.h`), 파일 스코프 `find_crlf` |
| 단위 테스트 | 4종 | + `extract`(헤더 추출) = 5종 |

### 설계 · 인터페이스

- **`parse()` 상태 분기 확장**: `RequestLine`/`Headers`/`Done`을 각각 처리하도록 `switch` 확장.
- **request line → 헤더 연쇄**: `parseRequestLine()`이 성공 시 `offset = crlf + 2`로 전진하고
  `State::Headers`로 전이한 뒤 `parseHeaders()`를 직접 호출한다.
- **`parseHeaders()`**: `find_crlf` 기반 라인 루프.
  - `:` 기준 name/value 분리 → name은 `to_lower`(대소문자 정규화), value는 `trim_ows`(선·후행 SP/TAB 제거).
  - **빈 줄** 만나면 `State::Done` + `Result::Ok`.
  - 라인이 CRLF로 끝나지 않고 버퍼가 소진되면 `Result::Incomplete`.
- **시맨틱 변화(중요)**: 이제 `Ok` = **request line + 헤더 blank line까지 완결**.
  종료 blank line이 없는 버퍼(예: `"GET / HTTP/1.1\r\n"`)는 `Ok`가 아니라 `Incomplete`다.

---

## 2. 트러블슈팅

발생(발견) 순서대로 기록. 코드는 사용자가 직접 수정, Claude는 분석·검증만 수행.

### 2-1. `find_crlf` 인자 오타로 컴파일 차단 🔴

- **증상**: `HttpRequestParser.cpp` 컴파일 실패.
  `error: no matching function for call to 'find_crlf'` /
  `note: no overload of 'size' matching 'size_t' for 2nd argument`.
- **원인**: `parseHeaders()`에서 `find_crlf(data, size, offset)`로 호출했으나 파라미터명은
  `len`이고 `size`라는 변수는 스코프에 없어 `std::size`로 해석됐다.
- **해결**: 2번째 인자를 `len`으로 정정(`find_crlf(data, len, offset)`).

### 2-2. `parseHeaders` 종료 경로에 `return` 누락 🔴

- **증상**: 헤더가 빈 줄 없이 버퍼 끝에서 끊기면 non-void 함수가 값 없이 함수 끝에 도달 → UB.
- **원인**: `while (offset < len)` 루프를 빈 줄을 못 만난 채 빠져나가는 분기에 반환문이 없었다.
- **해결**: 루프 이후 `return Result::Incomplete;` 추가("더 받아야 함"을 명시).

### 2-3. `offset` 미전진으로 request line 재파싱 🔴

- **증상**: `parseRequestLine()`이 `offset`을 전진시키지 않고 `parseHeaders()`를 호출.
  `offset == 0`이라 헤더 파서가 **request line을 다시 헤더 라인으로 스캔**했다.
- **원인**: request line은 전체 버퍼를 `std::string`으로 만들어 `find`로 떼는데,
  공유 `offset`을 request line 끝으로 옮기는 코드가 빠져 있었다.
  request line에 `:`이 없을 때만 우연히 `continue`로 넘어가 통과하는 취약 구조.
- **해결**: request line 성공 직후 `offset = crlf + 2`로 전진.
- **교훈**: 상태 간 **공유 커서(offset)** 를 넘길 땐 각 단계가 자기 소비 구간만큼 반드시
  전진시켜야 한다. path에 `:`이 든 입력(`GET http://x:80/p ...`)을 회귀 테스트로 못박아 확인.

### 2-4. 테스트의 미한정 `Result`로 전체 빌드 차단 🔴

- **증상**: `HttpRequestParserTest.cpp:39` `error: use of undeclared identifier 'Result'`.
  파서 라이브러리(`libparsers.a`)는 정상 빌드되나 **테스트 타깃이 빌드 실패** → `ctest` 실행 불가.
- **원인**: 중첩 타입을 `Result::Ok`로 미한정 참조. 나머지 테스트 4종은
  `HttpRequestParser::Result::...`로 한정하고 있었다.
- **해결**: `HttpRequestParser::Result::Ok`으로 한정.

### 2-5. `extract` 테스트가 gtest 관례 이탈 🟡

- **증상**: 신규 `extract` 테스트가 `EXPECT_*` 대신 `assert()`를 사용.
- **원인**: 실패 시 gtest 리포트 없이 `abort`, 게다가 `assert`는 Release(`-DNDEBUG`)에서
  컴파일 아웃되어 아무것도 검증하지 못한다.
- **해결**: 6개 단언을 모두 `EXPECT_TRUE(... == HttpRequestParser::Result::Ok)` 형태로 교체.

### 2-6. `normal` 테스트 회귀 실패 🟡

- **증상**: 빌드 성공 후 `ctest`에서 `HttpRequestParserTest.normal`만 실패.
  `parse(input, sizeof(input)) == Result::Ok` → `Actual: false`.
- **원인**: 입력 `"GET /index.html HTTP/1.1\r\n"`에 **종료 blank line이 없다.**
  헤더 파싱 도입으로 `Ok`의 의미가 "헤더 blank line까지 완결"로 바뀌어, 이 입력은
  정확히 `Incomplete`가 된다(파서는 정상, 테스트 기대값이 옛 시맨틱에 묶임).
- **해결**: 입력을 `"...HTTP/1.1\r\n\r\n"`으로 바꿔 완결 요청으로 만들고 `Ok` 기대 유지.
- **교훈**: 계약(시맨틱)을 바꾸는 변경은 **그 계약을 단언하던 기존 테스트를 함께 갱신**해야
  한다. 07-02에 예고한 "`Incomplete` 재설계" 경계가 이번에 그대로 드러났다.

---

## 3. 빌드 · 검증

이번 세션은 **macOS 로컬(Apple clang 21.0.0)** 에서 파서 단위 테스트를 검증했다.
`epoll` 의존 부분(`http_server`/`integration.*`)은 Darwin에서 빌드 불가라 제외했다.

```bash
# 파서 단위 테스트 (macOS 로컬)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target tests            # 신규 파서 코드 경고 0
ctest --test-dir build -R HttpRequestParserTest --output-on-failure
```

```
100% tests passed, 0 tests failed out of 5
  HttpRequestParserTest.normal ......... Passed
  HttpRequestParserTest.threeSP ........ Passed
  HttpRequestParserTest.isEmptyToken ... Passed
  HttpRequestParserTest.noSP ........... Passed
  HttpRequestParserTest.extract ........ Passed
Total Test time (real) = 0.02s
```

추가로 `libparsers.a`에 직접 링크한 독립 드라이버(`-Wall -Wextra`, 사용자 코드 미변경)로
경계 동작을 확인 → **18/18 통과**:

```
[1 normal]          Ok         method/path/version + headers 2개 정확
[2 ows]             Ok         "CONTENT-TYPE:  \ttext/html\t " → content-type="text/html"
[3 partial-headers] Incomplete 헤더 도중 끊김
[4 no-blank-line]   Incomplete 헤더 CRLF까지만, 종료 판단 불가
[5 no-headers]      Ok         "GET / HTTP/1.1\r\n\r\n" → headers 비어 있음
[6 colon-in-path]   Ok         "GET http://x:80/p ..." path 온전 (2-3 회귀)
[7 bad-reqline]     Error      "GET/x" 거부
== pass=18 fail=0 ==
```

> **macOS 제약**: `src/main.cpp:3 fatal error: 'sys/epoll.h' file not found`(Linux 전용)로
> `http_server` 타깃과 `integration.*` 5종은 이 환경에서 빌드/실행 불가(`Not Run`).
> 파서 단위 테스트는 플랫폼 독립이라 로컬에서 통과.

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | Ubuntu 22.04/CI 검증 | 이번 세션은 macOS 로컬 검증만 수행. 컨테이너 `ctest` 전체(16종)로 `integration.*` 포함 재확인 필요 |
| 🟡 | `Incomplete` 실동작 | `parse()`가 호출마다 `offset=0`으로 초기화 → 호출 간 버퍼 누적 없음. `Connection` read 버퍼와 연동해 진짜 incremental 파싱 필요 |
| 🟡 | 중복 헤더 처리 | 같은 name 재등장 시 `map[...] =` 로 **덮어쓰기**. 스펙상 결합(`,`) 정책 검토 |
| 🟡 | `using namespace std;` | `HttpRequest.h` 전역 오염(07-02부터 이월). `std::` 한정 권장 |
| ⚪ | 파서를 서버 흐름에 편입 | `EventLoop`/`Connection` 경로에 연결(현재 단위 테스트로만 구동) |
| ⚪ | request line 방식 통일 | `parseRequestLine`도 `find_crlf`/offset 기반으로 정리(2-3 근본 예방) |

---

## 5. 핵심 교훈 요약

1. 상태 간 **공유 커서(offset)** 는 각 단계가 소비분만큼 반드시 전진시켜야 한다(2-3).
2. **시맨틱을 바꾸는 변경**(`Ok`의 의미 확장)은 그 계약을 단언하던 기존 테스트를 함께
   갱신해야 한다 — 07-02에 예고한 `Incomplete` 경계가 `normal` 회귀로 현실화(2-6).
3. non-void 함수의 **모든 종료 경로에 반환문**을 둔다 — 루프 탈출 분기 누락은 UB(2-2).
4. 테스트 단언은 gtest 매크로로 — `assert`는 리포트가 없고 Release에서 무효화된다(2-5).
</content>
</invoke>
