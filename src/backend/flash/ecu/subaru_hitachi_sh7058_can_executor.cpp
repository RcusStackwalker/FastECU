#include "src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.h"

#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using bytes::Bytes;
constexpr std::array<std::uint16_t, 16> kSeedTable{0x90A1, 0x2F92, 0xDE3C, 0xCDC0, 0x1A99, 0x437C, 0xF91B, 0xDB57,
                                                   0x96BA, 0xDE10, 0xFCAF, 0x3F31, 0xF47F, 0x0BB6, 0x16E9, 0x4645};
constexpr std::array<std::uint16_t, 4> kPayloadTable{0x14CA, 0x77F4, 0x973C, 0xF50E};

class Session
{
  public:
    Session(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancel, IEventSink& events)
        : transport_(transport), clock_(clock), cancel_(cancel), events_(events)
    {
    }
    Status checkpoint() const
    {
        return cancel_.cancelled() ? fail(ErrorKind::Cancelled, "SH7058 write cancelled") : Status{};
    }
    Result<std::optional<Bytes>> exchange(bytes::ByteView payload, std::chrono::milliseconds delay = 0ms,
                                          std::chrono::milliseconds timeout = 200ms, std::uint32_t id = 0x7e0)
    {
        if (auto status = checkpoint(); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        Bytes frame = bytes::composeBe(id, payload);
        if (auto sent = transport_.write(frame, cancel_); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }
        if (delay > 0ms)
        {
            if (auto slept = clock_.sleep(delay, cancel_); !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
        }
        if (auto status = checkpoint(); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        auto reply = transport_.read(timeout, cancel_);
        if (!reply.has_value())
        {
            if (reply.error().kind == ErrorKind::Timeout)
            {
                return std::optional<Bytes>{};
            }
            return std::unexpected(reply.error());
        }
        if (auto status = checkpoint(); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        return std::move(*reply);
    }
    static bool prefix(const std::optional<Bytes>& reply, std::initializer_list<bytes::Byte> expected)
    {
        return reply.has_value() && reply->size() >= expected.size() + 4 && (*reply)[0] == 0 && (*reply)[1] == 0 &&
               (*reply)[2] == 7 && (*reply)[3] == 0xe8 &&
               std::equal(expected.begin(), expected.end(), reply->begin() + 4);
    }
    Status require(bytes::ByteView payload, std::initializer_list<bytes::Byte> expected,
                   std::chrono::milliseconds delay = 200ms, std::uint32_t id = 0x7e0)
    {
        auto reply = exchange(payload, delay, 200ms, id);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        return prefix(*reply, expected) ? Status{} : fail(ErrorKind::BadResponse, "unexpected SH7058 CAN reply");
    }
    Status present(bytes::ByteView payload, std::uint32_t id = 0x7e0)
    {
        auto reply = exchange(payload, 0ms, 200ms, id);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        return reply->has_value() && (*reply)->size() > 4 && (**reply)[4] != 0x7f
                   ? Status{}
                   : fail(ErrorKind::BadResponse, "missing SH7058 CAN reply");
    }
    Status security()
    {
        auto seed = exchange(Bytes{0x27, 0x01}, 200ms);
        if (!seed.has_value())
        {
            return std::unexpected(seed.error());
        }
        if (!prefix(*seed, {0x67, 0x01}) || (*seed)->size() < 10)
        {
            return fail(ErrorKind::BadResponse, "invalid SH7058 seed");
        }
        Bytes key = SsmProtocol::calculateSeedKey(bytes::ByteView{**seed}.subspan(6, 4), kSeedTable,
                                                  SsmProtocol::kIndexTransformationStock);
        Bytes request{0x27, 0x02};
        request.insert(request.end(), key.begin(), key.end());
        return require(request, {0x67, 0x02});
    }
    Status connect()
    {
        auto active = exchange(Bytes{0xb7}, 50ms);
        if (!active.has_value())
        {
            return std::unexpected(active.error());
        }
        if (!active->has_value())
        {
            return fail(ErrorKind::Timeout, "SH7058 kernel probe timed out");
        }
        if (prefix(*active, {0x7f, 0xb7, 0x13}))
        {
            return {};
        }
        if ((*active)->size() < 5)
        {
            return fail(ErrorKind::BadResponse, "invalid SH7058 kernel probe");
        }
        if (auto status = require(Bytes{0xaa}, {0xea}, 50ms); !status.has_value())
        {
            return status;
        }
        if (auto status = require(Bytes{0x09, 0x02}, {0x49, 0x02}, 50ms); !status.has_value())
        {
            return status;
        }
        if (auto status = require(Bytes{0x09, 0x04}, {0x49, 0x04}, 50ms); !status.has_value())
        {
            return status;
        }
        if (auto status = require(Bytes{0x09, 0x06}, {0x49, 0x06}, 50ms); !status.has_value())
        {
            return status;
        }
        auto mode = exchange(Bytes{0xa8, 0, 0, 0, 0xd7});
        if (!mode.has_value())
        {
            return std::unexpected(mode.error());
        }
        if (!mode->has_value() || (*mode)->size() < 6)
        {
            return fail(ErrorKind::BadResponse, "SH7058 programming mode absent");
        }
        if (auto slept = clock_.sleep(777ms, cancel_); !slept.has_value())
        {
            return slept;
        }
        if ((**mode)[5] == 0xa0 || (**mode)[5] == 0x20)
        {
            if (auto status = require(Bytes{0x10, 0x43}, {0x50, 0x43}); !status.has_value())
            {
                return status;
            }
            if (auto status = security(); !status.has_value())
            {
                return status;
            }
            if (auto status = require(Bytes{0x10, 0x42}, {0x50, 0x42}); !status.has_value())
            {
                return status;
            }
        }
        else
        {
            struct Step
            {
                std::uint32_t id;
                Bytes request;
            };
            const std::array<Step, 9> access{{
                {0x7e0, {0xa8, 0, 0, 1, 0x3b}},
                {0x7df, {0x10, 0x03}},
                {0x7e1, {0x04}},
                {0x7b0, {0x10, 0x03}},
                {0x7b0, {0x85, 0x02}},
                {0x7df, {0x85, 0x02}},
                {0x7b0, {0x85, 0x02}},
                {0x7df, {0x85, 0x02}},
                {0x7df, {0x28, 0x03, 0x01}},
            }};
            for (const auto& step : access)
            {
                if (auto status = present(step.request, step.id); !status.has_value())
                {
                    return status;
                }
            }
            if (auto status = security(); !status.has_value())
            {
                return status;
            }
            for (const Bytes& request : {Bytes{0xa8, 0, 0, 0, 0xd5}, Bytes{0xa8, 0, 0, 1, 0x3b},
                                         Bytes{0xa8, 0, 0, 0, 0x1c}, Bytes{0xa8, 0, 0, 0, 0x0e, 0, 0, 0x0f}})
            {
                if (auto status = present(request); !status.has_value())
                {
                    return status;
                }
            }
            if (auto status = require(Bytes{0x10, 0x02}, {0x50, 0x02}); !status.has_value())
            {
                return status;
            }
        }
        return require(Bytes{0x34, 0x04, 0x33, 0, 0, 0, 0x10, 0, 0}, {0x74, 0x20, 0x01, 0x04});
    }
    Status erase()
    {
        auto reply = exchange(Bytes{0x31, 0x01, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff});
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (prefix(*reply, {0x71, 0x01, 0x02}))
        {
            return {};
        }
        for (int count = 0; count < 20; ++count)
        {
            if (auto status = checkpoint(); !status.has_value())
            {
                return status;
            }
            reply = transport_.read(800ms, cancel_);
            if (!reply.has_value())
            {
                return std::unexpected(reply.error());
            }
            if (prefix(*reply, {0x71, 0x01, 0x02}))
            {
                return {};
            }
        }
        return fail(ErrorKind::BadResponse, "SH7058 erase was not acknowledged");
    }
    Status retry(bytes::ByteView request, std::initializer_list<bytes::Byte> expected, int attempts)
    {
        for (int count = 0; count < attempts; ++count)
        {
            auto reply = exchange(request, 0ms, 800ms);
            if (!reply.has_value())
            {
                return std::unexpected(reply.error());
            }
            if (prefix(*reply, expected))
            {
                return {};
            }
        }
        return fail(ErrorKind::BadResponse, "SH7058 retry limit exceeded");
    }
    Status program(bytes::ByteView encrypted)
    {
        const Bytes window{0x34, 0x04, 0x33, 0, 0, 0, 0x10, 0, 0};
        if (auto status = retry(window, {0x74}, 6); !status.has_value())
        {
            return status;
        }
        for (std::uint32_t address = 0; address < 0x100000; address += 0x100)
        {
            const Bytes request =
                bytes::composeBe(bytes::Byte{0xb6}, bytes::u24(address), encrypted.subspan(address, 0x100));
            auto reply = exchange(request, 0ms, 5000ms);
            if (!reply.has_value())
            {
                return std::unexpected(reply.error());
            }
            if (!prefix(*reply, {0xf6}))
            {
                return fail(ErrorKind::BadResponse, "SH7058 write frame rejected");
            }
            events_.progress(static_cast<int>(address + 0x100), 0x100000);
        }
        if (auto status = retry(Bytes{0x37}, {0x77}, 20); !status.has_value())
        {
            return status;
        }
        if (auto slept = clock_.sleep(100ms, cancel_); !slept.has_value())
        {
            return slept;
        }
        return retry(Bytes{0x31, 0x01, 0x02, 0x02, 0x01}, {0x71, 0x01, 0x02}, 20);
    }

  private:
    ICanFlashTransport& transport_;
    IClock& clock_;
    const ICancellationToken& cancel_;
    IEventSink& events_;
};
} // namespace
Result<Iso15765Config> SubaruHitachiSh7058CanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (auto valid = validate_subaru_hitachi_sh7058_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (plan.operation() != FlashOperation::Write)
    {
        return fail(ErrorKind::Unsupported, "SH7058 CAN supports write only");
    }
    return iso15765_config_from(std::get<SubaruHitachiSh7058CanPlan>(plan.family_plan()));
}

Result<FlashExecutionResult> SubaruHitachiSh7058CanExecutor::execute(const FlashPlan& plan,
                                                                     ICanFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancel,
                                                                     IEventSink& events)
{
    if (auto setup = transport_setup(plan); !setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    Session session(transport, clock, cancel, events);
    if (auto status = session.connect(); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    const Bytes encrypted =
        SsmProtocol::calculatePayload(*plan.image(), 0x100000, kPayloadTable, SsmProtocol::kIndexTransformationStock);
    if (auto status = session.erase(); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = session.program(encrypted); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return FlashExecutionResult{FlashOperation::Write, std::nullopt, std::nullopt};
}
} // namespace fastecu::flash
