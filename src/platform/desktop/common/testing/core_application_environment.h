#pragma once

#include <QCoreApplication>
#include <gtest/gtest.h>
#include <functional>
#include <memory>

namespace fastecu::testing
{
// Qt retains argc/argv; both are owned until after application destruction.
// Register explicitly with AddGlobalTestEnvironment in each executable.
template <typename Application> class ApplicationEnvironment : public ::testing::Environment
{
  public:
    explicit ApplicationEnvironment(std::function<void()> before_construction = {})
        : before_construction_(std::move(before_construction))
    {
    }

    void SetUp() override
    {
        ASSERT_EQ(QCoreApplication::instance(), nullptr);
        if (before_construction_)
            before_construction_();
        argc_ = 1;
        argv_[0] = name_;
        argv_[1] = nullptr;
        application_ = std::make_unique<Application>(argc_, argv_);
    }

    void TearDown() override
    {
        application_.reset();
    }

  private:
    std::function<void()> before_construction_;
    int argc_ = 1;
    char name_[13] = "fastecu-test";
    char *argv_[2] = {name_, nullptr};
    std::unique_ptr<Application> application_;
};

using CoreApplicationEnvironment = ApplicationEnvironment<QCoreApplication>;
} // namespace fastecu::testing
