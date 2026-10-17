#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <string_view>

namespace pc {
inline std::filesystem::path pathFromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
// A resource name as the game looks it up: lower case, forward slashes.
std::string assetKey(std::string name);
// The game's resources, from resource packs (files) and folders. A later
// source adds files and replaces those of the same name.
class Assets {
public:
    Assets() = default;
    explicit Assets(const std::filesystem::path& root) { add(root); }
    void add(const std::filesystem::path& source);
    std::vector<std::uint8_t> read(const std::string& name) const;
    bool contains(const std::string& name) const;
    std::size_t count() const { return files_.size(); }
    std::vector<std::string> names() const;
private:
    struct Entry { std::filesystem::path path; std::uint64_t offset=0,size=0,hash=0; bool packed=false; };
    std::unordered_map<std::string, Entry> files_;
    // A packed source is read once and kept, so a file inside it is a slice. On the
    // browser's virtual filesystem every open-then-read copies the whole file, so
    // reading a pack per file would copy it about 2000 times - measured as the page
    // blocking its own main thread. Windows pays nothing for this: the pack is small
    // and one read is the same cost as many.
    std::unordered_map<std::string, std::vector<std::uint8_t>> sources_;
};
}
