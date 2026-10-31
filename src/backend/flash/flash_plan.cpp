#include "src/backend/flash/flash_plan.h"

#include <optional>
#include <utility>

namespace fastecu::flash
{

const bytes::Bytes& FlashPlan::ImageOrEmpty() const
{
    static const bytes::Bytes kNoImage;
    return fields_.image.has_value() ? *fields_.image : kNoImage;
}

const KernelImage& FlashPlan::KernelOrEmpty() const
{
    static const KernelImage kNoKernel{};
    return fields_.kernel.has_value() ? *fields_.kernel : kNoKernel;
}

std::string_view FlashPlan::ExperimentalFamilyId() const
{
    return FlashFamilyName(Family());
}

} // namespace fastecu::flash
