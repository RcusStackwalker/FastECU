#pragma once
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "src/backend/config/config_paths.h"
#include "src/backend/config/protocol_catalog.h"
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

struct ResolvedCarModel
{
    std::string make, model, version, type, kw, hp, fuel, year, protocol_name;
    std::optional<ProtocolEntry> protocol; // nullopt: protocol_name had no match
};

// One row per CarModelEntry, in file order, each joined against the first
// ProtocolEntry with a matching protocol_name.
//
// First-match is unambiguous because protocol names are unique:
// load_protocol_catalog rejects a duplicate outright, so a catalog that came
// from there can contain at most one match. Passing a hand-built catalog with
// duplicates is a programming error; the first entry wins by definition
// rather than by any considered tie-break rule.
std::vector<ResolvedCarModel> resolve_car_models(const ProtocolCatalog& protocols,
                                                 const CarModelCatalog& car_models);

// The index of the LAST row (in resolve_car_models order) whose protocol_name
// equals flash_method; nullopt if none match. Mirrors
// FileActions::open_subaru_rom_file's own no-break scan over
// flash_protocol_protocol_name.
//
// Last-match here, unlike resolve_car_models above, because duplicates on
// this side are legitimate and common: several car models routinely share
// one protocol (in the shipped protocols.cfg, 63 <car_model> elements
// reference only 49 distinct protocols -- sub_ecu_denso_mc68hc16y5_02 alone
// is referenced by 6). They cannot be rejected at intake, and which row wins
// is observable: open_subaru_rom_file binds
// flash_protocol_selected_{make,model,version} from it. Switching to
// first-match would change the vehicle shown for every shared protocol.
std::optional<std::size_t> find_car_model_by_protocol_name(
    std::span<const ResolvedCarModel> resolved_car_models, std::string_view flash_method);

} // namespace fastecu::config
