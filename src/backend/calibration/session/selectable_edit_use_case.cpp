#include "src/backend/calibration/session/selectable_edit_use_case.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/definition/definition_model.h"

namespace fastecu::calibration
{
namespace
{

SelectableEditOutcome not_applicable(SelectableNotApplicableReason reason)
{
    return SelectableEditNotApplicable{.reason = reason};
}

// One byte of hex text; text that is not hexadecimal reads as zero.
std::uint8_t lenient_byte(std::string_view hex, std::size_t offset)
{
    const auto digits = hex.substr(std::min(offset, hex.size()), 2);
    unsigned value = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    return static_cast<std::uint8_t>(value);
}

} // namespace

Result<SelectableEditOutcome> apply_selectable_edit(CalibrationWorkspace& workspace,
                                                    const SelectableEditRequest& request)
{
    CalibrationSession *session = workspace.find(request.session);
    if (session == nullptr)
    {
        return not_applicable(SelectableNotApplicableReason::ClosedSession);
    }
    if (session->definition() == nullptr)
    {
        return not_applicable(SelectableNotApplicableReason::NoDefinition);
    }
    const auto& definition = session->definition()->definition;
    if (request.map_index >= definition.maps.size())
    {
        return not_applicable(SelectableNotApplicableReason::UnknownMap);
    }
    const auto& map = definition.maps[request.map_index];
    const auto *scaling = definition::find_scaling(definition, map.scaling_name);
    const auto storage = map.storage_type.has_value() ? map.storage_type
                         : scaling != nullptr         ? scaling->storage_type
                                                      : std::nullopt;
    if (scaling == nullptr || scaling->selections.empty() || storage != definition::StorageType::Bloblist)
    {
        return not_applicable(SelectableNotApplicableReason::NotBloblist);
    }
    const auto selected =
        std::ranges::find(scaling->selections, request.selection, &std::pair<std::string, std::string>::first);
    if (selected == scaling->selections.end())
    {
        return not_applicable(SelectableNotApplicableReason::UnknownSelection);
    }
    const auto width = element_byte_size(storage, scaling);
    bytes::Bytes data;
    data.reserve(width);
    for (std::uint32_t k = 0; k < width; ++k)
    {
        data.push_back(lenient_byte(selected->second, static_cast<std::size_t>(k) * 2));
    }
    const auto written = session->write_bytes(map.address.value_or(0), data);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return SelectableEditOutcome{SelectableEditChanged{}};
}

} // namespace fastecu::calibration
