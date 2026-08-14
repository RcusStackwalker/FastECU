# UDS/KWP2000 Protocol Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract a portable UDS/KWP2000 application-layer protocol library from the hand-rolled exchange code in the Mitsubishi Colt M32R CAN flash executor, and retrofit that executor onto it.

**Architecture:** Three new Bazel packages. A pure codec (`//src/algorithms/protocol/uds`) builds request PDUs and classifies response PDUs with no I/O. A port plus exchange helper (`//src/backend/protocol/uds`) owns one request/response round trip, absorbing NRC `0x78` responsePending. A per-transport adapter (`CanFlashUdsChannel`, in the existing `//src/backend/flash`) adds and strips the 4-byte CAN arbitration-id envelope, so the codec and client never see it.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest/gmock, `bytes::Byte`/`Bytes`/`ByteView` from `src/algorithms/protocol/bytes.h`, `fastecu::Result<T>` (`std::expected`) from `src/backend/ports/result.h`.

**Spec:** [UDS/KWP2000 protocol layer design](../specs/2026-08-14-uds-kwp2000-protocol-layer-design.md)

## Global Constraints

- **Branch:** work on `design/uds-kwp2000-protocol-layer`, already checked out. Never commit to `master`; the work lands as a pull request (CLAUDE.md).
- **Portable means portable:** every target in this plan is Qt-free, thread-free, and filesystem-free. Use `fastecu_portable_gtest` from `bazel/gtest_targets.bzl` for every test target, never `fastecu_gtest`.
- **Error model:** backend operations return `fastecu::Result<T>` / `fastecu::Status`. **Do not add new `ErrorKind` values** — the seven in `src/backend/ports/error.h` cover every case in this plan. Exceptions never cross a port.
- **Result checking:** check `Result` and `Status` with `.has_value()`, never the implicit `operator bool`. This includes rewriting `if (Status s = f(); !s)` into a plain declaration followed by `if (!s.has_value())`.
- **Byte types:** `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` only. No `QByteArray` anywhere in this plan.
- **Frame construction:** `bytes::composeBe` from `src/algorithms/protocol/bytes_compose.h` (ADR 0013). Byte literals take the `_b` suffix; three-byte fields use `u24(x)`.
- **Message construction:** `std::format` (ADR 0011). Prefer `std::string_view` by value over `const char*` (ADR 0009), gmock matchers for property assertions (ADR 0010), ranges/views over index loops (ADR 0012).
- **Headers:** every header starts with `#pragma once` (enforced by prek).
- **Mocks are package-owned:** a package defining an interface adds a `testing/` subpackage with one `cc_library(testonly = True)` per mock, each with its own test (ADR 0008).
- **Formatting gate:** run `prek run --all-files` before every commit.
- **Markdown links:** cross-document references are links with human-readable text, never backticked paths — lychee (via prek) checks links and cannot see a path written as inline code.

## File Structure

**Created:**

| File | Responsibility |
| --- | --- |
| `src/algorithms/protocol/uds/uds_pdu.h` / `.cpp` | Service-id arithmetic, structural NRC constants, request builders |
| `src/algorithms/protocol/uds/uds_pdu_test.cpp` | Builder and constant tests |
| `src/algorithms/protocol/uds/uds_response.h` / `.cpp` | `Response`, `parseResponse`, `payload`, `subfunction`, `describe` |
| `src/algorithms/protocol/uds/uds_response_test.cpp` | Classification tests, including every malformed case |
| `src/algorithms/protocol/uds/BUILD.bazel` | `:uds` library + two test targets |
| `src/backend/protocol/uds/iuds_channel.h` | The PDU-level port |
| `src/backend/protocol/uds/uds_client.h` / `.cpp` | `ExchangePolicy`, `UdsClient::request` |
| `src/backend/protocol/uds/uds_client_test.cpp` | Exchange, pending, and error-mapping tests |
| `src/backend/protocol/uds/BUILD.bazel` | `:uds_client` library + test target |
| `src/backend/protocol/uds/testing/scripted_uds_channel.h` | Scripted `IUdsChannel` fake |
| `src/backend/protocol/uds/testing/scripted_uds_channel_test.cpp` | The fake's own test (ADR 0008) |
| `src/backend/protocol/uds/testing/BUILD.bazel` | `:scripted_uds_channel` + its test |
| `src/backend/flash/can_flash_uds_channel.h` / `.cpp` | 4-byte CAN-id envelope adapter |
| `src/backend/flash/can_flash_uds_channel_test.cpp` | Envelope add/strip/reject tests |
| `docs/adr/0014-check-result-with-has-value.md` | The `.has_value()` convention |

**Modified:**

| File | Change |
| --- | --- |
| `BUILD.bazel` | Three new labels in the `portable_backend_closure` genquery (`expression` and `scope`), two new `BUILD.bazel` entries in the `portable_closure` `data` list |
| `scripts/check-portable-closure.py` | New `PORTABLE_ROOTS` entry for `src/backend/protocol/uds`; `can_flash_uds_channel` added to the `src/backend/flash` set |
| `src/backend/flash/BUILD.bazel` | `:can_flash_uds_channel` library + test target |
| `src/algorithms/protocol/colt/mitsu_colt_can_protocol.cpp` | Builder bodies re-expressed over `uds::buildRequest` |
| `src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.cpp` | Same |
| `src/algorithms/protocol/colt/BUILD.bazel` | Both colt libraries gain the `//src/algorithms/protocol/uds` dep |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp` | Six local helpers deleted, 13 exchange sites retrofitted |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp` | Log-expectation updates, four new tests |
| `src/backend/flash/ecu/BUILD.bazel` | Executor target gains two deps |
| `docs/flash-qualification-matrix.md` | `FlashEcuMitsuM32rCan` row notes the pending-retry path |
| `docs/colt_czt_47110032_can_bench_checklist.md` | Line recording pending-retry as unexercised on hardware |

---

### Task 1: UDS PDU builders and constants

**Files:**
- Create: `src/algorithms/protocol/uds/uds_pdu.h`
- Create: `src/algorithms/protocol/uds/uds_pdu.cpp`
- Create: `src/algorithms/protocol/uds/BUILD.bazel`
- Test: `src/algorithms/protocol/uds/uds_pdu_test.cpp`
- Modify: `BUILD.bazel` (the `portable_closure` `data` list)

**Interfaces:**
- Consumes: `bytes::Byte`, `bytes::Bytes`, `bytes::ByteView` from `//src/algorithms/protocol`; `bytes::composeBe` from `bytes_compose.h`.
- Produces: namespace `uds` with `kNegativeResponse`, `kPositiveResponseOffset`, `kNrcResponsePending`, `kNrcBusyRepeatRequest`, `positiveResponse(Byte) -> Byte`, `requestFromPositive(Byte) -> Byte`, and four `buildRequest` overloads returning `bytes::Bytes`. Bazel label `//src/algorithms/protocol/uds:uds`.

- [ ] **Step 1: Write the failing test**

Create `src/algorithms/protocol/uds/uds_pdu_test.cpp`:

```cpp
#include "src/algorithms/protocol/uds/uds_pdu.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using testing::ElementsAre;
using testing::IsEmpty;

TEST(UdsPduTest, PositiveResponseAddsTheServiceOffset)
{
    EXPECT_EQ(uds::positiveResponse(0x10), 0x50);
    EXPECT_EQ(uds::positiveResponse(0x27), 0x67);
    EXPECT_EQ(uds::positiveResponse(0x3B), 0x7B);
}

TEST(UdsPduTest, RequestFromPositiveIsTheInverse)
{
    EXPECT_EQ(uds::requestFromPositive(0x50), 0x10);
    EXPECT_EQ(uds::requestFromPositive(0x67), 0x27);
}

TEST(UdsPduTest, StructuralNrcConstantsMatchTheStandard)
{
    EXPECT_EQ(uds::kNegativeResponse, 0x7F);
    EXPECT_EQ(uds::kPositiveResponseOffset, 0x40);
    EXPECT_EQ(uds::kNrcResponsePending, 0x78);
    EXPECT_EQ(uds::kNrcBusyRepeatRequest, 0x21);
}

TEST(UdsPduTest, BuildsAServiceOnlyRequest)
{
    EXPECT_THAT(uds::buildRequest(0x3E), ElementsAre(0x3E));
}

TEST(UdsPduTest, BuildsASubfunctionRequest)
{
    EXPECT_THAT(uds::buildRequest(0x10, bytes::Byte{0x03}), ElementsAre(0x10, 0x03));
}

TEST(UdsPduTest, BuildsADataCarryingRequest)
{
    const bytes::Bytes data{0x12, 0x34, 0x56};
    EXPECT_THAT(uds::buildRequest(0x23, bytes::ByteView(data)),
                ElementsAre(0x23, 0x12, 0x34, 0x56));
}

TEST(UdsPduTest, BuildsASubfunctionAndDataRequest)
{
    const bytes::Bytes key{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_THAT(uds::buildRequest(0x27, bytes::Byte{0x06}, bytes::ByteView(key)),
                ElementsAre(0x27, 0x06, 0xDE, 0xAD, 0xBE, 0xEF));
}

TEST(UdsPduTest, EmptyDataYieldsAServiceOnlyFrame)
{
    EXPECT_THAT(uds::buildRequest(0x23, bytes::ByteView{}), ElementsAre(0x23));
}

TEST(UdsPduTest, BuildersNeverReturnAnEmptyFrame)
{
    EXPECT_THAT(uds::buildRequest(0x00), testing::Not(IsEmpty()));
}

} // namespace
```

- [ ] **Step 2: Create the BUILD file**

Create `src/algorithms/protocol/uds/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

package(default_visibility = [
    "//src/algorithms:__subpackages__",
    "//src/backend:__subpackages__",
    "//src/platform:__subpackages__",
    "//src/ui:__subpackages__",
    "//tests:__pkg__",
])

# root //:portable_closure reads this file as `data` to check that portable
# algorithms targets stay Qt-free; that reference is plumbing, not a layer
# dependency, so export it to the root package specifically rather than
# widen the package's general default_visibility.
exports_files(
    ["BUILD.bazel"],
    visibility = ["//:__pkg__"],
)

# Portable. Declares no QT_DEPS -- Bazel's sandbox therefore keeps Qt headers
# off the include path, so any residual Qt include fails to compile.
cc_library(
    name = "uds",
    srcs = [
        "uds_pdu.cpp",
        "uds_response.cpp",
    ],
    hdrs = [
        "uds_pdu.h",
        "uds_response.h",
    ],
    deps = [
        "//src/algorithms/diagnostics",
        "//src/algorithms/protocol",
    ],
)

fastecu_portable_gtest(
    name = "uds_pdu_test",
    srcs = ["uds_pdu_test.cpp"],
    deps = [":uds"],
)

fastecu_portable_gtest(
    name = "uds_response_test",
    srcs = ["uds_response_test.cpp"],
    deps = [":uds"],
)
```

The `uds_response.*` files and `uds_response_test.cpp` arrive in Task 2; create empty placeholder files now so this BUILD file parses:

```bash
printf '#pragma once\n' > src/algorithms/protocol/uds/uds_response.h
printf '#include "src/algorithms/protocol/uds/uds_response.h"\n' > src/algorithms/protocol/uds/uds_response.cpp
printf '// Placeholder; replaced in Task 2.\n' > src/algorithms/protocol/uds/uds_response_test.cpp
```

The test placeholder must **not** define `main()` — `fastecu_portable_gtest` links `@googletest//:gtest_main`, which supplies one. A comment-only translation unit links cleanly and reports zero tests.

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/algorithms/protocol/uds:uds_pdu_test`
Expected: FAIL — compile error, `uds_pdu.h` not found.

- [ ] **Step 4: Write the header**

Create `src/algorithms/protocol/uds/uds_pdu.h`:

```cpp
#pragma once

#include "src/algorithms/protocol/bytes.h"

// UDS/KWP2000 application-layer PDUs: [SID][subfunction?][data...].
//
// Pure and transport-independent. Nothing here knows about CAN arbitration
// ids, K-Line headers, timeouts, or retries -- the transport envelope belongs
// to IUdsChannel implementations in //src/backend/protocol/uds, and the
// exchange belongs to UdsClient there.
//
// The model is dialect-neutral: UDS and KWP2000 share this PDU shape, this
// positive-response convention, and this negative-response frame. Only the
// concrete service set differs, and that lives with each family.
namespace uds
{

// A negative response is 7F <request SID> <NRC>.
inline constexpr bytes::Byte kNegativeResponse = 0x7F;

// A positive response echoes the request SID with this offset added.
inline constexpr bytes::Byte kPositiveResponseOffset = 0x40;

// requestCorrectlyReceived-ResponsePending. The ECU has accepted the request
// and needs more time. The correct reaction is to keep LISTENING; re-sending
// is wrong. UdsClient absorbs this automatically.
inline constexpr bytes::Byte kNrcResponsePending = 0x78;

// busyRepeatRequest. Deliberately NOT absorbed by UdsClient: honoring it means
// re-TRANSMITTING the request, which is unsafe for the non-idempotent services
// this layer's first users send -- RequestDownload, TransferData, and erase
// routines. Exported so a caller that knows its own service is idempotent can
// implement the retry at its own level, where the safety argument is visible
// in review.
inline constexpr bytes::Byte kNrcBusyRepeatRequest = 0x21;

constexpr bytes::Byte positiveResponse(bytes::Byte sid)
{
    return static_cast<bytes::Byte>(sid + kPositiveResponseOffset);
}

constexpr bytes::Byte requestFromPositive(bytes::Byte positive_sid)
{
    return static_cast<bytes::Byte>(positive_sid - kPositiveResponseOffset);
}

// Four overloads rather than one variadic: the subfunction-versus-first-data-
// byte distinction is exactly what gets mis-read at call sites, so it gets its
// own named parameter position. `buildRequest(kServiceSecurityAccess, 0x05_b)`
// is then unambiguous on sight.
//
// A family that already composes its own frames with bytes::composeBe (ADR
// 0013) may keep doing so; these exist for callers that have no family builder.
bytes::Bytes buildRequest(bytes::Byte sid);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::ByteView data);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction, bytes::ByteView data);

} // namespace uds
```

- [ ] **Step 5: Write the implementation**

Create `src/algorithms/protocol/uds/uds_pdu.cpp`:

```cpp
#include "src/algorithms/protocol/uds/uds_pdu.h"

#include "src/algorithms/protocol/bytes_compose.h"

namespace uds
{

bytes::Bytes buildRequest(bytes::Byte sid)
{
    return bytes::composeBe(sid);
}

bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction)
{
    return bytes::composeBe(sid, subfunction);
}

bytes::Bytes buildRequest(bytes::Byte sid, bytes::ByteView data)
{
    return bytes::composeBe(sid, data);
}

bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction, bytes::ByteView data)
{
    return bytes::composeBe(sid, subfunction, data);
}

} // namespace uds
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/algorithms/protocol/uds:uds_pdu_test`
Expected: PASS, 9 tests.

- [ ] **Step 7: Register the BUILD file with the portable-closure guard**

In root `BUILD.bazel`, inside the `py_test(name = "portable_closure", ...)` `data` list, add the new label in alphabetical position — immediately after `"//src/algorithms/protocol/ssm:BUILD.bazel"`:

```python
        "//src/algorithms/protocol/ssm:BUILD.bazel",
        "//src/algorithms/protocol/uds:BUILD.bazel",
```

No `PORTABLE_ROOTS` change is needed for this package: `scripts/check-portable-closure.py` maps `src/algorithms` to `None`, which means "the entire subtree is portable" and recurses into every package under it.

- [ ] **Step 8: Verify the guard passes**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS.

- [ ] **Step 9: Format and commit**

```bash
prek run --all-files
git add src/algorithms/protocol/uds BUILD.bazel
git commit -m "feat: add UDS/KWP2000 PDU builders and structural constants

Dialect-neutral request builders and the two NRC constants the layer acts on
structurally. Pure and transport-independent: no CAN id, no K-Line header,
no I/O.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: Response classification

**Files:**
- Create: `src/algorithms/protocol/uds/uds_response.h` (replaces the Task 1 placeholder)
- Create: `src/algorithms/protocol/uds/uds_response.cpp` (replaces the placeholder)
- Test: `src/algorithms/protocol/uds/uds_response_test.cpp` (replaces the placeholder)

**Interfaces:**
- Consumes: `uds::kNegativeResponse`, `uds::kPositiveResponseOffset`, `uds::kNrcResponsePending`, `uds::requestFromPositive` from Task 1; `nrc_description(bytes::ByteView)` from `//src/algorithms/diagnostics`.
- Produces: `uds::ResponseKind{Positive, Negative, Malformed}`; `uds::Response{kind, service, nrc, data}` with `isPending()` and `matches(Byte)`; `uds::parseResponse(ByteView) -> Response`; `uds::payload(ByteView) -> ByteView`; `uds::subfunction(ByteView) -> std::optional<Byte>`; `uds::describe(ByteView) -> std::string`.

- [ ] **Step 1: Write the failing test**

Replace `src/algorithms/protocol/uds/uds_response_test.cpp` with:

```cpp
#include "src/algorithms/protocol/uds/uds_response.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>

namespace
{

using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

TEST(UdsResponseTest, ClassifiesAPositiveResponseAndRecoversTheRequestSid)
{
    const bytes::Bytes pdu{0x67, 0x01, 0x12, 0x34};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Positive);
    EXPECT_EQ(parsed.service, 0x27);
    EXPECT_THAT(parsed.data, ElementsAre(0x01, 0x12, 0x34));
    EXPECT_TRUE(parsed.matches(0x27));
    EXPECT_FALSE(parsed.matches(0x10));
    EXPECT_FALSE(parsed.isPending());
}

TEST(UdsResponseTest, ClassifiesAServiceOnlyPositiveResponse)
{
    const bytes::Bytes pdu{0x74};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Positive);
    EXPECT_EQ(parsed.service, 0x34);
    EXPECT_THAT(parsed.data, IsEmpty());
}

TEST(UdsResponseTest, ClassifiesANegativeResponse)
{
    const bytes::Bytes pdu{0x7F, 0x27, 0x35};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Negative);
    EXPECT_EQ(parsed.service, 0x27);
    EXPECT_EQ(parsed.nrc, 0x35);
    EXPECT_FALSE(parsed.isPending());
    EXPECT_FALSE(parsed.matches(0x27));
}

TEST(UdsResponseTest, RecognizesResponsePending)
{
    const bytes::Bytes pdu{0x7F, 0x31, 0x78};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Negative);
    EXPECT_TRUE(parsed.isPending());
}

TEST(UdsResponseTest, BusyRepeatRequestIsAnOrdinaryNegativeResponseNotPending)
{
    const bytes::Bytes pdu{0x7F, 0x36, 0x21};
    const uds::Response parsed = uds::parseResponse(pdu);

    EXPECT_EQ(parsed.kind, uds::ResponseKind::Negative);
    EXPECT_EQ(parsed.nrc, uds::kNrcBusyRepeatRequest);
    EXPECT_FALSE(parsed.isPending());
}

TEST(UdsResponseTest, EmptyPduIsMalformed)
{
    EXPECT_EQ(uds::parseResponse({}).kind, uds::ResponseKind::Malformed);
}

TEST(UdsResponseTest, TruncatedNegativeResponseIsMalformed)
{
    const bytes::Bytes bare{0x7F};
    const bytes::Bytes no_nrc{0x7F, 0x27};

    EXPECT_EQ(uds::parseResponse(bare).kind, uds::ResponseKind::Malformed);
    EXPECT_EQ(uds::parseResponse(no_nrc).kind, uds::ResponseKind::Malformed);
}

TEST(UdsResponseTest, AByteBelowTheServiceOffsetIsMalformed)
{
    // 0x10 is a request SID, not a response: no positive response can be
    // below 0x40, so an echoed request is a protocol error, not a reply.
    const bytes::Bytes pdu{0x10, 0x03};
    EXPECT_EQ(uds::parseResponse(pdu).kind, uds::ResponseKind::Malformed);
}

TEST(UdsResponseTest, PayloadSkipsTheServiceByte)
{
    const bytes::Bytes pdu{0x63, 0x27, 0x41, 0x12};
    EXPECT_THAT(uds::payload(pdu), ElementsAre(0x27, 0x41, 0x12));
    EXPECT_THAT(uds::payload({}), IsEmpty());
}

TEST(UdsResponseTest, SubfunctionIsTheSecondByteWhenPresent)
{
    const bytes::Bytes pdu{0x50, 0x03};
    const bytes::Bytes service_only{0x50};

    EXPECT_EQ(uds::subfunction(pdu), std::optional<bytes::Byte>{0x03});
    EXPECT_EQ(uds::subfunction(service_only), std::nullopt);
    EXPECT_EQ(uds::subfunction({}), std::nullopt);
}

TEST(UdsResponseTest, DescribeDelegatesToTheSharedNrcTable)
{
    const bytes::Bytes pdu{0x7F, 0x27, 0x35};
    EXPECT_THAT(uds::describe(pdu), HasSubstr("Invalid key"));
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/algorithms/protocol/uds:uds_response_test`
Expected: FAIL — compile error, `uds::ResponseKind` not declared.

- [ ] **Step 3: Confirm the expected NRC text before relying on it**

`describe()` forwards to the shared table, so the substring in the last test must match reality rather than a guess.

Run: `grep -n "0x35\|Invalid key" src/algorithms/diagnostics/dtc_tables.cpp`
Expected: a table entry mapping `0x35`. If its text differs from "Invalid key", change the `HasSubstr` argument in the test to a distinctive substring of the real entry. Do not change the table.

- [ ] **Step 4: Write the header**

Replace `src/algorithms/protocol/uds/uds_response.h` with:

```cpp
#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/uds/uds_pdu.h"

#include <optional>
#include <string>

namespace uds
{

enum class ResponseKind
{
    Positive,
    Negative,
    Malformed,
};

struct Response
{
    ResponseKind kind{ResponseKind::Malformed};

    // Always the *request* SID: a 0x67 positive response and a 7F 27 xx
    // negative response both report 0x27, so callers compare against the
    // service they sent without doing offset arithmetic.
    bytes::Byte service{};

    // Meaningful only when kind == Negative.
    bytes::Byte nrc{};

    // Bytes after the response service id. A VIEW INTO THE INPUT, not a copy:
    // it stays valid only as long as the buffer passed to parseResponse. The
    // only caller is UdsClient, which parses a buffer it owns for the whole
    // call; executors receive an owning Bytes from UdsClient::request and read
    // it with payload()/subfunction() below.
    bytes::ByteView data{};

    bool isPending() const
    {
        return kind == ResponseKind::Negative && nrc == kNrcResponsePending;
    }

    bool matches(bytes::Byte sid) const
    {
        return kind == ResponseKind::Positive && service == sid;
    }
};

// Classification is three rules applied in order:
//
//   1. pdu[0] == 0x7F is Negative, and requires at least three bytes
//      (7F <sid> <nrc>); shorter is Malformed.
//   2. pdu[0] >= 0x40 is Positive, with service = pdu[0] - 0x40.
//   3. Anything else, including an empty PDU, is Malformed.
//
// Rule 1 cannot collide with a legitimate positive response: that would
// require request SID 0x3F, which UDS reserves and no family uses.
Response parseResponse(bytes::ByteView pdu);

// Everything after the service id. Empty for an empty or service-only PDU.
bytes::ByteView payload(bytes::ByteView pdu);

// The byte after the service id, when the PDU has one.
std::optional<bytes::Byte> subfunction(bytes::ByteView pdu);

// Human-readable text for a negative-response frame, delegating to the shared
// NRC table in //src/algorithms/diagnostics. The table is not duplicated here.
std::string describe(bytes::ByteView pdu);

} // namespace uds
```

- [ ] **Step 5: Write the implementation**

Replace `src/algorithms/protocol/uds/uds_response.cpp` with:

```cpp
#include "src/algorithms/protocol/uds/uds_response.h"

#include "src/algorithms/diagnostics/nrc_parser.h"

namespace uds
{

Response parseResponse(bytes::ByteView pdu)
{
    if (pdu.empty())
    {
        return {};
    }
    if (pdu[0] == kNegativeResponse)
    {
        if (pdu.size() < 3)
        {
            return {};
        }
        return {ResponseKind::Negative, pdu[1], pdu[2], pdu.subspan(3)};
    }
    if (pdu[0] < kPositiveResponseOffset)
    {
        return {};
    }
    return {ResponseKind::Positive, requestFromPositive(pdu[0]), 0, pdu.subspan(1)};
}

bytes::ByteView payload(bytes::ByteView pdu)
{
    return pdu.empty() ? bytes::ByteView{} : pdu.subspan(1);
}

std::optional<bytes::Byte> subfunction(bytes::ByteView pdu)
{
    if (pdu.size() < 2)
    {
        return std::nullopt;
    }
    return pdu[1];
}

std::string describe(bytes::ByteView pdu)
{
    return nrc_description(pdu);
}

} // namespace uds
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test --config=release //src/algorithms/protocol/uds:all`
Expected: PASS, both targets.

- [ ] **Step 7: Format and commit**

```bash
prek run --all-files
git add src/algorithms/protocol/uds
git commit -m "feat: classify UDS/KWP2000 responses

parseResponse() splits a PDU into Positive/Negative/Malformed and always
reports the *request* SID, so callers stop doing +0x40 arithmetic and stop
indexing raw offsets. NRC text delegates to the shared diagnostics table
rather than duplicating it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: The channel port and its scripted fake

**Files:**
- Create: `src/backend/protocol/uds/iuds_channel.h`
- Create: `src/backend/protocol/uds/BUILD.bazel`
- Create: `src/backend/protocol/uds/testing/scripted_uds_channel.h`
- Create: `src/backend/protocol/uds/testing/BUILD.bazel`
- Test: `src/backend/protocol/uds/testing/scripted_uds_channel_test.cpp`

**Interfaces:**
- Consumes: `fastecu::Status`, `fastecu::Result`, `fastecu::fail`, `fastecu::ErrorKind` from `//src/backend/ports`; `bytes::ByteView`, `bytes::Bytes`.
- Produces: `uds::IUdsChannel` with `send(ByteView, const ICancellationToken&) -> Status` and `receive(int, const ICancellationToken&) -> Result<std::optional<Bytes>>`. Bazel labels `//src/backend/protocol/uds:uds_client` (the library the header ships in, completed in Task 4) and `//src/backend/protocol/uds/testing:scripted_uds_channel`. The fake exposes `expectSend(ByteView)`, `queueReceive(ByteView)`, `queueNoFrame()`, `queueError(ErrorKind, std::string)`, `sendsConsumed() -> std::size_t`, `scriptConsumed() -> bool`.

- [ ] **Step 1: Write the port header**

Create `src/backend/protocol/uds/iuds_channel.h`:

```cpp
#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <optional>

namespace uds
{

// One UDS application PDU in each direction.
//
// Implementations own the transport envelope -- the 4-byte big-endian CAN
// arbitration id for CanFlashUdsChannel, a KWP2000 0x80 header and trailing
// checksum for a future K-Line channel -- so UdsClient and every executor
// above it work purely in PDUs starting at the service id.
//
// Deliberately narrower than the transports it wraps: no configure, no
// open/close, no unblock. Connection lifetime stays with whoever owns the
// transport; this interface exists only for the duration of an exchange.
class IUdsChannel
{
  public:
    virtual ~IUdsChannel() = default;

    // Adds the envelope and transmits. `pdu` starts at the service id.
    virtual fastecu::Status send(bytes::ByteView pdu,
                                 const fastecu::ICancellationToken& cancellation) = 0;

    // Returns the next PDU with the envelope stripped, starting at the
    // response service id. A read that reaches its deadline with nothing
    // received is a successful empty optional; cancellation, disconnection,
    // and a frame that fails envelope validation are errors.
    virtual fastecu::Result<std::optional<bytes::Bytes>> receive(
        int timeout_ms, const fastecu::ICancellationToken& cancellation) = 0;
};

} // namespace uds
```

- [ ] **Step 2: Write the scripted fake**

Create `src/backend/protocol/uds/testing/scripted_uds_channel.h`:

```cpp
#pragma once

#include "src/backend/protocol/uds/iuds_channel.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uds
{

// Scripted IUdsChannel for exercising UdsClient without a transport.
// Modeled on fastecu::flash::ScriptedCanFlashTransport
// (src/backend/flash/testing/scripted_can_flash_transport.h): sends are
// matched against an expected sequence, receives are replayed from a queue.
//
// A send that does not match the next expectation fails with ErrorKind::
// Internal rather than an assertion, so a test sees the mismatch as a
// returned Error at the point of use.
class ScriptedUdsChannel final : public IUdsChannel
{
  public:
    void expectSend(bytes::ByteView pdu)
    {
        expected_.emplace_back(pdu.begin(), pdu.end());
    }
    void queueReceive(bytes::ByteView pdu)
    {
        receives_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(pdu.begin(), pdu.end())});
    }
    void queueNoFrame()
    {
        receives_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void queueError(fastecu::ErrorKind kind, std::string detail = {})
    {
        receives_.emplace_back(fastecu::fail(kind, std::move(detail)));
    }

    std::size_t sendsConsumed() const
    {
        return send_index_;
    }
    bool scriptConsumed() const
    {
        return send_index_ == expected_.size() && receives_.empty();
    }

    fastecu::Status send(bytes::ByteView pdu,
                         const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted UDS send cancelled");
        }
        if (send_index_ >= expected_.size() ||
            expected_.at(send_index_) != bytes::Bytes(pdu.begin(), pdu.end()))
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "unexpected scripted UDS send");
        }
        ++send_index_;
        return {};
    }

    fastecu::Result<std::optional<bytes::Bytes>> receive(
        int timeout_ms, const fastecu::ICancellationToken& cancellation) override
    {
        last_timeout_ms_ = timeout_ms;
        timeouts_.push_back(timeout_ms);
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted UDS receive cancelled");
        }
        if (receives_.empty())
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "no scripted UDS receive outcome");
        }
        auto result = std::move(receives_.front());
        receives_.pop_front();
        return result;
    }

    int last_timeout_ms_ = 0;
    std::vector<int> timeouts_;

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Result<std::optional<bytes::Bytes>>> receives_;
    std::size_t send_index_ = 0;
};

} // namespace uds
```

- [ ] **Step 3: Write the fake's own test**

Create `src/backend/protocol/uds/testing/scripted_uds_channel_test.cpp`:

```cpp
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using testing::ElementsAre;

TEST(ScriptedUdsChannelTest, AcceptsAnExpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    const bytes::Bytes pdu{0x10, 0x03};
    channel.expectSend(pdu);

    EXPECT_TRUE(channel.send(pdu, cancellation).has_value());
    EXPECT_EQ(channel.sendsConsumed(), 1u);
}

TEST(ScriptedUdsChannelTest, RejectsAnUnexpectedSend)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.expectSend(bytes::Bytes{0x10, 0x03});

    const fastecu::Status sent = channel.send(bytes::Bytes{0x10, 0x85}, cancellation);

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().kind, ErrorKind::Internal);
}

TEST(ScriptedUdsChannelTest, RejectsASendWithNoRemainingExpectation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;

    const fastecu::Status sent = channel.send(bytes::Bytes{0x3E}, cancellation);

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().kind, ErrorKind::Internal);
}

TEST(ScriptedUdsChannelTest, ReplaysQueuedReceivesInOrder)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.queueReceive(bytes::Bytes{0x50, 0x03});
    channel.queueNoFrame();
    channel.queueError(ErrorKind::Disconnected, "gone");

    const auto first = channel.receive(100, cancellation);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    EXPECT_THAT(**first, ElementsAre(0x50, 0x03));

    const auto second = channel.receive(100, cancellation);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->has_value());

    const auto third = channel.receive(100, cancellation);
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error().kind, ErrorKind::Disconnected);
}

TEST(ScriptedUdsChannelTest, RecordsEveryReceiveTimeout)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.queueReceive(bytes::Bytes{0x50});
    channel.queueReceive(bytes::Bytes{0x50});

    (void)channel.receive(500, cancellation);
    (void)channel.receive(3000, cancellation);

    EXPECT_THAT(channel.timeouts_, ElementsAre(500, 3000));
    EXPECT_EQ(channel.last_timeout_ms_, 3000);
}

TEST(ScriptedUdsChannelTest, HonorsCancellation)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    cancellation.cancel();
    channel.expectSend(bytes::Bytes{0x3E});

    const fastecu::Status sent = channel.send(bytes::Bytes{0x3E}, cancellation);
    const auto received = channel.receive(100, cancellation);

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error().kind, ErrorKind::Cancelled);
    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Cancelled);
}

TEST(ScriptedUdsChannelTest, ScriptConsumedReflectsRemainingWork)
{
    uds::ScriptedUdsChannel channel;
    FakeCancellationToken cancellation;
    channel.expectSend(bytes::Bytes{0x3E});
    channel.queueReceive(bytes::Bytes{0x7E});

    EXPECT_FALSE(channel.scriptConsumed());
    (void)channel.send(bytes::Bytes{0x3E}, cancellation);
    (void)channel.receive(100, cancellation);
    EXPECT_TRUE(channel.scriptConsumed());
}

} // namespace
```

- [ ] **Step 4: Confirm the cancellation fake's API before compiling**

`FakeCancellationToken`'s trigger method name must match the test above.

Run: `grep -n "class FakeCancellationToken" -A 20 src/backend/ports/testing/fake_cancellation_token.h`
Expected: a class in namespace `fastecu` with a method that sets the cancelled flag. If it is not named `cancel()`, update the two call sites in the test to the real name.

- [ ] **Step 5: Write the BUILD files**

Create `src/backend/protocol/uds/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

package(default_visibility = [
    "//src/backend:__subpackages__",
    "//src/platform:__subpackages__",
    "//src/ui:__subpackages__",
    "//tests:__pkg__",
])

exports_files(
    ["BUILD.bazel"],
    visibility = ["//:__pkg__"],
)

# Portable. No Qt, no platform, no filesystem, no thread.
cc_library(
    name = "uds_client",
    srcs = ["uds_client.cpp"],
    hdrs = [
        "iuds_channel.h",
        "uds_client.h",
    ],
    deps = [
        "//src/algorithms/diagnostics",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/uds",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "uds_client_test",
    srcs = ["uds_client_test.cpp"],
    deps = [
        ":uds_client",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/protocol/uds/testing:scripted_uds_channel",
    ],
)
```

`uds_client.*` and `uds_client_test.cpp` arrive in Task 4; create placeholders so this parses:

```bash
printf '#pragma once\n' > src/backend/protocol/uds/uds_client.h
printf '#include "src/backend/protocol/uds/uds_client.h"\n' > src/backend/protocol/uds/uds_client.cpp
printf '// Placeholder; replaced in Task 4.\n' > src/backend/protocol/uds/uds_client_test.cpp
```

As in Task 1, the test placeholder must not define `main()` — `gtest_main` supplies one.

Create `src/backend/protocol/uds/testing/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

package(
    default_testonly = True,
    default_visibility = [
        "//src:__subpackages__",
        "//tests:__pkg__",
    ],
)

cc_library(
    name = "scripted_uds_channel",
    hdrs = ["scripted_uds_channel.h"],
    deps = ["//src/backend/protocol/uds:uds_client"],
)

fastecu_portable_gtest(
    name = "scripted_uds_channel_test",
    srcs = ["scripted_uds_channel_test.cpp"],
    deps = [
        ":scripted_uds_channel",
        "//src/backend/ports/testing:fake_cancellation_token",
    ],
)
```

- [ ] **Step 6: Run the fake's test**

Run: `bazel test --config=release //src/backend/protocol/uds/testing:scripted_uds_channel_test`
Expected: PASS, 7 tests.

- [ ] **Step 7: Register the new package with the portable-closure guard**

In `scripts/check-portable-closure.py`, add an entry to `PORTABLE_ROOTS` immediately after the `src/backend/protocol` line. A sibling package needs its own entry — the existing `src/backend/protocol` entry has an explicit target set, and the script only puts that root's own `BUILD.bazel` in scope:

```python
    ROOT / "src/backend/protocol": {"protocol"},
    ROOT / "src/backend/protocol/uds": {"uds_client"},
```

In root `BUILD.bazel`, add the label to **both** the `expression` list and the `scope` list of `genquery(name = "portable_backend_closure", ...)`, immediately after the `//src/backend/protocol` entry in each. Note the two lists spell that existing entry differently — `"//src/backend/protocol:protocol"` in `expression`, `"//src/backend/protocol"` in `scope`. Match each list's surroundings:

```python
        # in `expression`:
        "//src/backend/protocol:protocol",
        "//src/backend/protocol/uds:uds_client",
```

```python
        # in `scope`:
        "//src/backend/protocol",
        "//src/backend/protocol/uds:uds_client",
```

And add the BUILD file to the `portable_closure` `data` list, after `"//src/backend/protocol:BUILD.bazel"`:

```python
        "//src/backend/protocol:BUILD.bazel",
        "//src/backend/protocol/uds:BUILD.bazel",
```

- [ ] **Step 8: Verify the guard passes**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS.

- [ ] **Step 9: Format and commit**

```bash
prek run --all-files
git add src/backend/protocol/uds BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat: add the UDS channel port and its scripted fake

IUdsChannel carries one application PDU in each direction; implementations
own the transport envelope so the client above never sees it. The scripted
fake is package-owned per ADR 0008 and has its own test.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: UdsClient

**Files:**
- Create: `src/backend/protocol/uds/uds_client.h` (replaces the Task 3 placeholder)
- Create: `src/backend/protocol/uds/uds_client.cpp` (replaces the placeholder)
- Test: `src/backend/protocol/uds/uds_client_test.cpp` (replaces the placeholder)

**Interfaces:**
- Consumes: `uds::IUdsChannel` (Task 3), `uds::parseResponse` / `uds::Response` / `uds::describe` (Task 2), `fastecu::IClock`, `fastecu::IEventSink`, `fastecu::ICancellationToken`.
- Produces: `uds::ExchangePolicy{pre_read_delay_ms, read_timeout_ms, pending_timeout_ms, max_pending_repeats}`; `uds::UdsClient(IUdsChannel&, fastecu::IClock&, fastecu::IEventSink&)` with `request(bytes::ByteView, const ExchangePolicy&, const fastecu::ICancellationToken&) -> fastecu::Result<bytes::Bytes>` returning the positive-response PDU, service byte included.

- [ ] **Step 1: Write the failing test**

Replace `src/backend/protocol/uds/uds_client_test.cpp` with:

```cpp
#include "src/backend/protocol/uds/uds_client.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::RecordingEventSink;
using testing::ElementsAre;
using testing::HasSubstr;

struct Fixture
{
    uds::ScriptedUdsChannel channel;
    FakeClock clock;
    RecordingEventSink events;
    FakeCancellationToken cancellation;

    uds::UdsClient client()
    {
        return uds::UdsClient(channel, clock, events);
    }
};

constexpr uds::ExchangePolicy kPolicy{
    .pre_read_delay_ms = 50, .read_timeout_ms = 500,
    .pending_timeout_ms = 3000, .max_pending_repeats = 3};

TEST(UdsClientTest, ReturnsThePositiveResponsePdu)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_THAT(*received, ElementsAre(0x50, 0x03));
    EXPECT_TRUE(f.channel.scriptConsumed());
}

TEST(UdsClientTest, SleepsForThePreReadDelayBeforeTheFirstRead)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.client();
    (void)client.request(request, kPolicy, f.cancellation);

    // FakeClock::sleep advances now_ by the requested duration, so the total
    // is the only observable: one 50 ms pre-read delay and nothing else.
    EXPECT_EQ(f.clock.now_, 50u);
    EXPECT_THAT(f.channel.timeouts_, ElementsAre(500));
}

TEST(UdsClientTest, AbsorbsOneResponsePendingAndReadsAgain)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_THAT(*received, ElementsAre(0x71, 0xE0));
    // Absorbed by re-reading only: exactly one transmission.
    EXPECT_EQ(f.channel.sendsConsumed(), 1u);
    // The pending read uses the longer pending timeout.
    EXPECT_THAT(f.channel.timeouts_, ElementsAre(500, 3000));
}

TEST(UdsClientTest, AbsorbsRepeatedResponsePending)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(f.channel.sendsConsumed(), 1u);
}

TEST(UdsClientTest, GivesUpAfterMaxPendingRepeats)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    for (int i = 0; i < 4; ++i)
    {
        f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    }

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Timeout);
    EXPECT_THAT(received.error().detail, HasSubstr("responsePending"));
    EXPECT_EQ(f.channel.sendsConsumed(), 1u);
}

TEST(UdsClientTest, DoesNotRetryBusyRepeatRequest)
{
    // The safety property: honoring 0x21 would mean re-TRANSMITTING, which is
    // unsafe for RequestDownload/TransferData/erase. It must surface as an
    // ordinary negative response after exactly one send.
    Fixture f;
    const bytes::Bytes request{0x36, 0x01};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x36, 0x21});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(f.channel.sendsConsumed(), 1u);
    EXPECT_TRUE(f.channel.scriptConsumed());
}

TEST(UdsClientTest, ReportsANegativeResponseWithItsNrcDescription)
{
    Fixture f;
    const bytes::Bytes request{0x27, 0x06};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x27, 0x35});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(received.error().detail, uds::describe(bytes::Bytes{0x7F, 0x27, 0x35}));
}

TEST(UdsClientTest, RejectsAResponseToADifferentService)
{
    Fixture f;
    const bytes::Bytes request{0x27, 0x05};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x50, 0x03});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(received.error().detail, HasSubstr("0x27"));
    EXPECT_THAT(received.error().detail, HasSubstr("0x10"));
}

TEST(UdsClientTest, RejectsAMalformedResponse)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x10});

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(received.error().detail, HasSubstr("malformed"));
}

TEST(UdsClientTest, ReportsATimeoutWhenNothingArrives)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueNoFrame();

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Timeout);
}

TEST(UdsClientTest, PropagatesAChannelErrorVerbatim)
{
    Fixture f;
    const bytes::Bytes request{0x10, 0x03};
    f.channel.expectSend(request);
    f.channel.queueError(ErrorKind::Disconnected, "adapter closed");

    uds::UdsClient client = f.client();
    const auto received = client.request(request, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(received.error().detail, "adapter closed");
}

TEST(UdsClientTest, RefusesToSendWhenAlreadyCancelled)
{
    Fixture f;
    f.cancellation.cancel();

    uds::UdsClient client = f.client();
    const auto received = client.request(bytes::Bytes{0x10, 0x03}, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(f.channel.sendsConsumed(), 0u);
}

TEST(UdsClientTest, RejectsAnEmptyRequest)
{
    Fixture f;

    uds::UdsClient client = f.client();
    const auto received = client.request({}, kPolicy, f.cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Internal);
    EXPECT_EQ(f.channel.sendsConsumed(), 0u);
}

TEST(UdsClientTest, LogsOnceForEachAbsorbedPending)
{
    Fixture f;
    const bytes::Bytes request{0x31, 0xE0};
    f.channel.expectSend(request);
    f.channel.queueReceive(bytes::Bytes{0x7F, 0x31, 0x78});
    f.channel.queueReceive(bytes::Bytes{0x71, 0xE0});

    uds::UdsClient client = f.client();
    (void)client.request(request, kPolicy, f.cancellation);

    int pending_lines = 0;
    for (const auto& entry : f.events.logs)
    {
        if (entry.second.find("responsePending") != std::string::npos)
        {
            ++pending_lines;
        }
    }
    EXPECT_EQ(pending_lines, 1);
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/protocol/uds:uds_client_test`
Expected: FAIL — compile error, `uds::UdsClient` not declared.

- [ ] **Step 3: Write the header**

Replace `src/backend/protocol/uds/uds_client.h` with:

```cpp
#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/iuds_channel.h"

namespace uds
{

// Per-exchange timing. Vendor sequencing quirks live at the call site; only
// the timing knobs each exchange genuinely needs are parameters here.
struct ExchangePolicy
{
    // Quiet period between the write and the first read. Several families
    // need one; 0 skips the sleep entirely.
    int pre_read_delay_ms = 0;

    int read_timeout_ms = 500;

    // Read timeout used once the ECU has reported responsePending. Separate
    // from read_timeout_ms because "busy, wait" legitimately takes much
    // longer than a normal reply.
    int pending_timeout_ms = 3000;

    // Guard against an ECU that reports responsePending forever.
    int max_pending_repeats = 10;
};

// One UDS request/response round trip.
//
// Owns exactly one concern: getting a validated positive response back, or a
// typed error. It does not own session state, tester-present keepalive, or
// service sequencing -- those stay with each family's executor, where the
// vendor quirks are readable.
class UdsClient
{
  public:
    UdsClient(IUdsChannel& channel, fastecu::IClock& clock, fastecu::IEventSink& events);

    // Sends `pdu` and returns the positive-response PDU, service byte
    // included and envelope already stripped by the channel.
    //
    // NRC 0x78 (responsePending) is absorbed by re-READING, never by
    // re-sending. NRC 0x21 (busyRepeatRequest) is deliberately NOT absorbed:
    // honoring it means re-transmitting, which is unsafe for the
    // non-idempotent services this layer's callers send. It surfaces as an
    // ordinary negative response so the caller decides.
    fastecu::Result<bytes::Bytes> request(bytes::ByteView pdu, const ExchangePolicy& policy,
                                          const fastecu::ICancellationToken& cancellation);

  private:
    IUdsChannel& channel_;
    fastecu::IClock& clock_;
    fastecu::IEventSink& events_;
};

} // namespace uds
```

- [ ] **Step 4: Write the implementation**

Replace `src/backend/protocol/uds/uds_client.cpp` with:

```cpp
#include "src/backend/protocol/uds/uds_client.h"

#include <format>
#include <optional>
#include <utility>

#include "src/algorithms/protocol/uds/uds_pdu.h"
#include "src/algorithms/protocol/uds/uds_response.h"

namespace uds
{
namespace
{
using fastecu::ErrorKind;
using fastecu::fail;
using fastecu::LogLevel;
} // namespace

UdsClient::UdsClient(IUdsChannel& channel, fastecu::IClock& clock, fastecu::IEventSink& events)
    : channel_(channel), clock_(clock), events_(events)
{
}

fastecu::Result<bytes::Bytes> UdsClient::request(
    bytes::ByteView pdu, const ExchangePolicy& policy,
    const fastecu::ICancellationToken& cancellation)
{
    if (pdu.empty())
    {
        return fail(ErrorKind::Internal, "UDS request PDU is empty");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before request");
    }

    const bytes::Byte expected_service = pdu[0];

    const fastecu::Status sent = channel_.send(pdu, cancellation);
    if (!sent.has_value())
    {
        return std::unexpected(sent.error());
    }

    int delay_ms = policy.pre_read_delay_ms;
    int timeout_ms = policy.read_timeout_ms;

    // One normal read, then up to max_pending_repeats further reads while the
    // ECU holds us on 0x78.
    for (int attempt = 0; attempt <= policy.max_pending_repeats; ++attempt)
    {
        if (delay_ms > 0)
        {
            const fastecu::Status slept = clock_.sleep(delay_ms, cancellation);
            if (!slept.has_value())
            {
                return std::unexpected(slept.error());
            }
        }

        fastecu::Result<std::optional<bytes::Bytes>> received =
            channel_.receive(timeout_ms, cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return fail(ErrorKind::Timeout, "no response within the read timeout");
        }

        bytes::Bytes frame = std::move(**received);
        const Response parsed = parseResponse(frame);

        if (parsed.isPending())
        {
            events_.log(LogLevel::Debug,
                        std::format("ECU reported responsePending for SID 0x{:02x}; waiting",
                                    expected_service));
            // Only the first read observes the caller's pre-read delay; a
            // pending re-read waits inside the (longer) receive timeout.
            delay_ms = 0;
            timeout_ms = policy.pending_timeout_ms;
            continue;
        }

        switch (parsed.kind)
        {
        case ResponseKind::Malformed:
            return fail(ErrorKind::BadResponse,
                        std::format("malformed UDS response: {}", bytes::toHex(frame)));
        case ResponseKind::Negative:
            return fail(ErrorKind::BadResponse, describe(frame));
        case ResponseKind::Positive:
            if (!parsed.matches(expected_service))
            {
                return fail(ErrorKind::BadResponse,
                            std::format("expected response to SID 0x{:02x}, got 0x{:02x}",
                                        expected_service, parsed.service));
            }
            return frame;
        }
    }

    return fail(ErrorKind::Timeout,
                std::format("ECU still reporting responsePending after {} repeats",
                            policy.max_pending_repeats));
}

} // namespace uds
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/protocol/uds:uds_client_test`
Expected: PASS, 14 tests.

- [ ] **Step 6: Verify the whole portable graph still holds**

Run: `bazel test --config=release //:portable_closure //src/algorithms/protocol/uds:all //src/backend/protocol/uds:all //src/backend/protocol/uds/testing:all`
Expected: PASS.

- [ ] **Step 7: Format and commit**

```bash
prek run --all-files
git add src/backend/protocol/uds
git commit -m "feat: add UdsClient with asymmetric pending handling

One request/response round trip: validates the service echo, maps negative
responses to BadResponse carrying the shared NRC description, and absorbs
0x78 responsePending by re-READING.

0x21 busyRepeatRequest is deliberately not absorbed -- honoring it means
re-transmitting, which is unsafe for RequestDownload, TransferData, and erase
routines. An explicit test asserts exactly one send on 0x21.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: CanFlashUdsChannel

**Files:**
- Create: `src/backend/flash/can_flash_uds_channel.h`
- Create: `src/backend/flash/can_flash_uds_channel.cpp`
- Test: `src/backend/flash/can_flash_uds_channel_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`
- Modify: `BUILD.bazel`, `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: `uds::IUdsChannel` (Task 3); `fastecu::flash::ICanFlashTransport` from `src/backend/flash/flash_executor.h`; `bytes::composeBe`, `bytes::readU32Be`.
- Produces: `fastecu::flash::CanFlashUdsChannel(ICanFlashTransport&, std::uint32_t request_id, std::uint32_t response_id)` implementing `uds::IUdsChannel`. Bazel label `//src/backend/flash:can_flash_uds_channel`.

- [ ] **Step 1: Write the failing test**

Create `src/backend/flash/can_flash_uds_channel_test.cpp`:

```cpp
#include "src/backend/flash/can_flash_uds_channel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{

using bytes::Bytes;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::flash::CanFlashUdsChannel;
using fastecu::flash::ScriptedCanFlashTransport;
using testing::ElementsAre;
using testing::HasSubstr;

constexpr std::uint32_t kRequestId = 0x7e0;
constexpr std::uint32_t kResponseId = 0x7e8;

TEST(CanFlashUdsChannelTest, PrependsTheRequestIdOnSend)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.expectWrite(Bytes{0x00, 0x00, 0x07, 0xE0, 0x10, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const fastecu::Status sent = channel.send(Bytes{0x10, 0x03}, cancellation);

    EXPECT_TRUE(sent.has_value());
    EXPECT_EQ(transport.writesConsumed(), 1u);
}

TEST(CanFlashUdsChannelTest, StripsTheReplyIdOnReceive)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07, 0xE8, 0x50, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(received->has_value());
    EXPECT_THAT(**received, ElementsAre(0x50, 0x03));
}

TEST(CanFlashUdsChannelTest, PassesATimeoutThroughAsAnEmptyOptional)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queue_no_frame();

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_TRUE(received.has_value());
    EXPECT_FALSE(received->has_value());
}

TEST(CanFlashUdsChannelTest, RejectsAFrameShorterThanTheEnvelope)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
}

TEST(CanFlashUdsChannelTest, RejectsAFrameFromAnUnexpectedReplyId)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x03});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(received.error().detail, HasSubstr("7e9"));
}

TEST(CanFlashUdsChannelTest, AcceptsAnEnvelopeOnlyFrameAsAnEmptyPdu)
{
    // A well-addressed frame carrying no PDU is not an envelope failure; the
    // client above classifies it as Malformed.
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queueRead(Bytes{0x00, 0x00, 0x07, 0xE8});

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_TRUE(received.has_value());
    ASSERT_TRUE(received->has_value());
    EXPECT_TRUE((*received)->empty());
}

TEST(CanFlashUdsChannelTest, PropagatesATransportError)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.queue_error(ErrorKind::Disconnected, "adapter closed");

    CanFlashUdsChannel channel(transport, kRequestId, kResponseId);
    const auto received = channel.receive(500, cancellation);

    ASSERT_FALSE(received.has_value());
    EXPECT_EQ(received.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(received.error().detail, "adapter closed");
}

} // namespace
```

- [ ] **Step 2: Add the BUILD targets**

In `src/backend/flash/BUILD.bazel`, after the `flash_executor` `cc_library` and its test, add:

```python
# Portable. Adapts the ISO-15765 flash transport to the PDU-level UDS channel:
# adds and strips the 4-byte CAN arbitration-id envelope so nothing above it
# indexes past a header.
cc_library(
    name = "can_flash_uds_channel",
    srcs = ["can_flash_uds_channel.cpp"],
    hdrs = ["can_flash_uds_channel.h"],
    deps = [
        ":flash_executor",
        "//src/algorithms/protocol",
        "//src/backend/ports",
        "//src/backend/protocol/uds:uds_client",
    ],
)

fastecu_portable_gtest(
    name = "can_flash_uds_channel_test",
    srcs = ["can_flash_uds_channel_test.cpp"],
    deps = [
        ":can_flash_uds_channel",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
    ],
)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/flash:can_flash_uds_channel_test`
Expected: FAIL — compile error, `can_flash_uds_channel.h` not found.

- [ ] **Step 4: Write the header**

Create `src/backend/flash/can_flash_uds_channel.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/iuds_channel.h"

namespace fastecu::flash
{

// Adds and strips the 4-byte big-endian CAN arbitration id that ISO-15765
// flash traffic carries in front of every UDS PDU.
//
// This is the only place in the CAN flash path that knows the envelope
// exists. Executors above it work in PDUs starting at the service id, which
// is why they no longer carry a `kServiceOffset = 4` constant.
//
// Non-owning: the caller keeps the transport alive for at least this
// channel's lifetime, and remains responsible for configure/open/close.
class CanFlashUdsChannel final : public uds::IUdsChannel
{
  public:
    // Number of envelope bytes in front of every PDU.
    static constexpr std::size_t kEnvelopeSize = 4;

    CanFlashUdsChannel(ICanFlashTransport& transport, std::uint32_t request_id,
                       std::uint32_t response_id);

    Status send(bytes::ByteView pdu, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> receive(int timeout_ms,
                                                const ICancellationToken& cancellation) override;

  private:
    ICanFlashTransport& transport_;
    std::uint32_t request_id_;
    std::uint32_t response_id_;
};

} // namespace fastecu::flash
```

- [ ] **Step 5: Write the implementation**

Create `src/backend/flash/can_flash_uds_channel.cpp`:

```cpp
#include "src/backend/flash/can_flash_uds_channel.h"

#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"

namespace fastecu::flash
{

CanFlashUdsChannel::CanFlashUdsChannel(ICanFlashTransport& transport, std::uint32_t request_id,
                                       std::uint32_t response_id)
    : transport_(transport), request_id_(request_id), response_id_(response_id)
{
}

Status CanFlashUdsChannel::send(bytes::ByteView pdu, const ICancellationToken& cancellation)
{
    return transport_.write(bytes::composeBe(request_id_, pdu), cancellation);
}

Result<std::optional<bytes::Bytes>> CanFlashUdsChannel::receive(
    int timeout_ms, const ICancellationToken& cancellation)
{
    Result<std::optional<bytes::Bytes>> frame = transport_.read(timeout_ms, cancellation);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    if (!frame->has_value())
    {
        return std::optional<bytes::Bytes>{};
    }

    const bytes::Bytes& raw = **frame;
    if (raw.size() < kEnvelopeSize)
    {
        return fail(ErrorKind::BadResponse,
                    std::format("CAN frame of {} bytes is shorter than its 4-byte id envelope",
                                raw.size()));
    }

    const std::uint32_t id = bytes::readU32Be(raw);
    if (id != response_id_)
    {
        return fail(ErrorKind::BadResponse,
                    std::format("expected CAN reply id 0x{:x}, got 0x{:x}", response_id_, id));
    }

    return std::optional<bytes::Bytes>(
        bytes::Bytes(raw.begin() + static_cast<std::ptrdiff_t>(kEnvelopeSize), raw.end()));
}

} // namespace fastecu::flash
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/flash:can_flash_uds_channel_test`
Expected: PASS, 7 tests.

- [ ] **Step 7: Register the new target with the portable-closure guard**

In `scripts/check-portable-closure.py`, add `"can_flash_uds_channel"` to the `src/backend/flash` set:

```python
    ROOT / "src/backend/flash": {
        "flash_types",
        "flash_plan",
        "flash_validation",
        "flash_executor",
        "flash_device_lookup",
        "can_flash_uds_channel",
    },
```

In root `BUILD.bazel`, add `"//src/backend/flash:can_flash_uds_channel"` to **both** the `expression` and `scope` lists of the `portable_backend_closure` genquery, immediately after `"//src/backend/flash:flash_device_lookup"` in each.

- [ ] **Step 8: Verify the guard passes**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS. `//src/backend/flash` is in `QT_FREE_PACKAGES`, so this also confirms the new targets introduced no Qt rule.

- [ ] **Step 9: Format and commit**

```bash
prek run --all-files
git add src/backend/flash BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat: adapt the ISO-15765 flash transport to the UDS channel port

CanFlashUdsChannel is the only place in the CAN flash path that knows the
4-byte arbitration-id envelope exists. Reply-id validation is new: the plan
has declared response_id since it was written, so checking it costs nothing
and turns a silent mis-parse into a typed error.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: Re-express the Colt builders over uds::buildRequest

**Files:**
- Modify: `src/algorithms/protocol/colt/mitsu_colt_can_protocol.cpp`
- Modify: `src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.cpp`
- Modify: `src/algorithms/protocol/colt/BUILD.bazel`

**Interfaces:**
- Consumes: `uds::buildRequest` (Task 1).
- Produces: no signature change. Every `MitsuColtCan::build*` and `MitsuColtCanVendorExt::build*` function keeps its exact name, parameters, and output bytes.

**This task must change zero bytes on the wire.** The existing byte-level tests in this package are the proof, and they are not edited.

- [ ] **Step 1: Record the current test state as the baseline**

Run: `bazel test --config=release //src/algorithms/protocol/colt:all`
Expected: PASS. These same tests must still pass unchanged at the end of this task; if any expectation needs editing, the re-expression changed a frame and is wrong.

- [ ] **Step 2: Add the dependency**

In `src/algorithms/protocol/colt/BUILD.bazel`, add `"//src/algorithms/protocol/uds"` to the `deps` of both `mitsu_colt_can_protocol` and `mitsu_colt_can_vendor_ext_protocol`:

```python
cc_library(
    name = "mitsu_colt_can_protocol",
    srcs = ["mitsu_colt_can_protocol.cpp"],
    hdrs = ["mitsu_colt_can_protocol.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/uds",
    ],
)

cc_library(
    name = "mitsu_colt_can_vendor_ext_protocol",
    srcs = ["mitsu_colt_can_vendor_ext_protocol.cpp"],
    hdrs = ["mitsu_colt_can_vendor_ext_protocol.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/uds",
    ],
)
```

- [ ] **Step 3: Re-express the builder bodies**

In `src/algorithms/protocol/colt/mitsu_colt_can_protocol.cpp`, add the include next to the existing `bytes_compose.h` one:

```cpp
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_pdu.h"
```

Then replace the builder bodies (currently at lines 766-821). The subfunction-only and data-only forms map directly onto an overload; the multi-field forms compose their data first and hand it to the `(sid, data)` overload, which costs one extra intermediate `Bytes` per call — acceptable on frames of at most 256 bytes built once per exchange, and the price of having a single generic entry point:

```cpp
bytes::Bytes buildRequestDownload(std::uint32_t start, std::uint32_t size)
{
    return uds::buildRequest(kServiceRequestDownload,
                             composeBe(u24(start), 0x00_b, u24(size)));
}

std::vector<bytes::Bytes> buildTransferDataFrames(bytes::ByteView payload)
{
    std::vector<bytes::Bytes> frames;
    for (std::size_t offset = 0; offset < payload.size(); offset += kTransferChunkSize)
    {
        const std::size_t chunkSize =
            std::min<std::size_t>(kTransferChunkSize, payload.size() - offset);
        frames.push_back(
            uds::buildRequest(kServiceTransferData, payload.subspan(offset, chunkSize)));
    }
    return frames;
}

bytes::Bytes buildRoutineCheckCrc(std::uint32_t targetStart)
{
    return uds::buildRequest(kServiceRoutineControl, kRoutineCheckCrc,
                             composeBe(targetStart < 0x800000 ? 2_b : 1_b));
}

bytes::Bytes buildRoutineErase()
{
    return uds::buildRequest(kServiceRoutineControl, kRoutineErase);
}

bytes::Bytes buildRequestReflashUnlock()
{
    // Verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181.
    // Original author's comment: "caused bootloader lockup". See header doc.
    static const bytes::Byte kData[11] = {154, 1, 1, 'R', 'c', 'u', 's', '0', '0', 0, 1};
    return uds::buildRequest(kServiceRequestReflash, bytes::ByteView(kData));
}

bytes::Bytes buildReadMemoryByAddress(std::uint32_t addr, bytes::Byte len)
{
    return uds::buildRequest(kServiceReadMemoryByAddress, composeBe(u24(addr), len));
}

bytes::Bytes buildDiagnosticSession(bytes::Byte sessionId)
{
    return uds::buildRequest(kServiceDiagnosticSession, sessionId);
}

bytes::Bytes buildSecurityAccessSeedRequest()
{
    return uds::buildRequest(kServiceSecurityAccess, 0x05_b);
}

bytes::Bytes buildSecurityAccessKey(bytes::ByteView key)
{
    assert(key.size() == 4);
    return uds::buildRequest(kServiceSecurityAccess, 0x06_b, key);
}
```

Note `buildRequestReflashUnlock`: the original 12-byte array led with `kServiceRequestReflash`. Moving the SID into `buildRequest` means the array drops that first element and shrinks to 11 — the emitted frame is unchanged. Verify this one against its test explicitly; it is the only builder where the array literal itself changes.

- [ ] **Step 4: Re-express the vendor-extension builders**

Read `src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.cpp` and apply the same transformation to each `build*` function: a frame of `composeBe(SID, x, y, ...)` becomes `uds::buildRequest(SID, composeBe(x, y, ...))`, and a frame of `composeBe(SID, subfunction)` becomes `uds::buildRequest(SID, subfunction)`. Add the same `uds_pdu.h` include. Change no constant and no value.

- [ ] **Step 5: Run the colt tests unedited**

Run: `bazel test --config=release //src/algorithms/protocol/colt:all`
Expected: PASS, with **no edits to any test file**. A failure here means a frame changed; fix the builder, never the expectation.

- [ ] **Step 6: Run every downstream consumer**

Run: `bazel test --config=release //src/backend/flash/ecu:all //src/backend/flash:all`
Expected: PASS. The Colt executor's byte-exact suite exercises these builders end to end and is the second, stronger proof that nothing moved.

- [ ] **Step 7: Format and commit**

```bash
prek run --all-files
git add src/algorithms/protocol/colt
git commit -m "refactor: build Colt CAN frames through uds::buildRequest

Bodies only -- every signature, constant, and emitted byte is unchanged, and
the existing byte-level colt tests pass without edits. Demonstrates the
generic PDU builder against a real family and leaves one entry point for
service-id composition.

buildRequestReflashUnlock's literal array drops its leading SID byte (12
elements to 11) because buildRequest now supplies it; the frame on the wire
is identical.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Retrofit the Colt CAN executor

**Files:**
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `uds::UdsClient`, `uds::ExchangePolicy` (Task 4); `fastecu::flash::CanFlashUdsChannel` (Task 5); `uds::payload`, `uds::subfunction`, `uds::positiveResponse` (Tasks 1-2).
- Produces: no public signature change. `MitsuColtM32rCanExecutor::execute` keeps its `IFlashExecutor` signature.

- [ ] **Step 1: Record the baseline**

Run: `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test`
Expected: PASS. Note the test count; it must not drop.

- [ ] **Step 2: Add the dependencies**

In `src/backend/flash/ecu/BUILD.bazel`, add two deps to the `mitsu_colt_m32r_can_executor` `cc_library`:

```python
        "//src/algorithms/protocol/uds",
        "//src/backend/flash:can_flash_uds_channel",
        "//src/backend/protocol/uds:uds_client",
```

- [ ] **Step 3: Delete the six local helpers**

In `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`, delete these, all in the anonymous namespace: `kServiceOffset` (around line 33), `positive()` (around line 38), `build_request()` (around line 124), `nrc_context()` (around line 136), `exchange()` (around line 147), and `service_is()` (around line 178). Add the new includes:

```cpp
#include "src/algorithms/protocol/uds/uds_pdu.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/protocol/uds/uds_client.h"
```

Add `uds::UdsClient& uds;` to `Ctx` and drop the now-unused `request_id` threading from every call site.

- [ ] **Step 4: Construct the channel and client in execute()**

In `MitsuColtM32rCanExecutor::execute`, after the transport is configured and opened and before the first exchange, construct both on the stack so they outlive every exchange:

```cpp
CanFlashUdsChannel channel(transport, plan_family.request_id, plan_family.response_id);
uds::UdsClient uds_client(channel, clock, events);
```

Use whatever local names the surrounding code already has for the plan family, transport, clock, and event sink; read the function before editing rather than assuming. Thread `uds_client` into the `Ctx` aggregate that the phase functions receive.

- [ ] **Step 5: Retrofit all 13 exchange sites**

The 13 `exchange(ctx, ...)` calls are at approximately lines 198, 215, 244, 290, 311, 356, 394, 412, 431, 448, 462, 493, and 504. Each becomes a `ctx.uds.request(...)` call whose fourth and fifth legacy arguments (delay, timeout) become policy fields. The first site is the worked example; apply the same shape to the rest:

```cpp
// Before
Result<bytes::Bytes> received =
    exchange(ctx, family.request_id, buildDiagnosticSession(kSessionBasic), 50,
             kReadTimeoutMs);
if (!received)
{
    return std::unexpected(received.error());
}
if (received->size() <= 5 ||
    !service_is(*received, positive(kServiceDiagnosticSession)) ||
    (*received)[5] != kSessionBasic)
{
    error(ctx, std::format("Wrong response from ECU: {}", nrc_context(*received)));
    return fail(ErrorKind::BadResponse, "basic diagnostic session rejected");
}

// After
Result<bytes::Bytes> received = ctx.uds.request(
    buildDiagnosticSession(kSessionBasic),
    {.pre_read_delay_ms = 50, .read_timeout_ms = kReadTimeoutMs}, ctx.cancellation);
if (!received.has_value())
{
    error(ctx, std::format("Wrong response from ECU: {}", received.error().detail));
    return std::unexpected(received.error());
}
if (uds::subfunction(*received) != kSessionBasic)
{
    error(ctx, "Wrong response from ECU: unexpected session id");
    return fail(ErrorKind::BadResponse, "basic diagnostic session rejected");
}
```

Three rules cover every site:

1. **Drop the service check entirely.** `UdsClient` validates the service echo, so `service_is(*received, positive(SID))` has no replacement. Seven of the thirteen sites checked *only* the service, and their whole validation block disappears.
2. **Convert offsets by subtracting 4.** The envelope is gone. Legacy `(*received)[5]` is `uds::subfunction(*received)`; legacy `(*received)[6]` is `uds::payload(*received)[1]`; legacy `received->subspan(6, 4)` is `uds::payload(*received).subspan(1, 4)`; legacy `received->subspan(7, 4)` is `uds::payload(*received).subspan(2, 4)`. Size guards shrink the same way: a legacy `received->size() <= 10` (that is, "require at least 11 raw bytes") becomes `uds::payload(*received).size() < 6`.
3. **Never set `pending_timeout_ms` or `max_pending_repeats`.** No site needs a bespoke value; the defaults (3000 ms, 10 repeats) are correct everywhere, including the erase.

All thirteen sites, in file order. `RD` = `.pre_read_delay_ms`, `TO` = `.read_timeout_ms`:

| # | Line | Request | RD | TO | Validation after the retrofit |
| --- | --- | --- | --- | --- | --- |
| 1 | 198 | `buildDiagnosticSession(kSessionBasic)` | 50 | `kReadTimeoutMs` | `uds::subfunction(*received) != kSessionBasic` |
| 2 | 215 | `MitsuColtCanVendorExt::buildChallengeSeedRequest()` | 200 | `kReadTimeoutMs` | `payload.size() < 6 \|\| payload[0] != kVendorChallengeSelector \|\| payload[1] != kVendorChallengeSeedSubfunction`; seed is `payload.subspan(2, 4)` |
| 3 | 244 | `MitsuColtCanVendorExt::buildChallengeKey(vendor_key)` | 200 | `kReadTimeoutMs` | `payload.size() < 2 \|\| payload[0] != kVendorChallengeSelector \|\| payload[1] != kVendorChallengeAccepted` |
| 4 | 290 | `buildSecurityAccessSeedRequest()` | 200 | `kReadTimeoutMs` | `payload.size() < 5 \|\| uds::subfunction(*received) != 5`; seed is `payload.subspan(1, 4)` |
| 5 | 311 | `buildSecurityAccessKey(key)` | 200 | `kReadTimeoutMs` | `uds::subfunction(*received) != 6` |
| 6 | 356 | `buildReadMemoryByAddress(addr, chunk_len)` | 50 | `kReadTimeoutMs` | `payload.size() < chunk_len`; the chunk is `payload.subspan(0, chunk_len)` |
| 7 | 394 | `buildRequestDownload(start, data.size())` | 50 | `kReadTimeoutMs` | none — service check only |
| 8 | 412 | `chunk` (a `buildTransferDataFrames` element) | 50 | `kReadTimeoutMs` | none |
| 9 | 431 | `buildRequestDownload(kCrcTransferAddress, kCrcTransferSize)` | 50 | `kReadTimeoutMs` | none |
| 10 | 448 | `buildTransferDataFrames(composeBe(crc)).front()` | 50 | `kReadTimeoutMs` | none |
| 11 | 462 | `buildRoutineCheckCrc(start)` | 200 | `kExtraLongTimeoutMs` | none |
| 12 | 493 | `buildRequestReflashUnlock()` | 200 | `kExtraLongTimeoutMs` | none |
| 13 | 504 | `buildRoutineErase()` | 200 | `kExtraLongTimeoutMs` | none |

Bind `payload` once per site (`const bytes::ByteView payload = uds::payload(*received);`) rather than calling the accessor repeatedly.

For the seven "none" sites, the entire block collapses to the error branch, which keeps its existing bespoke log wording and now interpolates the client's detail:

```cpp
// Site 7, before
Result<bytes::Bytes> received = exchange(ctx, family.request_id, ..., 50, kReadTimeoutMs);
if (!received)
{
    return std::unexpected(received.error());
}
if (!service_is(*received, positive(kServiceRequestDownload)))
{
    error(ctx, std::format("RequestDownload to 0x{:x} rejected: {}", start,
                           nrc_context(*received)));
    return fail(ErrorKind::BadResponse, "RequestDownload rejected");
}

// Site 7, after
Result<bytes::Bytes> received = ctx.uds.request(
    buildRequestDownload(start, static_cast<std::uint32_t>(data.size())),
    {.pre_read_delay_ms = 50, .read_timeout_ms = kReadTimeoutMs}, ctx.cancellation);
if (!received.has_value())
{
    error(ctx, std::format("RequestDownload to 0x{:x} rejected: {}", start,
                           received.error().detail));
    return std::unexpected(received.error());
}
```

Note the returned `Error` changes at these seven sites: the executor propagates the client's error verbatim instead of substituting its own `fail(ErrorKind::BadResponse, "RequestDownload rejected")`. The `ErrorKind` is unchanged for both the negative-response case (`BadResponse`) and the timeout case (`Timeout`, which the legacy `exchange()` already returned) — only `detail` differs, now carrying the NRC description instead of a fixed phrase.

- [ ] **Step 6: Convert the remaining operator-bool checks in this file**

Run: `grep -n "if (!received)\|if (!written)\|if (!slept)\|; ![a-z_]*)" src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`

Convert every hit to `.has_value()`. For an init-statement condition such as `if (Status written = ctx.transport.write(out, ctx.cancellation); !written)`, split it:

```cpp
const Status written = ctx.transport.write(out, ctx.cancellation);
if (!written.has_value())
```

- [ ] **Step 7: Build and fix compile errors**

Run: `bazel build --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor`
Expected: PASS. Resolve every error before running tests; a leftover reference to a deleted helper shows up here.

- [ ] **Step 8: Run the existing suite and update only the log expectations**

Run: `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test --test_output=errors`

Two categories of expectation may change, and only these two:

1. **Log-message expectations** at sites that previously hit a malformed or wrong-service frame, where `"Wrong response from ECU: Not a valid answer"` becomes the client's typed detail.
2. **`error().detail` expectations** at the seven service-check-only sites (7-13 in the table above), which now propagate the client's error instead of a fixed phrase. `error().kind` expectations must **not** change.

**If any `expectWrite` or `queueRead` expectation fails, stop.** Those are the wire bytes, and they are supposed to be unchanged. A failure there means the retrofit altered a frame — fix the code, never the script.

- [ ] **Step 9: Add the four new tests**

Append to `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`. All four use the vendor read plan, whose first exchange is the basic diagnostic session — the shortest script that reaches a real exchange. Note the suite name is `MitsuColtM32rCanExecutor` (no `Test` suffix) and log entries are `events.logs`, matching the file's existing tests.

```cpp
TEST(MitsuColtM32rCanExecutor, AbsorbsResponsePendingWithoutResending)
{
    // The safety property, end to end: 0x78 is absorbed by re-READING. If the
    // client re-sent instead, the scripted transport would see a third write
    // it has no expectation for and fail it.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x7f, 0x10, 0x78}));
    transport.queueRead(response({0x50, 0x81}));

    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queue_no_frame(); // stop here: pending absorption is what this pins

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_EQ(transport.writesConsumed(), 2u);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, FailsWhenTheEcuPendsPastTheRepeatLimit)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    // One normal read plus the default max_pending_repeats of 10.
    for (int i = 0; i < 11; ++i)
    {
        transport.queueRead(response({0x7f, 0x10, 0x78}));
    }

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_THAT(result.error().detail, HasSubstr("responsePending"));
    EXPECT_EQ(transport.writesConsumed(), 1u);
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, RejectsAResponseToADifferentService)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    // A SecurityAccess reply (0x67) to a DiagnosticSession request (0x10).
    transport.queueRead(response({0x67, 0x05}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(result.error().detail, HasSubstr("0x10"));
    EXPECT_THAT(result.error().detail, HasSubstr("0x27"));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, RejectsAFrameFromTheWrongReplyId)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    // Built inline: the response() helper hardcodes the plan's 0x7e8.
    bytes::Bytes wrong_id;
    bytes::appendU32Be(wrong_id, 0x7e9);
    wrong_id.insert(wrong_id.end(), {0x50, 0x81});
    transport.queueRead(wrong_id);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(result.error().detail, HasSubstr("7e9"));
    EXPECT_TRUE(transport.scriptConsumed());
}
```

`HasSubstr` may need adding to the file's `using testing::...` block; check before compiling.

Each of the four must fail if its behavior is removed. Verify at least the first by temporarily changing `UdsClient`'s pending branch to fall through to the negative-response case and confirming the test goes red.

- [ ] **Step 10: Run everything**

Run: `bazel test --config=release //...`
Expected: PASS across the repository, including `//:portable_closure`.

- [ ] **Step 11: Format and commit**

```bash
prek run --all-files
git add src/backend/flash/ecu
git commit -m "refactor: retrofit the Colt CAN executor onto the UDS layer

Deletes kServiceOffset, positive(), build_request(), nrc_context(),
exchange(), and service_is(); all 13 exchange sites now go through
UdsClient over CanFlashUdsChannel. Offsets are written against the UDS
frame layout instead of an envelope the reader had to remember.

The wire is unchanged: every expectWrite and queueRead in the existing suite
passes unedited. Behavior changes are the four the design records --
responsePending absorbed, typed detail for malformed/wrong-service frames,
reply-id validation, and a service echo check at every exchange.

Also converts this file's operator-bool Result checks to .has_value().

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 8: ADR, hardware documentation, and the follow-up issue

**Files:**
- Create: `docs/adr/0014-check-result-with-has-value.md`
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `docs/colt_czt_47110032_can_bench_checklist.md`

**Interfaces:**
- Consumes: nothing in code.
- Produces: documentation only. No target changes.

- [ ] **Step 1: Measure the current split, so the ADR states a fact**

Run:
```bash
grep -rn "has_value()" src/backend src/algorithms | wc -l
grep -rEn "if \(![a-z_]+\)|; ![a-z_]+\)" src/backend src/algorithms | wc -l
```
Expected: roughly 1200 and roughly 150. Use the numbers you actually get in the ADR text below.

- [ ] **Step 2: Write the ADR**

Create `docs/adr/0014-check-result-with-has-value.md`, matching the Status/Context/Decision/Consequences shape of the neighbouring ADRs:

```markdown
# ADR 0014: Check Result with has_value

## Status

Accepted

## Context

`fastecu::Result<T>` is `std::expected<T, Error>`, and `fastecu::Status` is
`Result<void>`. Both convert implicitly to `bool`, so a success check can be
written either as `if (!result.has_value())` or as `if (!result)`.

The codebase had already settled on the explicit form by a wide margin --
about <N> `.has_value()` uses across `src/backend` and `src/algorithms`
against about <M> operator-bool sites -- but nothing recorded the choice, so
new code drifted either way.

The implicit form is also ambiguous in a way that matters here. For a
`Result<bool>` and a `Result<std::optional<T>>`, `if (result)` reads as a
question about the contained value, not about success, and the two readings
disagree exactly when the contained value is falsy.

## Decision

Check `Result` and `Status` with `.has_value()`.

Do not use the implicit `operator bool`. Where an init-statement condition
would need it -- `if (Status s = f(); !s)` -- split it into a declaration and
a separate `if (!s.has_value())`.

## Consequences

Success checks read the same everywhere, and no reader has to know a type's
value category to know what is being tested. The remaining operator-bool
sites are a legible cleanup rather than an inconsistency; they are tracked in
a follow-up issue and converted opportunistically as files are touched, not
in one repo-wide diff.
```

Replace `<N>` and `<M>` with the counts from Step 1.

- [ ] **Step 3: Update the flash qualification matrix**

In `docs/flash-qualification-matrix.md`, in the `FlashEcuMitsuM32rCan` row's notes column, append:

```
Exchanges run through the shared UDS layer (`UdsClient` over
`CanFlashUdsChannel`): NRC 0x78 responsePending is absorbed by re-reading, up
to ten repeats, and the reply CAN id is validated. The pending-retry path is
unexercised on hardware — see the bench checklist.
```

Leave `hardware_status` as `experimental` and change no other column.

- [ ] **Step 4: Update the bench checklist**

In `docs/colt_czt_47110032_can_bench_checklist.md`, add a line to the appropriate verification section:

```markdown
- [ ] **responsePending (NRC 0x78) handling.** The erase and CRC-check
      routines are the exchanges most likely to make the ECU report
      responsePending. Confirm the operation completes rather than aborting,
      and that the debug log shows one "responsePending" line per absorbed
      reply. This path has no hardware evidence behind it; it was added with
      the UDS layer and is verified only by scripted tests.
```

Match the file's existing checklist item formatting; read the surrounding items before inserting.

- [ ] **Step 5: File the follow-up issue**

```bash
gh issue create --repo RcusStackwalker/FastECU \
  --title "Tech debt: convert remaining operator-bool Result checks to .has_value()" \
  --body "ADR 0014 records \`.has_value()\` as the way to check \`Result\`/\`Status\`. The UDS protocol layer work converted the sites in the files it touched; roughly 150 remain across \`src/backend\`, \`src/algorithms\`, \`src/platform\`, and \`src/ui\`.

Deliberately not done in one sweep: a 150-file diff would have buried the protocol change it came from. Convert opportunistically as files are touched, or in one focused PR of its own.

Find them with:

\`\`\`sh
grep -rEn 'if \(![a-z_]+\)|; ![a-z_]+\)' src
\`\`\`

Note the pattern also matches plain bool checks, so each hit needs a look rather than a blind rewrite."
```

- [ ] **Step 6: Verify links and formatting**

Run: `prek run --all-files`
Expected: PASS, including lychee on the new ADR and the two edited docs.

- [ ] **Step 7: Commit**

```bash
git add docs
git commit -m "docs: record ADR 0014 and the Colt pending-retry hardware gap

ADR 0014 writes down the .has_value() convention the codebase already
followed 8:1. The qualification matrix and bench checklist record that
responsePending handling reached the Colt CAN flash path with scripted
coverage only and no hardware evidence.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

- [ ] **Step 8: Final full verification**

Run: `bazel test --config=release //...`
Expected: PASS.

Run: `bazel run //:clang_tidy_report_changed`
Expected: no new findings against the changed files. Fix anything it reports before opening the pull request.

---

## Verification Summary

Before opening the pull request, all of the following must hold:

- [ ] `bazel test --config=release //...` passes.
- [ ] `prek run --all-files` passes.
- [ ] `bazel run //:clang_tidy_report_changed` reports nothing new.
- [ ] `//:portable_closure` passes with all three new packages registered.
- [ ] `mitsu_colt_m32r_can_executor_test` has **no edits to any `expectWrite` or `queueRead` script** — only log-message expectations changed, and four tests were added.
- [ ] `//src/algorithms/protocol/colt:all` passes with **no test-file edits at all**.
- [ ] No new `ErrorKind` value was added.
- [ ] No new operator-bool `Result` check was introduced.
