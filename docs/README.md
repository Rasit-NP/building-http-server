# 작업 로그 인덱스

`building-http-server`의 일자별 작업 & 트러블슈팅 문서 목록.
새 문서 추가 시 **최신 항목이 위로** 오도록 한 줄씩 등록한다. (규칙: [`../CLAUDE.md`](../CLAUDE.md), 양식: [`_TEMPLATE.md`](_TEMPLATE.md))

- [2026-06-08] nonblocking-io — listening/client socket을 `O_NONBLOCK`으로 전환, `handle_client()` 분리, 종료 플래그를 `std::atomic<bool>`(lock-free)로 교체. 빌드 차단(🔴: `accept()` optional 반환값 폐기 + `Socket`/`nullopt` 비교) 수정 ([문서](2026-06-08-nonblocking-io.md))
- [2026-06-07] socket-unit-tests — 더미 `calculator` 모듈 제거, 테스트를 RAII `Socket` 단위 테스트 6종으로 전환. 빌드 차단 2건(🔴: `sockets` 미링크 → undefined symbols, `sockets` include 경로 누락 → `socket.h` not found) 발견·수정 ([문서](2026-06-07-socket-unit-tests.md))
- [2026-06-05] graceful-shutdown — `SIGINT`/`SIGTERM` 핸들러 + `accept()`→`std::optional<Socket>`로 graceful shutdown 구현. idle 종료는 정상, 활성 연결 중 종료는 미보장(향후 과제) ([문서](2026-06-05-graceful-shutdown.md))
- [2026-06-02] socket-refactoring — `main.cpp` echo 로직을 RAII `Socket` 클래스로 분리, 빌드 통과 ([문서](2026-06-02-socket-refactoring.md))
