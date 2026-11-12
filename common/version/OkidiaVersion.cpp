// common/version/OkidiaVersion.cpp
#include "OkidiaVersion.h"

#ifndef OKIDIA_BUILD_VERSION
#define OKIDIA_BUILD_VERSION "0.1.0"
#endif

#ifndef OKIDIA_BUILD_OS_TAG
#define OKIDIA_BUILD_OS_TAG "linux"
#endif

namespace Okidia {
namespace Version {

const char* c_str() {
    return OKIDIA_BUILD_VERSION;
}

QString string() {
    return QString::fromUtf8(OKIDIA_BUILD_VERSION);
}

QString osTag() {
    return QString::fromUtf8(OKIDIA_BUILD_OS_TAG);
}

} // namespace Version
} // namespace Okidia
