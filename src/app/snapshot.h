#pragma once

#include <QStringList>

namespace moyou::app {
int snapshot(const QStringList &args);   // headless render to PNG (moyou --snapshot ...)
}
