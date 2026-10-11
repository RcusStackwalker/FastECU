#pragma once
#include "src/backend/config/catalog.h"

namespace fastecu::config
{

// The protocols and vehicles this build supports. Only the desktop
// composition root hands it on; everything else is given a Catalog.
const Catalog& BuiltinCatalog();

} // namespace fastecu::config
