//
// GamesBackup: the console's built-in games (/gaadata/<id>/ - the discs, the cover, pcsx.cfg) copied to the
// USB stick, a folder per game named by its title, under "Games Backup". Nothing on the console is written:
// the games are read, the copies go to the stick. A game already there at the same sizes is skipped, so an
// interrupted backup picks up where it stopped. SDL-free, tested.
//
#pragma once

#include <ableem/engine/byte_progress.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

//******************
// GamesBackup
//******************
class GamesBackup {
public:
    struct File {
        std::string source;
        std::string dest;
        uint64_t size = 0;
    };
    struct Game {
        int id = 0;
        std::string title;
        std::string source; // <internal games dir>/<id>
        std::string dest;   // <dest root>/<folder named by the title>
        std::vector<File> files;
        uint64_t bytes = 0; // every file's size together
    };

    static const char *const FolderName; // "Games Backup", the folder on the stick's root

    // every numbered folder under `internalGamesDir` (the console's /gaadata) as a game, in id order, titled
    // from `internalDb` (the stick's copy of the console's database) when it knows the id and "Game <id>"
    // otherwise; each goes to <destRoot>/<its title as a FAT-safe name>, "<title> (<id>)" when two share one
    static std::vector<Game> find(const std::string &internalGamesDir, const std::string &internalDb,
                                  const std::string &destRoot);

    // a file already copied: there, at the source's size
    static bool alreadyThere(const File &file);
    // what copy() will write, in bytes (files already there left out)
    static uint64_t bytesToCopy(const std::vector<Game> &games);

    using GameStarted = std::function<void(const Game &game, size_t index, size_t count)>;
    // copies every file not already there - through "<name>.part", renamed once whole, so a copy cut short
    // never looks finished. `started` is told of each game, `bytes` of the bytes written over the whole
    // backup (against bytesToCopy). False at the first file that fails, with the reason logged.
    static bool copy(const std::vector<Game> &games, const GameStarted &started, const ableem::ByteProgress &bytes);

    // `title` as a folder name every filesystem the stick may have takes: no <>:"/\|?* or control
    // characters, no trailing dots or spaces, not empty
    static std::string folderName(const std::string &title);
};
