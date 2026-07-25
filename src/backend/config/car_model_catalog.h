#pragma once
#include <string>
#include <vector>
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// One <car_model> element from protocols.cfg's <car_models> section (a
// sibling of <protocols>, not nested inside it -- see
// file_actions.cpp:1262-1379 in the legacy read_protocols_file for the
// section this replaces). Deliberately has no `id` field: legacy assigns
// that as a sequential loop counter over car_models only
// (file_actions.cpp:1265/1272, `id++` once per <car_model>, unrelated to
// anything in the XML) -- a list-position artifact of flattening into the
// legacy struct, not real domain data, so it is assigned by the adapter at
// that point instead of stored here.
//
// `protocol_name` is a cross-reference key (the <protocol> child's TEXT
// content) into ProtocolCatalog::protocol_name -- resolving that join is
// also the adapter's job (matching where the existing
// copy_protocol_catalog_into_legacy denormalization-for-legacy-shape
// already lives), not this value model's: some real car_models reference a
// protocol name absent from the shipped <protocols> section (see
// car_model_catalog_test.cpp), so this field is left unresolved here by
// design.
struct CarModelEntry
{
    std::string make;
    std::string model;
    std::string version;
    std::string type;
    std::string kw;
    std::string hp;
    std::string fuel;
    std::string year;
    std::string protocol_name;
};

using CarModelCatalog = std::vector<CarModelEntry>;

// Replaces FileActions::read_protocols_file's parsing of the <car_models>
// section of protocols.cfg into a directly queryable catalog of
// <car_model> entries (the cross-reference join against ProtocolCatalog is
// out of scope here; see CarModelEntry's comment above).
Result<CarModelCatalog> load_car_model_catalog(const ConfigPaths& paths, IFileRepository& file_repository);

} // namespace fastecu::config
