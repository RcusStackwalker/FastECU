#pragma once
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/wire_transcript.h"
#include "src/backend/ports/clock.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace fastecu::flash
{

// Wraps a scripted transport and a clock; records every Write/Read/Sleep as a
// WireTranscript. Lifecycle calls are forwarded and not recorded.
class RecordingTranscriptCanFlashTransport final : public ICanFlashTransport, public IClock
{
  public:
    RecordingTranscriptCanFlashTransport(ScriptedCanFlashTransport& inner, IClock& clock) : inner_(inner), clock_(clock)
    {
    }

    const WireTranscript& Transcript() const
    {
        return transcript_;
    }

    Status ResetConnection() override
    {
        return inner_.ResetConnection();
    }
    Status Configure(const Iso15765Config& config) override
    {
        return inner_.Configure(config);
    }
    Status Open() override
    {
        return inner_.Open();
    }
    Status Close() override
    {
        return inner_.Close();
    }
    void RequestUnblock() noexcept override
    {
        inner_.RequestUnblock();
    }

    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        const auto& sections = inner_.Sections();
        const std::size_t index = inner_.WritesConsumed();
        const std::string label = index < sections.size() ? sections[index] : std::string{};
        Status status = inner_.Write(data, cancellation);
        if (!status)
        {
            return status;
        }
        if (label != last_section_)
        {
            Add(WireTranscriptStep::Kind::kSection, {}, {}, label);
            last_section_ = label;
        }
        Add(WireTranscriptStep::Kind::kWrite, bytes::Bytes(data.begin(), data.end()));
        return status;
    }

    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        auto result = inner_.Read(timeout, cancellation);
        if (!result)
        {
            return result;
        }
        if (timeout != last_timeout_)
        {
            Add(WireTranscriptStep::Kind::kReadTimeout, {}, timeout);
            last_timeout_ = timeout;
        }
        if (result->has_value())
        {
            Add(WireTranscriptStep::Kind::kRead, **result);
        }
        else
        {
            Add(WireTranscriptStep::Kind::kNoFrame);
        }
        return result;
    }

    std::chrono::steady_clock::time_point Now() const override
    {
        return clock_.Now();
    }

    Status Sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
    {
        Status status = clock_.Sleep(duration, cancellation);
        if (status)
        {
            Add(WireTranscriptStep::Kind::kSleep, {}, duration);
        }
        return status;
    }

  private:
    void Add(WireTranscriptStep::Kind kind, bytes::Bytes frame = {}, std::chrono::milliseconds duration = {},
             std::string label = {})
    {
        transcript_.push_back(WireTranscriptStep{kind, std::move(frame), duration, std::move(label)});
    }

    ScriptedCanFlashTransport& inner_;
    IClock& clock_;
    WireTranscript transcript_;
    std::string last_section_;
    std::optional<std::chrono::milliseconds> last_timeout_;
};

// Replays a transcript; any deviation fails with kInternal.
class ReplayCanFlashTransport final : public ICanFlashTransport, public IClock
{
  public:
    explicit ReplayCanFlashTransport(WireTranscript transcript) : transcript_(std::move(transcript))
    {
    }

    // True once every non-section step has been consumed.
    bool Consumed()
    {
        SkipSections();
        return index_ >= transcript_.size();
    }

    Status ResetConnection() override
    {
        return {};
    }
    Status Configure(const Iso15765Config&) override
    {
        return {};
    }
    Status Open() override
    {
        return {};
    }
    Status Close() override
    {
        return {};
    }
    void RequestUnblock() noexcept override
    {
    }

    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "replayed CAN write cancelled");
        }
        SkipSections();
        const auto *step = Peek();
        if (step == nullptr || step->kind != WireTranscriptStep::Kind::kWrite)
        {
            return Mismatch(std::format("write {}", bytes::ToHex(data)), step);
        }
        if (!std::equal(step->frame.begin(), step->frame.end(), data.begin(), data.end()))
        {
            return Mismatch(std::format("write {}", bytes::ToHex(data)), step);
        }
        ++index_;
        return {};
    }

    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "replayed CAN read cancelled");
        }
        SkipSections();
        if (const auto *step = Peek(); step != nullptr && step->kind == WireTranscriptStep::Kind::kReadTimeout)
        {
            last_timeout_ = step->duration;
            ++index_;
        }
        if (last_timeout_ != timeout)
        {
            return Mismatch(std::format("read timeout {} ms", timeout.count()), Peek(),
                            std::format("recorded timeout {}", DescribeTimeout()));
        }
        const auto *step = Peek();
        if (step != nullptr && step->kind == WireTranscriptStep::Kind::kNoFrame)
        {
            ++index_;
            return std::optional<bytes::Bytes>{};
        }
        if (step != nullptr && step->kind == WireTranscriptStep::Kind::kRead)
        {
            ++index_;
            return std::optional<bytes::Bytes>{step->frame};
        }
        return Mismatch(std::format("read ({} ms)", timeout.count()), step);
    }

    std::chrono::steady_clock::time_point Now() const override
    {
        return std::chrono::steady_clock::time_point{} + elapsed_;
    }

    Status Sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
    {
        SkipSections();
        const auto *step = Peek();
        if (step == nullptr || step->kind != WireTranscriptStep::Kind::kSleep || step->duration != duration)
        {
            return Mismatch(std::format("sleep {} ms", duration.count()), step);
        }
        ++index_;
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled);
        }
        elapsed_ += duration < std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero() : duration;
        return {};
    }

  private:
    void SkipSections()
    {
        while (index_ < transcript_.size() && transcript_[index_].kind == WireTranscriptStep::Kind::kSection)
        {
            section_ = transcript_[index_].label;
            ++index_;
        }
    }

    const WireTranscriptStep *Peek() const
    {
        return index_ < transcript_.size() ? &transcript_[index_] : nullptr;
    }

    std::string DescribeTimeout() const
    {
        return last_timeout_ ? std::format("{} ms", last_timeout_->count()) : std::string{"none"};
    }

    static std::string Describe(const WireTranscriptStep *step)
    {
        if (step == nullptr)
        {
            return "end of transcript";
        }
        switch (step->kind)
        {
        case WireTranscriptStep::Kind::kWrite:
            return std::format("write {}", bytes::ToHex(step->frame));
        case WireTranscriptStep::Kind::kRead:
            return std::format("read {}", bytes::ToHex(step->frame));
        case WireTranscriptStep::Kind::kNoFrame:
            return "no frame";
        case WireTranscriptStep::Kind::kReadTimeout:
            return std::format("read timeout {} ms", step->duration.count());
        case WireTranscriptStep::Kind::kSleep:
            return std::format("sleep {} ms", step->duration.count());
        case WireTranscriptStep::Kind::kSection:
            return std::format("section {}", step->label);
        }
        return "unknown step";
    }

    std::unexpected<Error> Mismatch(const std::string& actual, const WireTranscriptStep *step,
                                    const std::string& note = {}) const
    {
        const std::string expected = note.empty() ? Describe(step) : std::format("{} ({})", Describe(step), note);
        return Fail(
            ErrorKind::kInternal,
            std::format("replayed CAN transcript diverged at step {} (section {})\n  expected: {}\n  actual:   {}",
                        index_, section_, expected, actual));
    }

    WireTranscript transcript_;
    std::size_t index_ = 0;
    std::string section_;
    std::optional<std::chrono::milliseconds> last_timeout_;
    std::chrono::milliseconds elapsed_{0};
};

} // namespace fastecu::flash
