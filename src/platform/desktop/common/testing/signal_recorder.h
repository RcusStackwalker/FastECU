#pragma once

#include <QObject>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <vector>

namespace fastecu::testing
{
template <typename Signal> class SignalRecorder;

template <typename Sender, typename... Args> class SignalRecorder<void (Sender::*)(Args...)>
{
  public:
    using Record = std::tuple<std::decay_t<Args>...>;

    SignalRecorder(Sender *sender, void (Sender::*signal)(Args...)) : storage_(std::make_shared<Storage>())
    {
        connection_ = QObject::connect(
            sender, signal, &context_,
            [storage = storage_](Args... args)
            {
                const std::lock_guard lock(storage->mutex);
                storage->records.emplace_back(args...);
            },
            Qt::DirectConnection);
    }
    ~SignalRecorder()
    {
        QObject::disconnect(connection_);
    }
    SignalRecorder(const SignalRecorder&) = delete;
    SignalRecorder& operator=(const SignalRecorder&) = delete;

    bool is_valid() const
    {
        return static_cast<bool>(connection_);
    }
    std::size_t count() const
    {
        const std::lock_guard lock(storage_->mutex);
        return storage_->records.size();
    }
    std::vector<Record> snapshot() const
    {
        const std::lock_guard lock(storage_->mutex);
        return storage_->records;
    }

  private:
    struct Storage
    {
        std::mutex mutex;
        std::vector<Record> records;
    };
    QObject context_;
    std::shared_ptr<Storage> storage_;
    QMetaObject::Connection connection_;
};

template <typename Sender, typename Owner, typename... Args>
SignalRecorder(Sender *, void (Owner::*)(Args...)) -> SignalRecorder<void (Owner::*)(Args...)>;
} // namespace fastecu::testing
