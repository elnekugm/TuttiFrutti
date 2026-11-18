// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/art/launchbox.h"

#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

#include "common/fs/path_util.h"

namespace LaunchBox {
namespace {

constexpr quint32 IndexMagic = 0x54464C42;  // "TFLB"
constexpr quint32 IndexVersion = 1;
constexpr quint32 MaxIndexedGames = 200'000;

bool IsSwitchPlatform(const QString& platform) {
    return platform == QStringLiteral("Nintendo Switch") ||
           platform == QStringLiteral("Nintendo Switch 2");
}

/// Levenshtein distance, stopping once it is known to exceed `limit`.
int EditDistance(const QString& a, const QString& b, int limit) {
    if (std::abs(a.size() - b.size()) > limit) {
        return limit + 1;
    }
    QVector<int> previous(b.size() + 1);
    QVector<int> current(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) {
        previous[j] = j;
    }
    for (int i = 1; i <= a.size(); ++i) {
        current[0] = i;
        int row_min = current[0];
        for (int j = 1; j <= b.size(); ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            row_min = std::min(row_min, current[j]);
        }
        if (row_min > limit) {
            return limit + 1;
        }
        std::swap(previous, current);
    }
    return previous[b.size()];
}

/// A token matches exactly, or one typo away for words of four letters or more.
bool TokenMatches(const QString& a, const QString& b) {
    if (a == b) {
        return true;
    }
    if (a.size() < 4 || b.size() < 4) {
        return false;
    }
    const int allowed = (a.size() >= 6 && b.size() >= 6) ? 2 : 1;
    return EditDistance(a, b, allowed) <= allowed;
}

double ScoreName(const QString& normalized_query, const QString& candidate) {
    const ParsedTitle parsed = ParseTitle(candidate);
    const QString name = Normalize(parsed.base);
    if (name.isEmpty()) {
        return 0.0;
    }
    if (name == normalized_query) {
        return 1.0;
    }
    QString compact_query = normalized_query;
    compact_query.remove(QLatin1Char(' '));
    QString compact_name = name;
    compact_name.remove(QLatin1Char(' '));
    if (compact_name == compact_query) {
        return 0.98;
    }
    const QStringList query_tokens = normalized_query.split(u' ', Qt::SkipEmptyParts);
    const QStringList name_tokens = name.split(u' ', Qt::SkipEmptyParts);
    if (query_tokens.isEmpty() || name_tokens.isEmpty()) {
        return 0.0;
    }
    int present = 0;
    for (const QString& token : query_tokens) {
        if (std::any_of(name_tokens.begin(), name_tokens.end(),
                        [&](const QString& candidate_token) {
                            return TokenMatches(token, candidate_token);
                        })) {
            ++present;
        }
    }
    const double coverage = double(present) / double(query_tokens.size());
    const double ratio = double(std::min(normalized_query.size(), name.size())) /
                         double(std::max(normalized_query.size(), name.size()));
    double score = 0.70 * coverage + 0.20 * ratio;
    if (name.startsWith(normalized_query)) {
        score += 0.05;
    } else if (name.contains(normalized_query)) {
        score += 0.02;
    }
    return std::min(score, 0.99);
}

struct ImagePick {
    QString file;
    int score{};
};

int RegionScore(const QString& region) {
    const QString value = region.trimmed().toLower();
    if (value.isEmpty() || value == QStringLiteral("world")) {
        return 3;
    }
    if (value == QStringLiteral("north america") || value == QStringLiteral("usa")) {
        return 2;
    }
    if (value == QStringLiteral("europe")) {
        return 1;
    }
    return 0;
}

}  // namespace

QString Normalize(const QString& text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    bool pending_space = false;
    for (const QChar ch : decomposed) {
        if (ch.category() == QChar::Mark_NonSpacing) {
            continue;
        }
        if (ch.isLetterOrNumber()) {
            if (pending_space && !out.isEmpty()) {
                out.append(u' ');
            }
            pending_space = false;
            out.append(ch.toLower());
        } else {
            pending_space = true;
        }
    }
    return out;
}

ParsedTitle ParseTitle(const QString& title) {
    QString text = title;

    static const QStringList extensions{QStringLiteral(".nsp"),  QStringLiteral(".xci"),
                                       QStringLiteral(".nro"),  QStringLiteral(".nup"),
                                       QStringLiteral(".nsz"),  QStringLiteral(".xcz"),
                                       QStringLiteral(".zip"),  QStringLiteral(".7z"),
                                       QStringLiteral(".rar")};
    for (const QString& extension : extensions) {
        if (text.endsWith(extension, Qt::CaseInsensitive)) {
            text.chop(extension.size());
            break;
        }
    }
    text.replace(u'_', u' ');
    // CamelCase words are separate tags: GlobalJam, GlobalFriendsPass, DragXDrive.
    text.replace(QRegularExpression(QStringLiteral("([a-z0-9])([A-Z])")),
                 QStringLiteral("\\1 \\2"));

    const QString lowered = text.toLower();
    ParsedTitle parsed;
    parsed.update =
        QRegularExpression(QStringLiteral("\\b(update|patch)\\b")).match(lowered).hasMatch();
    parsed.demo = QRegularExpression(
                      QStringLiteral("\\b(demo|trial|beta|global jam|global friends pass)\\b"))
                      .match(lowered)
                      .hasMatch();
    parsed.dlc =
        QRegularExpression(QStringLiteral("\\b(dlc|add-?on)\\b")).match(lowered).hasMatch();
    parsed.edition = lowered.contains(QStringLiteral("switch 2 edition")) ||
                     QRegularExpression(QStringLiteral("\\bsw2\\b")).match(lowered).hasMatch();

    // Bracketed IDs and tags, then parenthesized region/version groups.
    text.remove(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]")));
    text.remove(QRegularExpression(
        QStringLiteral("\\((?:usa|eur|jpn|world|asia|kor|chn|twn|can|aus|bra|mex|v?\\d[\\d.]*)"
                       "[^)]*\\)"),
        QRegularExpression::CaseInsensitiveOption));

    static const QRegularExpression version(
        QStringLiteral("\\bv?\\d+(?:\\.\\d+)+\\b|\\bversion\\s*\\d+\\b|\\bbuild\\s*\\d+\\b"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(version);

    static const QStringList tags{
        QStringLiteral("update"),          QStringLiteral("patch"),
        QStringLiteral("demo"),            QStringLiteral("trial"),
        QStringLiteral("beta"),            QStringLiteral("dlc"),
        QStringLiteral("add-on"),          QStringLiteral("addon"),
        QStringLiteral("global jam"),      QStringLiteral("global friends pass"),
        QStringLiteral("switch 2 edition"), QStringLiteral("sw2"),
        QStringLiteral("nsp"),             QStringLiteral("xci"),
        QStringLiteral("nro"),             QStringLiteral("nup"),
        QStringLiteral("nsz"),             QStringLiteral("xcz"),
    };
    for (const QString& tag : tags) {
        text.remove(QRegularExpression(
            QStringLiteral("\\b") + QRegularExpression::escape(tag) + QStringLiteral("\\b"),
            QRegularExpression::CaseInsensitiveOption));
    }
    text.replace(u'+', u' ');
    text.replace(QChar(0x2013), u' ');
    text.replace(QChar(0x2014), u' ');

    text = text.simplified();
    const auto is_tail_punctuation = [](QChar ch) {
        return ch == u'-' || ch == u':' || ch == u',' || ch == QChar(0x2013) || ch == QChar(0x2014);
    };
    while (!text.isEmpty() && is_tail_punctuation(text.back())) {
        text.chop(1);
    }

    parsed.base = text.simplified();
    return parsed;
}

double Score(const QString& title, const QString& platform, const Game& game) {
    const QString normalized_query = Normalize(ParseTitle(title).base);
    if (normalized_query.isEmpty()) {
        return 0.0;
    }
    double best = ScoreName(normalized_query, game.name);
    for (const QString& alternate : game.alternate_names) {
        best = std::max(best, ScoreName(normalized_query, alternate));
    }

    if (!platform.isEmpty()) {
        const QString query_platform = Normalize(platform);
        const QString game_platform = Normalize(game.platform);
        if (query_platform == game_platform) {
            best += 0.06;
        } else if (query_platform.startsWith(QStringLiteral("nintendo switch")) &&
                   game_platform.startsWith(QStringLiteral("nintendo switch"))) {
            best += 0.03;
        } else {
            best -= 0.10;
        }
    }
    return std::clamp(best, 0.0, 1.0);
}

QVector<Candidate> Rank(const QString& title, const QString& platform, const QVector<Game>& games,
                        int max_results, const QHash<QString, QString>& aliases) {
    QString query = title;
    const QString normalized_query = Normalize(ParseTitle(title).base);
    if (const auto alias = aliases.constFind(normalized_query); alias != aliases.constEnd()) {
        query = alias.value();
    }

    QVector<Candidate> candidates;
    candidates.reserve(std::min<qsizetype>(games.size(), 64));
    for (const Game& game : games) {
        const double score = Score(query, platform, game);
        if (score >= MinScore) {
            candidates.push_back({game, score});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.game.name.size() != b.game.name.size()) {
            return a.game.name.size() < b.game.name.size();
        }
        return a.game.database_id < b.game.database_id;
    });

    QVector<Candidate> unique;
    QSet<qint64> seen;
    for (const Candidate& candidate : candidates) {
        if (seen.contains(candidate.game.database_id)) {
            continue;
        }
        seen.insert(candidate.game.database_id);
        unique.push_back(candidate);
        if (unique.size() >= max_results) {
            break;
        }
    }
    return unique;
}

int ImageTypeScore(const QString& type) {
    const QString value = type.trimmed().toLower();
    if (value == QStringLiteral("box - front")) {
        return 100;
    }
    if (value == QStringLiteral("box - 3d")) {
        return 80;
    }
    // Icons, clear logos, banners, screenshots, backgrounds, advertisements, carts, discs.
    return 0;
}

QString BestImage(const Game& game) {
    return game.box_front.isEmpty() ? game.box_3d : game.box_front;
}

QString ImageUrl(const QString& file_name) {
    return QStringLiteral("https://gamesdb-images.launchbox.gg/") + file_name;
}

namespace {

void ParseGame(QXmlStreamReader& xml, Game* game) {
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement && xml.name() == QStringLiteral("Game")) {
            return;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString name = xml.name().toString();
        if (name == QStringLiteral("Name")) {
            game->name = xml.readElementText();
        } else if (name == QStringLiteral("Platform")) {
            game->platform = xml.readElementText();
        } else if (name == QStringLiteral("DatabaseID")) {
            game->database_id = xml.readElementText().toLongLong();
        } else if (name == QStringLiteral("ReleaseYear")) {
            game->year = xml.readElementText().toInt();
        } else if (name == QStringLiteral("ReleaseDate")) {
            const QString date = xml.readElementText();
            if (game->year == 0 && date.size() >= 4) {
                game->year = date.left(4).toInt();
            }
        } else {
            xml.skipCurrentElement();
        }
    }
}

void ParseAlternateName(QXmlStreamReader& xml, qint64* database_id, QString* name) {
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement &&
            xml.name() == QStringLiteral("GameAlternateName")) {
            return;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString field = xml.name().toString();
        if (field == QStringLiteral("AlternateName")) {
            *name = xml.readElementText();
        } else if (field == QStringLiteral("DatabaseID")) {
            *database_id = xml.readElementText().toLongLong();
        } else {
            xml.skipCurrentElement();
        }
    }
}

void ParseImage(QXmlStreamReader& xml, qint64* database_id, QString* file, QString* type,
                QString* region) {
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement && xml.name() == QStringLiteral("GameImage")) {
            return;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString field = xml.name().toString();
        if (field == QStringLiteral("DatabaseID")) {
            *database_id = xml.readElementText().toLongLong();
        } else if (field == QStringLiteral("FileName")) {
            *file = xml.readElementText();
        } else if (field == QStringLiteral("Type")) {
            *type = xml.readElementText();
        } else if (field == QStringLiteral("Region")) {
            *region = xml.readElementText();
        } else {
            xml.skipCurrentElement();
        }
    }
}

}  // namespace

bool ParseMetadata(QIODevice& xml, QVector<Game>* out, QString* error) {
    QXmlStreamReader reader(&xml);
    QVector<Game> games;
    QHash<qint64, int> by_id;
    QHash<qint64, ImagePick> front;
    QHash<qint64, ImagePick> three_d;

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString name = reader.name().toString();
        if (name == QStringLiteral("Game")) {
            Game game;
            ParseGame(reader, &game);
            if (IsSwitchPlatform(game.platform)) {
                by_id.insert(game.database_id, games.size());
                games.push_back(std::move(game));
            }
        } else if (name == QStringLiteral("GameAlternateName")) {
            qint64 database_id = 0;
            QString alternate;
            ParseAlternateName(reader, &database_id, &alternate);
            const auto it = by_id.constFind(database_id);
            if (it != by_id.constEnd() && !alternate.isEmpty()) {
                games[it.value()].alternate_names.push_back(alternate);
            }
        } else if (name == QStringLiteral("GameImage")) {
            qint64 database_id = 0;
            QString file;
            QString type;
            QString region;
            ParseImage(reader, &database_id, &file, &type, &region);
            if (!by_id.contains(database_id)) {
                continue;
            }
            const int type_score = ImageTypeScore(type);
            if (type_score == 0 || file.isEmpty()) {
                continue;
            }
            ImagePick pick{file, type_score * 10 + RegionScore(region)};
            QHash<qint64, ImagePick>& target =
                type.compare(QStringLiteral("Box - 3D"), Qt::CaseInsensitive) == 0 ? three_d : front;
            const auto existing = target.constFind(database_id);
            if (existing == target.constEnd() || pick.score > existing->score) {
                target.insert(database_id, pick);
            }
        }
    }

    if (reader.hasError()) {
        if (error) {
            *error = reader.errorString();
        }
        return false;
    }
    for (auto it = front.constBegin(); it != front.constEnd(); ++it) {
        games[by_id.value(it.key())].box_front = it->file;
    }
    for (auto it = three_d.constBegin(); it != three_d.constEnd(); ++it) {
        games[by_id.value(it.key())].box_3d = it->file;
    }
    *out = std::move(games);
    return true;
}

bool SaveIndex(const QVector<Game>& games, const QString& path, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << IndexMagic << IndexVersion << quint32(games.size());
    for (const Game& game : games) {
        stream << game.database_id << game.name << game.platform << qint32(game.year)
               << game.alternate_names << game.box_front << game.box_3d;
    }
    if (stream.status() != QDataStream::Ok || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

bool LoadIndex(const QString& path, QVector<Game>* out, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 count = 0;
    stream >> magic >> version >> count;
    if (stream.status() != QDataStream::Ok || magic != IndexMagic || version != IndexVersion ||
        count > MaxIndexedGames) {
        if (error) {
            *error = QStringLiteral("not a LaunchBox art index, or a newer version");
        }
        return false;
    }
    QVector<Game> games;
    games.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        Game game;
        qint32 year = 0;
        stream >> game.database_id >> game.name >> game.platform >> year >>
            game.alternate_names >> game.box_front >> game.box_3d;
        if (stream.status() != QDataStream::Ok) {
            if (error) {
                *error = QStringLiteral("truncated LaunchBox art index");
            }
            return false;
        }
        game.year = year;
        games.push_back(std::move(game));
    }
    *out = std::move(games);
    return true;
}

QString DefaultIndexPath() {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                      "launchbox" / "switch-index.bin";
    return QString::fromStdString(path.generic_string());
}

QString DefaultAliasesPath() {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) /
                      "launchbox-aliases.json";
    return QString::fromStdString(path.generic_string());
}

QHash<QString, QString> LoadAliases(const QString& path) {
    QHash<QString, QString> aliases;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return aliases;
    }
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return aliases;
    }
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString target = it.value().toString();
        if (target.isEmpty()) {
            continue;
        }
        aliases.insert(Normalize(it.key()), target);
    }
    return aliases;
}

}  // namespace LaunchBox