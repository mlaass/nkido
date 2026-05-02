#include "cedar/io/handlers/github_handler.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace cedar {

namespace {

constexpr std::array<std::string_view, 8> AUDIO_EXTENSIONS = {
    ".wav", ".ogg", ".flac", ".mp3", ".aiff", ".sf2", ".sf3", ".json",
};

bool ends_with(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    auto tail = s.substr(s.size() - suffix.size());
    if (tail.size() != suffix.size()) return false;
    for (std::size_t i = 0; i < tail.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(tail[i]))
                != std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

bool looks_like_file(std::string_view path) {
    for (auto ext : AUDIO_EXTENSIONS) {
        if (ends_with(path, ext)) return true;
    }
    return false;
}

}  // namespace

std::string GithubHandler::to_https_url(std::string_view uri) {
    constexpr std::string_view prefix = "github:";
    if (uri.substr(0, prefix.size()) != prefix) return std::string();
    std::string_view rest = uri.substr(prefix.size());
    while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);

    // Split into segments by '/'.
    std::vector<std::string_view> segs;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= rest.size(); ++i) {
        if (i == rest.size() || rest[i] == '/') {
            if (i > start) segs.push_back(rest.substr(start, i - start));
            start = i + 1;
        }
    }
    if (segs.size() < 2) return std::string();  // Need at least user/repo

    std::string user(segs[0]);
    std::string repo(segs[1]);
    std::string branch = segs.size() >= 3 ? std::string(segs[2]) : "main";

    std::string sub_path;
    if (segs.size() >= 4) {
        for (std::size_t i = 3; i < segs.size(); ++i) {
            sub_path.push_back('/');
            sub_path.append(segs[i].begin(), segs[i].end());
        }
    }

    std::string base = "https://raw.githubusercontent.com/" + user + "/"
                       + repo + "/" + branch;
    if (sub_path.empty()) {
        return base + "/strudel.json";
    }
    if (looks_like_file(sub_path)) {
        return base + sub_path;
    }
    return base + sub_path + "/strudel.json";
}

LoadResult GithubHandler::load(std::string_view uri) const {
    auto url = to_https_url(uri);
    if (url.empty()) {
        return {FileLoadError{FileError::InvalidFormat,
                              "invalid github: URI: " + std::string(uri)}};
    }
    return UriResolver::instance().load(url);
}

}  // namespace cedar
