#include "app/icon.h"

namespace moyou::app {

QIcon appIcon() {
    QIcon icon;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 128, 256}) icon.addFile(QStringLiteral(":/moyou/icons/moyou-%1.png").arg(s));
    return icon;
}

}  // namespace moyou::app
