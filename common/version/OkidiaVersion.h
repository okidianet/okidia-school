// common/version/OkidiaVersion.h
#pragma once

#include <QString>

namespace Okidia {
namespace Version {

/**
 * @brief Okidia sürüm metnini C stringi (const char*) olarak döndürür.
 * Protobuf, loglar veya düşük seviyeli C API'leri için uygundur.
 */
const char* c_str();

/**
 * @brief Okidia sürüm metnini QString nesnesi olarak döndürür.
 */
QString string();
inline QString qstr() { return string(); }

/**
 * @brief Hedef işletim sistemi etiketini (örn: "ubuntu24", "windows") döndürür.
 */
QString osTag();

} // namespace Version
} // namespace Okidia

#ifndef OKIDIA_VERSION
#define OKIDIA_VERSION Okidia::Version::c_str()
#endif

#ifndef OKIDIA_OS_TAG
#define OKIDIA_OS_TAG Okidia::Version::osTag()
#endif
