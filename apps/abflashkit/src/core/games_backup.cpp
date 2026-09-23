//
// GamesBackup: the built-in games copied to the stick.
//
// 64-bit sizes on the 32-bit console (stat of a big disc image)
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif

#include "games_backup.h"
#include "core/main.h"

#include <ableem/engine/game_database.h>
#include <ableem/engine/log.h>

#include <algorithm>
#include <cctype>
#include <map>

using namespace std;

const char *const GamesBackup::FolderName = "Games Backup";

namespace {
bool isNumber(const string &name) {
    return !name.empty() && all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// every file under `dir`, sub-folders included, paired with where it goes under `dest`
void collect(const string &dir, const string &dest, vector<GamesBackup::File> &files) {
    for (const DirEntry &entry : DirEntry::diru(dir)) {
        const string source = dir + sep + entry.name;
        const string target = dest + sep + entry.name;
        if (entry.isDir) {
            collect(source, target, files);
            continue;
        }
        long long size = DirEntry::fileSize(source);
        GamesBackup::File file;
        file.source = source;
        file.dest = target;
        file.size = size > 0 ? static_cast<uint64_t>(size) : 0;
        files.push_back(file);
    }
}

// id -> title from the stick's internal.db; empty when there is none or it cannot be read
map<int, string> titlesFrom(const string &internalDb) {
    map<int, string> titles;
    if (internalDb.empty() || !DirEntry::exists(internalDb))
        return titles;
    GameDatabase db;
    if (!db.open(internalDb))
        return titles;
    for (const ableem::GameRecord &record : db.loadInternalGames())
        titles[record.gameId] = record.title;
    return titles;
}
} // namespace

//*******************************
// GamesBackup::folderName
//*******************************
string GamesBackup::folderName(const string &title) {
    string name;
    for (char c : title) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || string("<>:\"/\\|?*").find(c) != string::npos)
            name += (c == ':' ? " -" : " ");
        else
            name += c;
    }
    // no runs of spaces left by the replacements, nothing at either end Windows would drop
    string squeezed;
    for (char c : name) {
        if (c == ' ' && !squeezed.empty() && squeezed.back() == ' ')
            continue;
        squeezed += c;
    }
    while (!squeezed.empty() && (squeezed.back() == ' ' || squeezed.back() == '.'))
        squeezed.pop_back();
    while (!squeezed.empty() && squeezed.front() == ' ')
        squeezed.erase(squeezed.begin());
    return squeezed.empty() ? "Game" : squeezed;
}

//*******************************
// GamesBackup::find
//*******************************
vector<GamesBackup::Game> GamesBackup::find(const string &internalGamesDir, const string &internalDb,
                                            const string &destRoot) {
    vector<Game> games;
    if (!DirEntry::isDirectory(internalGamesDir))
        return games;
    const map<int, string> titles = titlesFrom(internalDb);
    for (const DirEntry &entry : DirEntry::diru_DirsOnly(internalGamesDir)) {
        if (!isNumber(entry.name) || entry.name.size() > 6)
            continue;
        Game game;
        game.id = atoi(entry.name.c_str());
        auto title = titles.find(game.id);
        // Sony's database has a trailing space here and there ("Cool Boarders 2 ")
        const string known = title != titles.end() ? Strings::trim(title->second) : "";
        game.title = !known.empty() ? known : "Game " + entry.name;
        game.source = internalGamesDir + sep + entry.name;
        games.push_back(game);
    }
    sort(games.begin(), games.end(), [](const Game &a, const Game &b) { return a.id < b.id; });

    // the folders: by title, the id added to every one of a title two games share
    map<string, int> uses;
    for (const Game &game : games)
        uses[folderName(game.title)]++;
    for (Game &game : games) {
        string folder = folderName(game.title);
        if (uses[folder] > 1)
            folder += " (" + to_string(game.id) + ")";
        game.dest = destRoot + sep + folder;
        collect(game.source, game.dest, game.files);
        for (const File &file : game.files)
            game.bytes += file.size;
    }
    // a numbered folder with nothing in it is not a game
    games.erase(remove_if(games.begin(), games.end(), [](const Game &game) { return game.files.empty(); }),
                games.end());
    return games;
}

//*******************************
// GamesBackup::alreadyThere / bytesToCopy
//*******************************
bool GamesBackup::alreadyThere(const File &file) {
    return DirEntry::fileSize(file.dest) == static_cast<long long>(file.size);
}

uint64_t GamesBackup::bytesToCopy(const vector<Game> &games) {
    uint64_t total = 0;
    for (const Game &game : games)
        for (const File &file : game.files)
            if (!alreadyThere(file))
                total += file.size;
    return total;
}

//*******************************
// GamesBackup::copy
//*******************************
bool GamesBackup::copy(const vector<Game> &games, const GameStarted &started, const ableem::ByteProgress &bytes) {
    const uint64_t total = bytesToCopy(games);
    uint64_t before = 0; // the bytes of the files already copied in this run
    for (size_t i = 0; i < games.size(); i++) {
        const Game &game = games[i];
        if (started)
            started(game, i, games.size());
        for (const File &file : game.files) {
            if (alreadyThere(file))
                continue;
            if (!DirEntry::createDirs(DirEntry::getDirNameFromPath(file.dest))) {
                PLOG_ERROR << "Cannot create the folder for " << file.dest;
                return false;
            }
            const string part = file.dest + ".part";
            bool ok = DirEntry::copy(file.source, part, [&](uint64_t done, uint64_t) {
                if (bytes)
                    bytes(before + done, total);
            });
            // a short copy (the source unreadable part way) is as bad as a failed one
            ok = ok && DirEntry::fileSize(part) == static_cast<long long>(file.size);
            ok = ok && DirEntry::replaceFile(part, file.dest);
            if (!ok) {
                PLOG_ERROR << "Copying " << file.source << " to " << file.dest << " failed";
                DirEntry::removeFile(part);
                return false;
            }
            before += file.size;
        }
        PLOG_INFO << "Backed up game " << game.id << " (" << game.title << ") to " << game.dest;
    }
    return true;
}
