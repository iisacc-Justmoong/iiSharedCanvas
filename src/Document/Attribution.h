#pragma once
#include <string>
#include <string_view>
#ifdef IISHAREDCANVAS_NATIVE_API
#include <iiFileProviderNative.h>
#else
#include <iiFileProvider.h>
#endif

namespace iiSharedCanvas {
// Native and legacy packages are distinct ABI variants. A final executable must
// choose one; the native target never exposes a Qt metadata value.
#ifdef IISHAREDCANVAS_NATIVE_API
using DocumentAuthorship=iiFileProvider::native::Authorship;
using DocumentFileAuthor=iiFileProvider::native::FileAuthor;
inline std::string authorshipDump(const DocumentAuthorship& value) {return value.dump();}
inline auto authorshipFromDump(std::string_view value) {return DocumentAuthorship::fromDump(value);}
#else
using DocumentAuthorship=iiFileProvider::Authorship;
using DocumentFileAuthor=iiFileProvider::FileAuthor;
inline std::string authorshipDump(const DocumentAuthorship& value) {return value.dump().toStdString();}
inline auto authorshipFromDump(std::string_view value) {
    return DocumentAuthorship::fromDump(QByteArray(value.data(),static_cast<qsizetype>(value.size())));
}
#endif
}
