#include "Assets.hpp"
#include "AssetPack.hpp"
#include "GameInstall.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace pc {
std::string assetKey(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    const std::filesystem::path path(name);
    if (path.is_absolute() || path.has_root_name()) throw std::runtime_error("Absolute asset path rejected");
    for (const auto& part : path)
        if (part == "..") throw std::runtime_error("Asset path traversal rejected");
    return path.lexically_normal().generic_string();
}
void Assets::add(const std::filesystem::path& root) {
    std::unordered_map<std::string, Entry> files;
    if(std::filesystem::is_regular_file(root)) {
        // A Nintendo DS ROM is a source in its own right. The game reads its own
        // file system, so the browser build points at the ROM instead of at a
        // folder extracted from it. romGameCode is the discriminator - it
        // validates the header CRC, so a pack cannot be mistaken for a ROM and
        // the Windows path (pack file, or folder) is untouched.
        if(!romGameCode(root).empty()) {
            for(const auto& [name,r]:romRanges(root))
                if(!files.emplace(assetKey(name),Entry{root,r.offset,r.size,r.hash,true}).second)
                    throw std::runtime_error("Duplicate ROM path: "+name);
        } else for(const auto& e:readAssetIndex(root)) {
            const auto name=assetKey(e.name);
            if(name!=e.name || !files.emplace(name,Entry{root,e.offset,e.size,e.hash,true}).second)
                throw std::runtime_error("Invalid or duplicate packed asset path: "+e.name);
        }
    } else {
        if (!std::filesystem::is_directory(root))
            throw std::runtime_error("Assets directory missing: " + root.string());
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            const auto name = assetKey(entry.path().lexically_relative(root).generic_string());
            if (!files.emplace(name, Entry{entry.path()}).second)
                throw std::runtime_error("Ambiguous asset name: " + name);
        }
    }
    // Read a packed source once, here, so every later file is a slice. Measured the
    // alternative on the browser's virtual filesystem: reading it per file copies the
    // whole file each time, about 2000 times, and the page stops answering.
    bool packed=false;
    for(const auto& [name,entry]:files) if(entry.packed) {packed=true;break;}
    if(packed && !sources_.contains(root.string())) {
        std::ifstream stream(root,std::ios::binary|std::ios::ate);
        if(!stream) throw std::runtime_error("Cannot open asset source: "+root.string());
        const auto size=std::uint64_t(stream.tellg());
        stream.seekg(0);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if(!stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read asset source: "+root.string());
        sources_.emplace(root.string(),std::move(bytes));
    }
    for (auto& [name, entry] : files) files_.insert_or_assign(name, std::move(entry));
}
std::vector<std::uint8_t> Assets::read(const std::string& name) const {
    const auto found = files_.find(assetKey(name));
    if (found == files_.end()) throw std::runtime_error("Asset missing: " + name);
    const auto& entry=found->second;
    // From the cache when the source is a packed file, which is the whole point of
    // caching it; the folder path still opens the file itself.
    const auto cached=sources_.find(entry.path.string());
    std::vector<std::uint8_t> data;
    if(cached!=sources_.end()) {
        if(entry.offset>cached->second.size()||entry.size>cached->second.size()-entry.offset)
            throw std::runtime_error("Asset range out of bounds: "+name);
        data.assign(cached->second.begin()+std::ptrdiff_t(entry.offset),
                    cached->second.begin()+std::ptrdiff_t(entry.offset+entry.size));
    } else {
        std::ifstream stream(entry.path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error("Cannot open asset: " + name);
        const auto size = entry.packed ? std::streamoff(entry.size) : std::streamoff(stream.tellg());
        if (size < 0) throw std::runtime_error("Cannot size asset: " + name);
        data.resize(static_cast<std::size_t>(size));
        stream.seekg(static_cast<std::streamoff>(entry.offset));
        if (!stream.read(reinterpret_cast<char*>(data.data()), size))
            throw std::runtime_error("Cannot read asset: " + name);
    }
    // A stored hash of zero means the source has no per-file checksum - that is the
    // ROM case, and it is a measured decision in GameInstall.cpp, not a gap.
    if(entry.packed && entry.hash && assetHash(data)!=entry.hash) {
        std::cout<<"diagnostic: "<<name<<" size "<<data.size()<<" hash-at-index "<<std::hex<<entry.hash
                 <<" hash-of-what-i-read "<<assetHash(data)<<"\n";
        throw std::runtime_error("Asset checksum mismatch: "+name);
    }
    return data;
}
bool Assets::contains(const std::string& name) const { return files_.contains(assetKey(name)); }
std::vector<std::string> Assets::names() const {
    std::vector<std::string> result; result.reserve(files_.size());
    for(const auto& [name,entry]:files_) result.push_back(name);
    std::sort(result.begin(),result.end()); return result;
}
}
