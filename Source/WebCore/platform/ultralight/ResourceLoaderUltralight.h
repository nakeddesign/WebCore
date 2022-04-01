#pragma once
#include <Ultralight/platform/FileSystem.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

namespace ResourceLoader {

    WEBCORE_EXPORT ultralight::RefPtr<ultralight::Buffer> openFile(const String& filePath);

    WEBCORE_EXPORT String readFileToString(const String& filePath);

} // namespace ResourceLoader

} // namespace WebCore
