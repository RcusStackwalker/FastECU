#include "apps/desktop/default_config_root.h"

#include <QDir>

QString default_config_root()
{
    return QDir::homePath() + "/AppData/Local/FastECU/";
}
