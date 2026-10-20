#include "apps/desktop/default_config_root.h"

#include <QDir>

QString DefaultConfigRoot()
{
    return QDir::homePath() + "/AppData/Local/FastECU/";
}
