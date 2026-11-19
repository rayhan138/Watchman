# Offline network protocol/result tests

`network_test_protocol_test.cpp` exercises the header-only production helpers in
`network_test_protocol.h`. It uses no Windows networking APIs, sockets, DNS,
application engine, database, or external services. Checks remain enabled in
Release builds (they do not depend on `assert`).

Add this inside the existing `if(BUILD_TESTING)` block in `CMakeLists.txt`:

```cmake
add_executable(NetworkTestProtocolTest tests/network_test_protocol_test.cpp)
target_include_directories(NetworkTestProtocolTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME NetworkTestProtocol COMMAND NetworkTestProtocolTest)
```

No additional libraries are required. Run only this test with:

```text
ctest --test-dir build -C Release -R "^NetworkTestProtocol$" --output-on-failure
```

## Parent integration contract

`SpeedTestResult` now includes:

- `warning_message`: usable but partial/unconfirmed measurements and transport
  issues; separate from `error_message`, which describes failed phases.
- `download_state` and `upload_state`: `network_test::TransferState::{NotRun,
  Complete, Partial, Failed}`.

The renderer should show `warning_message` when nonempty, not report a clean
success, and explain that `Partial` results are provisional. Failed phases can
coexist with a usable measurement from the other phase. Existing engine getters
return the extended result by value and need no signature change. No fields in
`snapshots.h` have been changed. The parent owns the M-Lab disclosure dialog.

`PingTester::GetProbeError()` and `IsRunning()` expose worker/ICMP-handle failures
for future snapshot integration. `GetQualityScoreString()` distinguishes
`Not measured`, `No ICMP response`, and `Probe unavailable`; zero successful
Google ICMP probes do not prove an Internet outage. A zero quality percentage
alone still cannot distinguish those states in the existing network snapshot.

## Explicit cancellation/deadline limitation

There is deliberately no new cancellation API. SpeedTester and DiagnosticsRunner
still use synchronous OS calls. The ten-second speed-test window and upload
thirteen-second reader window are checked only between calls, not hard deadlines.
WinHttpWebSocketShutdown closes the upload sending channel while leaving receiving
open, allowing queued final measurements and a peer close to be consumed before
join. It does not guarantee a blocked receive returns; send, shutdown, receive,
DNS resolution, and destructor joins can still wait on the OS/peer. HTTP timeout
configuration must not be advertised as guaranteed WebSocket cancellation.

A genuinely bounded cancel implementation needs a separately reviewed I/O lifetime
and cancellation design (for example asynchronous WinHTTP with coordinated handle
ownership). Do not detach workers that access the owning service.

## Coverage boundaries

Tests cover units (including sub-millisecond RTT), AppInfo/TCPInfo field scoping,
latest-cumulative-sample aggregation, receiver-side download selection,
server-confirmed versus estimated upload, warning/failure classification,
fragment assembly/size bounds, ICMP status interpretation, no-response labels,
and worker running-flag cleanup on normal and exceptional exits.

These helper tests do not reproduce WinHTTP's scheduler, peer-close races,
blocking behavior, OS route selection, or resolver behavior. They do not assert
that live I/O is bounded. Native source files should also be compiled against
the Windows SDK; no application launch or network traffic is required for that.
