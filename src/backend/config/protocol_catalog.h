#pragma once
#include <string>
#include <vector>
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// One <protocol> element from protocols.cfg. Deliberately has no `id`
// field: read_protocols_file (file_actions.cpp:1089-1393) never populates a
// protocol-level id. The only "flash_protocol_id" it produces is a
// sequential counter assigned per <car_model> (file_actions.cpp:1272,
// `QString::number(id)` where `id` increments once per car_model, not per
// protocol) -- it belongs to the separate car_model/vehicle catalog that
// function also builds by cross-referencing car_models against this
// protocol table, which is out of scope for this task (see
// Consumes/Produces in the plan: ConfigPaths + IFileRepository + pugixml ->
// ProtocolCatalog, scoped to <protocols>/<protocol> only).
struct ProtocolEntry
{
    std::string protocol_name; // <protocol name="...">, "No name" if absent
    std::string alias;         // <protocol alias="...">, "No alias" if absent
                               // (matches legacy read_protocols_file's Qt
                               // QDomElement::attribute(name, default)
                               // defaulting -- not empty string)
    std::string ecu;
    std::string mcu;
    std::string mode;
    // Raw text, not bool: legacy read_protocols_file stores whatever
    // protocol_data.text() returns verbatim (file_actions.cpp), and real
    // shipped protocols.cfg entries use "n/a" as well as "yes"/"no" (e.g.
    // sub_ecu_mitsu_m32r_kline's <checksum>n/a</checksum>). A bool
    // representation collapses "n/a" into "no", which is observable: legacy
    // branches on "n/a" specifically at file_actions.cpp:1795 (RomInfo text)
    // and file_actions.cpp:2347 (whether the checksum-module-missing warning
    // dialog fires at flash time) -- collapsing to "no" silently suppresses
    // that warning for real M32R K-Line and CVT-CAN targets.
    std::string checksum;
    std::string read;
    std::string test_write;
    std::string write;
    std::string flash_transport;
    std::string log_transport;
    std::string log_protocol;
    std::string ecu_id_ascii;
    std::string ecu_id_addr;
    std::string ecu_id_length;
    std::string cal_id_ascii;
    std::string cal_id_addr;
    std::string cal_id_length;
    std::string kernel;
    std::string kernel_addr;
    std::string description;
};

using ProtocolCatalog = std::vector<ProtocolEntry>;

// Replaces FileActions::read_protocols_file's parsing of the <protocols>
// section of protocols.cfg into a directly queryable catalog of <protocol>
// entries (the function's car_models cross-referencing is out of scope
// here; see ProtocolEntry's comment above).
Result<ProtocolCatalog> load_protocol_catalog(const ConfigPaths& paths, IFileRepository& file_repository);

} // namespace fastecu::config
