#pragma once

#include <string>

static std::string extract_host_from_srt_url(const std::string& url) {
    const std::string prefix = "srt://";
    size_t pos   = url.rfind(prefix, 0);
    size_t start = (pos == 0) ? prefix.size() : 0;
    size_t end   = url.find_first_of(":/?", start);
    if (end == std::string::npos) end = url.size();
    return url.substr(start, end - start);
}
