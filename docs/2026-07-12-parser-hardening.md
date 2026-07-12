<!--
작업 로그. 규칙은 ../CLAUDE.md 참고.
- 트러블슈팅: 증상 → 원인 → 해결 → 교훈
- 심각도: 🔴 차단 / 🟡 권장 / ⚪ 향후
- 빌드/검증 결과는 실제 실행 후 첨부 (근거 기반)
-->

# HttpRequestParser 검증 강화 (parser-hardening) — 작업 & 트러블슈팅 로그

- **날짜**: 2026-07-12
- **작업**: 관대하게 통과시키던 `HttpRequestParser`에 4종의 거부 규칙(version 형식 검증·bare LF 거부·colon 없는 header 거부·Error 상태 latch)을 도입하고 회귀 테스트 4종 추가
- **결과**: macOS 로컬(Apple clang 21) 파서 `ctest` 10/10 통과, 파서 `-Wall -Wextra` 경고 0 / `integration.*`는 `sys/epoll.h` 부재로 미실행(관례)

---

## 1. 작업 개요

증분 파서(07-06)가 상태를 유지하게 되면서, 이전까지 "일단 통과"시키던 malformed
입력을 명시적으로 `Error`로 거부하는 방향으로 파서를 조인다. 07-02에서 향후
과제로 남긴 "version 형식 미검증(`"FOO"` false-accept)"을 이번에 해소한다.

| 항목 | Before | After |
|------|--------|-------|
| version 검증 | `isValid()`(비어있지 않으면 통과) → `"FOO"` 오수용 | `isValidVersion()` 추가, `HTTP/D.D`(정확히 8자) 엄격 검증 |
| bare LF | CRLF 미검출 시 무조건 `Incomplete`(잘림 대기) | request line 구간에 `\r` 없는 `\n` 있으면 `Error` |
| colon 없는 header | `continue`로 조용히 스킵(관대) | `Error` |
| Error 상태 | switch `State::Error` case가 이미 `Error` 반환 | `parse()` 진입부에 early-return latch 추가(계약 명시) + `ErrorLatch` 테스트로 고정 |

- `HttpRequest`에 `isValidVersion()` 추가(헤더 내 인라인 헬퍼).
- 신규 테스트: `RequestLine1`(version `FOO`) / `RequestLine2`(colon 없는 header)
  / `RequestLine3`(bare LF) / `ErrorLatch`(Error 후 정상 입력도 Error 유지).
- `src/CMakeLists.txt`는 소스 목록을 여러 줄로 재정렬(동작 변화 없음).

---

## 2. 트러블슈팅

> 이번 건은 빌드 중 발생한 장애가 아니라, **기존 파서의 관찰 가능한 오동작(증상)**
> 을 닫는 강화 작업이다. 각 증상은 07-02~07-06 코드/향후 과제에서 확인된 사실이다.

### 2-1. version 필드를 형식 검증 없이 수용 🟡

- **증상**: `GET / FOO\r\n\r\n` 처럼 version 자리에 임의 토큰이 와도 유효 요청으로
  파싱됨. 07-02 작업 로그에 `"FOO"` false-accept로 이미 향후 과제 기록됨.
- **원인**: `parseRequestLine()`이 3토큰 분해 후 `isValid()`(`method`/`path`/`version`
  이 모두 비어있지 않은지)만 확인. version은 "비어있지 않음"만 보장되어 형식 무검증.
- **해결**: `HttpRequest::isValidVersion()` 추가 — `size()==8`, 접두사 `"HTTP/"`,
  `version[5]`/`version[7]`이 `std::isdigit`, `version[6]=='.'` 를 모두 만족해야 통과.
  `parseRequestLine()`에서 `isValid()` 직후 `isValidVersion()` 실패 시 `Error`.
- **교훈**: RFC 7230의 `HTTP-version = "HTTP/" DIGIT "." DIGIT` 은 major/minor가
  **각 한 자리**이므로 8자 고정 검사가 규격에 부합. `HTTP/1.10`·`HTTP/10.1` 은
  의도대로 거부된다.

### 2-2. bare LF(`\n`)로 끝나는 줄이 영영 Incomplete로 대기 🟡

- **증상**: `GET / HTTP/1.1\n`(CRLF 아닌 bare LF)이 오면 `Error`가 아니라
  `Incomplete`로 판정되어, 뒤에 아무리 데이터가 와도 완결되지 않는 malformed 요청을
  "아직 덜 왔다"고 계속 기다림.
- **원인**: `find_crlf()`는 `\r\n`만 찾으므로 bare LF 입력에 `npos` 반환 →
  `parseRequestLine()`이 무조건 `Incomplete` 반환. "덜 온 것"과 "규격 위반"을 구분 못함.
- **해결**: `crlf == npos` 분기에서 `offset_..buffer_.size()` 를 훑어, `\r` 이
  선행하지 않는 `\n`(줄 시작 위치의 `\n` 포함)이 있으면 `Error`. 진짜 잘린 입력
  (`\n` 없음)만 `Incomplete` 유지.
- **교훈**: "미완결"과 "불량"을 같은 코드경로(`npos`)로 뭉뚱그리면 malformed 입력이
  무한 대기로 새는 DoS 표면이 된다. 증분 파서에서는 두 경우를 분리해야 한다.

### 2-3. colon 없는 header 줄을 조용히 스킵 🟡

- **증상**: `Host localhost\r\n`(colon 없음)처럼 잘못된 헤더 줄이 에러 없이 무시되고
  파싱이 계속됨.
- **원인**: `parseHeaders()`가 `line.find(':') == npos`일 때 `continue`로 해당 줄을
  건너뜀 — 관대한 처리.
- **해결**: `continue` 를 `state_ = State::Error; return Result::Error;` 로 교체.
- **교훈**: 헤더 문법 위반을 눈감으면 이후 요청 해석이 조용히 어긋난다. 파서는
  "모르는 줄은 버린다"보다 "규격 위반은 거부한다"가 안전하다.

### 2-4. Error 상태 계약을 진입부에서 명시(방어적) + 테스트 고정 ⚪

- **증상**: 한 번 `Error`로 판정된 파서가 후속 `parse()` 호출에서도 계속 `Error`를
  반환해야 한다는 계약이 코드/테스트로 못박혀 있지 않았음.
- **원인**: 계약 자체는 switch의 `case State::Error: return Result::Error;` 로 이미
  성립하나(HEAD 확인), 진입부에 명시가 없어 의도가 드러나지 않고 회귀 테스트도 없음.
- **해결**: `parse()` 진입부(버퍼 append·크기 검사 직후)에
  `if (state_ == State::Error) return Result::Error;` early-return 추가.
  `ErrorLatch` 테스트로 "bad 입력 → Error, 이어서 good 입력 → 여전히 Error" 고정.
- **교훈**: 이 early-return은 기존 switch case와 **기능상 중복**이다(둘 다 `Error`
  반환). 실질 가치는 (1) 진입부에서 계약을 명시해 가독성을 높이고 (2) `ErrorLatch`
  테스트로 구현과 무관하게 계약을 잠그는 데 있다. 중복 자체는 무해하나, 추후
  리팩토링 시 두 지점 중 하나로 일원화할 여지가 있다.

---

## 3. 빌드 · 검증

macOS 로컬(Apple clang 21). 서버 본체·`integration.*` 는 `sys/epoll.h` 부재로
macOS에서 빌드/실행 불가 → 07-05·07-06과 동일하게 파서 단위 테스트만 검증하고
Ubuntu/CI 검증은 대기.

```bash
# 1) 테스트 타깃 빌드
cmake --build build --target tests

# 2) 파서 테스트 실행
cd build && ctest -R HttpRequestParser --output-on-failure

# 3) 신규 코드 경고 점검 (파서 단독 컴파일)
c++ -std=c++17 -Wall -Wextra -Iinclude -c src/http/HttpRequestParser.cpp -o /dev/null
```

```
# ctest 결과
100% tests passed, 0 tests failed out of 10
Total Test time (real) = 0.04 sec
  (normal / threeSP / isEmptyToken / noSP / extract / incremental
   + 신규 RequestLine1 / RequestLine2 / RequestLine3 / ErrorLatch)

# 경고 점검 결과
exit: 0   (-Wall -Wextra 경고 0건)
```

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `HttpRequest.h` 헤더 위생 | `isValidVersion()`이 `std::isdigit`를 쓰지만 `HttpRequest.h`는 `<cctype>`를 직접 include 하지 않음. 현재는 `HttpUtil.h`(→`<cctype>`)가 먼저 포함되는 순서 덕에 컴파일되나, 단독 포함 시 깨질 수 있음 → `<cctype>` 직접 추가 필요 |
| ⚪ | Error 경로 일원화 | 2-4의 진입부 early-return과 switch `State::Error` case 중 하나로 정리 |
| ⚪ | method/path 검증 | version처럼 method(토큰 문자셋)·path 형식은 아직 미검증 |
| ⚪ | 서버 흐름 편입·중복 헤더 정책 | 07-06부터 이어지는 미해결 과제 |

---

## 5. 핵심 교훈 요약

1. 증분 파서에서 **"미완결(Incomplete)"과 "규격 위반(Error)"은 반드시 분리**해야
   한다 — 뭉뚱그리면 malformed 입력이 무한 대기로 샌다(2-2).
2. version은 RFC 7230 기준 major/minor 각 한 자리이므로 **8자 고정 검사**가 규격에
   부합한다(2-1).
3. 방어적 중복 코드는 무해할 수 있으나, 계약의 실질 보증은 **테스트**(`ErrorLatch`)가
   한다 — 구현 지점보다 계약 고정이 본질(2-4).
