#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <cstdint>

namespace pc {
// The game's data comes from the player's own copies of the game, extracted
// into the assets folder when the game is built (build.ps1, dqiv_pack): the
// European Nintendo DS ROM (required) and the Android version's data file
// (.obb, optional HD sound).

// The ROM's four-letter game code ("YIVP" for the European Dragon Quest IV),
// empty when the file is not a Nintendo DS ROM.
std::string romGameCode(const std::filesystem::path& rom);
inline constexpr const char* europeanGameCode="YIVP";

// The ROM's own file table: name -> range inside the ROM file. The browser build
// reads the ROM in place instead of extracting it, so it needs the table rather
// than a folder. Same data extractRom works from. hash is the same checksum the
// asset pack carries, so the integrity check in Assets::read stays real here too.
struct RomRange { std::uint64_t offset=0, size=0, hash=0; };
std::map<std::string,RomRange> romRanges(const std::filesystem::path& rom);

// Copies the ROM's file system (data/...) into a folder; throws on another ROM.
void extractRom(const std::filesystem::path& rom,const std::filesystem::path& folder);
// Copies the Android version's sounds (assets/sound in the .obb) into
// folder/data/SOUND/MOBILE; throws when the file has none.
void extractMobileSounds(const std::filesystem::path& obb,const std::filesystem::path& folder);
}
