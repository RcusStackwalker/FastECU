#include "src/backend/calibration/session/selectable_edit_use_case.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/definition/text_format.h"

namespace fastecu::calibration
{
namespace
{

SelectableEditOutcome not_applicable(SelectableNotApplicableReason reason)
{
    return SelectableEditNotApplicable{.reason = reason};
}

// The selection's bytes. The value must be whole hexadecimal bytes and exactly
// as wide as the blob.
Result<bytes::Bytes> selection_bytes(std::string_view hex, std::uint32_t width)
{
    if (hex.size() % 2 != 0)
    {
        return fail(ErrorKind::InvalidConfig, "selection hex value has an incomplete byte");
    }
    if (hex.size() / 2 != width)
    {
        return fail(ErrorKind::InvalidConfig, "selection value width differs from the blob width");
    }
    bytes::Bytes data(width, 0);
    for (std::size_t k = 0; k < data.size(); ++k)
    {
        const auto byte = definition::parse_hex_value(hex.substr(k * 2, 2));
        if (!byte.has_value())
        {
            return fail(ErrorKind::InvalidConfig, "selection value is not hexadecimal bytes");
        }
        data[k] = static_cast<std::uint8_t>(*byte);
    }
    return data;
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
    const auto data = selection_bytes(selected->second, element_byte_size(storage, scaling));
    if (!data.has_value())
    {
        return std::unexpected(data.error());
    }
    const auto offset = map.address.value_or(0);
    const auto image = session->rom();
    if (offset <= image.size() && data->size() <= image.size() - offset &&
        std::ranges::equal(*data, image.subspan(static_cast<std::size_t>(offset), data->size())))
    {
        return SelectableEditOutcome{SelectableEditUnchanged{}};
    }
    const auto written = session->write_bytes(offset, *data);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return SelectableEditOutcome{SelectableEditChanged{}};
}

} // namespace fastecu::calibration
