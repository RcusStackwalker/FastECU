#pragma once

#include <QCoreApplication>
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <memory>

namespace fastecu::testing
{
// Qt retains argc/argv; both are owned until after application destruction.
// Register explicitly with AddGlobalTestEnvironment in each executable.
template <typename Application> class ApplicationEnvironment : public ::testing::Environment
{
  public:
    explicit ApplicationEnvironment(std::function<void()> before_construction = {}, bool use_96_dpi = false)
        : before_construction_(std::move(before_construction)), use_96_dpi_(use_96_dpi)
    {
    }

    void SetUp() override
    {
        ASSERT_EQ(QCoreApplication::instance(), nullptr);
        if (before_construction_)
        {
            before_construction_();
        }
        argc_ = 1;
        argv_[0] = name_.data();
        argv_[1] = nullptr;
        application_ = std::make_unique<Application>(argc_, argv_.data());
        // Preserve the former Qt test main macros' post-construction DPI policy.
        if (use_96_dpi_)
        {
            QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true);
        }
    }

    void TearDown() override
    {
        application_.reset();
    }

  private:
    std::function<void()> before_construction_;
    bool use_96_dpi_ = false;
    int argc_ = 1;
    std::array<char, 13> name_ = {'f', 'a', 's', 't', 'e', 'c', 'u', '-', 't', 'e', 's', 't', '\0'};
    std::array<char *, 2> argv_ = {name_.data(), nullptr};
    std::unique_ptr<Application> application_;
};

using CoreApplicationEnvironment = ApplicationEnvironment<QCoreApplication>;
} // namespace fastecu::testing
