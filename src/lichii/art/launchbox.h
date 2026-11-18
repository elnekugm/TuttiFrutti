// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QIODevice;

/// LaunchBox Games Database artwork lookup.
///
/// LaunchBox has no public API. Its metadata package (Metadata.zip, updated daily) holds the games
/// and their image file names, and images are served from gamesdb-images.launchbox.gg/<FileName>;
/// both were checked against the real package. The index is built by an explicit user action, not
/// at start-up, and only the Nintendo Switch platforms are kept.
namespace LaunchBox {

/// One game of the metadata package, reduced to what a lookup needs.
struct Game {
    qint64 database_id{};
    QString name;
    QString platform;
    int year{};
    QStringList alternate_names;
    QString square;     ///< FileName of the "Square" image, the library art, if any
    QString box_front;  ///< FileName of the "Box - Front" image, if any
    QString box_3d;     ///< FileName of the "Box - 3D" image, if any
};

/// A search candidate and how well it matches.
struct Candidate {
    Game game;
    double score{};
};

/// What a title's suffix says about the entry.
struct ParsedTitle {
    QString base;  ///< The title without versions, tags, regions or an extension
    bool update{};
    bool demo{};
    bool dlc{};
    bool edition{};
};

/// Title normalization: Unicode decomposition, accents and diacritics dropped, case folded,
/// everything but letters and digits turned into single spaces.
QString Normalize(const QString& text);

/// Strips a file extension, region/ID brackets, version tokens and demo/update/DLC tags.
ParsedTitle ParseTitle(const QString& title);

/// How well `game` answers `title` on `platform`, in [0, 1]. Exact normalized names score 1.
double Score(const QString& title, const QString& platform, const Game& game);

/// Candidates with score >= MinScore, best first, at most `max_results`. `aliases` maps a
/// normalized query to a better one (for example an abbreviation the user configured).
QVector<Candidate> Rank(const QString& title, const QString& platform, const QVector<Game>& games,
                        int max_results = 20,
                        const QHash<QString, QString>& aliases = {});

constexpr double MinScore = 0.55;

/// Parses Metadata.xml, keeping Nintendo Switch and Nintendo Switch 2 games, their alternate names
/// and their box images. `xml` must be positioned at the start.
bool ParseMetadata(QIODevice& xml, QVector<Game>* out, QString* error);

/// Adds every Switch game from one metadata package. `report(percent)` may return false to stop.
bool BuildIndexFromZip(const QString& zip_path, QVector<Game>* out, QString* error,
                       const std::function<bool(int)>& report = {});

/// Index file, written atomically.
bool SaveIndex(const QVector<Game>& games, const QString& path, QString* error);
bool LoadIndex(const QString& path, QVector<Game>* out, QString* error);

/// <config>/launchbox-aliases.json (normalized source -> title).
QString DefaultAliasesPath();

/// <cache>/launchbox/switch-index.bin
QString DefaultIndexPath();

/// https://gamesdb-images.launchbox.gg/<FileName>
QString ImageUrl(const QString& file_name);

/// How good an image type is as library artwork: Square (the library cover) first, then box
/// front, then box 3D. Icons, logos, banners, screenshots, backgrounds and advertisements are
/// excluded (0).
int ImageTypeScore(const QString& type);

/// FileName of the best image of `game` for the library, or empty. Square artwork wins.
QString BestImage(const Game& game);

/// Reads the user's aliases (normalized source -> title), or an empty map when the file is absent.
QHash<QString, QString> LoadAliases(const QString& path);

}  // namespace LaunchBox