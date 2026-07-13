<!--
작업 로그. 규칙은 ../CLAUDE.md 참고.
- 트러블슈팅: 증상 → 원인 → 해결 → 교훈
- 심각도: 🔴 차단 / 🟡 권장 / ⚪ 향후
- 빌드/검증 결과는 실제 실행 후 첨부 (근거 기반)
-->

# HttpRequestParser 테스트 파라미터화 (test-parameterization) — 작업 & 트러블슈팅 로그

- **날짜**: 2026-07-13
- **작업**: 개별 `TEST` 매크로로 흩어져 있던 `HttpRequestParserTest`를 GoogleTest value-parameterized test(`TEST_P` + `INSTANTIATE_TEST_SUITE_P`)로 재구성하고, 정상/증분/에러 케이스를 데이터 테이블로 통합
- **결과**: macOS 로컬(Apple clang 21) 파서 관련 `ctest` 15/15 통과, 테스트 TU `-Wall -Wextra` 경고 0 / `integration.*`는 `sys/epoll.h` 부재로 미실행(관례)

---

## 1. 작업 개요

파서 동작(`src/http/*`)은 **변경 없음**. `tests/HttpRequestParserTest.cpp` 한 파일만
고치는 `test` 리팩토링이다. 07-02~07-12 동안 케이스를 추가할 때마다 `TEST` 블록을
복붙해 온 구조를, **케이스=데이터 한 줄**로 추가되는 파라미터화 구조로 바꾼다.

| 항목 | Before | After |
|------|--------|-------|
| 테스트 형태 | 개별 `TEST`(normal/threeSP/isEmptyToken/noSP/extract/incremental/RequestLine1~3) | 3개 fixture의 `TEST_P` + 데이터 테이블 |
| 정상 케이스 | `TEST`마다 입력·기대값 하드코딩 | `std::vector<HeaderCase> cases`(3건)로 통합, `ParserTest`가 소비 |
| 증분 파싱 | `incremental` 1건, 고정 5조각 | `ChunkTest`가 chunk 크기 `{1,2,3,4,5}` 스윕 × `cases` 전체 |
| 에러 케이스 | `TEST` 5건 개별 | `std::vector<ErrorCase> errors`(6건)로 통합, `ErrorTest`가 소비 |
| 케이스 추가 비용 | `TEST` 블록 추가 | 테이블 한 줄 추가 |

- 데이터 구조체: `HeaderCase`(input + 기대 method/path/version + 기대 headers),
  `ErrorCase`(input + `error_name`).
- 헬퍼: `checkParsedHeader()`(size 일치 + 키/값 대조), `feedInChunks()`(offset을
  `chunk`씩 전진하며 `parse`, 마지막 `Result` 반환).
- 커스텀 name generator 3종(`CaseNameGenerator`/`ChunkNameGenerator`/
  `ErrorNameGenerator`)으로 테스트 리스트 가독성 확보.
- `ErrorLatch`(07-12)와 `SocketTest.*` 6종은 그대로 유지.

---

## 2. 트러블슈팅

### 2-1. 파일 끝에 빈 이름의 미완성 `TEST` 스텁 잔존 🔴

- **증상**: 작성 중 파일 말미에 `TEST(HttpRequestParserTest, )`(이름·본문 없음,
  개행 없음)가 남아 있었음. 이대로면 컴파일 차단.
- **원인**: 파라미터화로 옮기는 도중의 미완결 편집이 커밋 전 working tree에 남음.
- **해결**: 해당 스텁 줄 제거. 파일이 `ErrorLatch` 테스트에서 정상 종료.
- **교훈**: `TEST(...)` 매크로는 이름·본문이 없으면 문법 오류다. 파라미터화 이관
  중간 산출물은 빌드로 한 번 걸러야 한다.

### 2-2. `gtest_discover_tests`가 빌드 단계에서 abort — 잘못된 param name 🔴

- **증상**: 컴파일·링크는 성공하나, 빌드 마지막에 다음과 함께 `make` 실패
  (`Error 1/2`)하여 `ctest`까지 도달하지 못함:
  ```
  [ FATAL ] gtest-param-util.h:585: Condition IsValidParamName(param_name) failed.
  Parameterized test name 'ErrorTest: Empty Token' is invalid,
    in tests/HttpRequestParserTest.cpp line 133
  Result: Subprocess aborted
  ```
- **원인**: GoogleTest의 파라미터 인스턴스 이름은 **ASCII 영숫자 + `_` 만** 허용
  (`IsValidParamName`). `ErrorNameGenerator`가 만든 이름이 이를 위반했다. 위반은
  두 겹이었다 — (1) 접두사 `"ErrorTest: "`의 **콜론+공백**, (2) `errors` 테이블의
  `error_name` 값(`"Empty Token"`, `"Three SP"`, …)에 든 **공백**. `CaseNameGenerator`
  (`ParserTest0`…)·`ChunkNameGenerator`(`ChunkTest1`…)는 영숫자뿐이라 무관.
- **해결**: 2단계로 좁혀 수정. ① 접두사 `"ErrorTest: "` → `"ErrorTest_"`(콜론·공백
  제거) ② `error_name` 값의 공백 제거(`"EmptyToken"`, `"ThreeSP"`, `"NoSP"`,
  `"WrongVersion"`, `"NoColonInHeader"`, `"BareLF"`). 이후 discovery 정상 통과.
- **교훈**: `gtest_discover_tests`는 **빌드 단계에서 테스트 바이너리를 실행**해 이름을
  수집하므로, 잘못된 param name은 런타임 실패가 아니라 **빌드 실패**로 나타난다.
  커스텀 name generator의 반환 문자열은 접두사·데이터 양쪽 모두 `[A-Za-z0-9_]`로
  제한해야 한다.

---

## 3. 빌드 · 검증

macOS 로컬(Apple clang 21). 서버 본체·`integration.*`는 `sys/epoll.h` 부재로
macOS에서 빌드/실행 불가 → 07-05·07-06·07-12와 동일하게 파서 단위 테스트만 검증하고
Ubuntu/CI 검증은 대기.

```bash
# 1) 테스트 타깃 빌드 (gtest_discover_tests 포함)
cmake --build build --target tests

# 2) 파서 관련 테스트 실행
cd build && ctest -R "ParserTest|ChunkTest|ErrorTest|HttpRequestParserTest" --output-on-failure

# 3) 테스트 TU 경고 점검 (-Wall -Wextra)
c++ -std=c++17 -Wall -Wextra -Iinclude \
    -Ibuild/_deps/googletest-src/googletest/include \
    -fsyntax-only tests/HttpRequestParserTest.cpp
```

```
# 빌드 결과
[100%] Built target tests

# ctest 결과
100% tests passed, 0 tests failed out of 15
Total Test time (real) = 0.06 sec
  (SocketTest 6 + HttpRequestParserTest.ErrorLatch 1
   + ParserTest 3 + ChunkTest 5 + ErrorTest 6)

# 등록 이름 확인 (--gtest_list_tests) — 가독형으로 정상 등록
  Normal/ParserTest  → ParserTest_0 / _1 / _2
  Normal/ChunkTest   → ChunkTest_1 … _5
  Error/ErrorTest    → ErrorTest_EmptyToken / _ThreeSP / _NoSP
                       / _WrongVersion / _NoColonInHeader / _bareLF

# 경고 점검 결과
exit: 0   (-Wall -Wextra 경고 0건)
```

---

## 4. 남은 과제

| 우선순위 | 항목 | 내용 |
|---------|------|------|
| 🟡 | `feedInChunks`의 미초기화 반환 | `HttpRequestParser::Result r;`을 초기화 없이 선언. 현재 입력은 항상 비어있지 않아 루프가 1회 이상 돌지만, 빈 `input`이 들어오면 미초기화 값 반환(UB). `Result r = Result::Incomplete;` 등으로 초기화 권장 |
| 🟡 | 미사용 fixture 멤버 | `ParserTest`/`ChunkTest`/`ErrorTest`의 멤버 `HttpRequestParser parser`가 각 `TEST_P` 본문이 지역 parser를 새로 만들어 쓰므로 dead. 멤버 제거하거나 지역 변수 대신 멤버 사용으로 일원화 |
| ⚪ | 리스트 라벨 정리 | `HeaderCase`/`ErrorCase`에 `operator<<`/`PrintTo` 부재 → `--gtest_list_tests` 주석에 `GetParam() = NNN-byte object <hex…>` 덤프. 테스트 이름·통과엔 무관하나 `PrintTo` 정의로 `input`만 출력하게 하면 정리됨 |
| ⚪ | `ChunkTest` 이축 파라미터화 | 현재 chunk 크기만 `TEST_P`, `cases`는 내부 `for` 순회. `Combine`으로 (chunk × case) 격자화하면 실패 케이스가 이름에 드러남 |
| ⚪ | 에러 입력의 증분 latch | `ErrorTest`는 한 번에 준 첫 `parse` 반환만 검증. 증분 급식 시 error latch 유지까지 보려면 케이스 확장 필요 |

---

## 5. 핵심 교훈 요약

1. `gtest_discover_tests`는 빌드 시점에 바이너리를 실행하므로, **잘못된 param name은
   빌드 실패**로 표면화된다. name generator 반환값은 접두사·데이터 모두
   `[A-Za-z0-9_]`로 제한한다(2-2).
2. 파라미터화의 실익은 **케이스 추가가 코드 추가가 아니라 데이터 한 줄**이 되는 것.
   정상/증분/에러를 fixture로 분리하고 테이블로 모으면 커버리지 확장이 값싸진다.
3. 리팩토링 중간 산출물(빈 `TEST` 스텁 등)은 반드시 **빌드로 한 번 거른다**(2-1).
