// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include "lichii/art/launchbox.h"
#include "lichii/art/launchbox_net.h"

#include <atomic>

using namespace LaunchBox;

namespace {

Game MakeGame(qint64 id, const QString& name, const QString& platform, int year = 0,
              const QStringList& alternate_names = {}, const QString& box_front = {},
              const QString& box_3d = {}) {
    Game game;
    game.database_id = id;
    game.name = name;
    game.platform = platform;
    game.year = year;
    game.alternate_names = alternate_names;
    game.box_front = box_front;
    game.box_3d = box_3d;
    return game;
}

}  // namespace

TEST_CASE("LaunchBox titles normalize accents, tags and versions", "[launchbox]") {
    REQUIRE(Normalize(QStringLiteral("Pokémon")) == QStringLiteral("pokemon"));
    REQUIRE(Normalize(QStringLiteral("  Mario   Kart—World ")) == QStringLiteral("mario kart world"));

    const ParsedTitle mkw = ParseTitle(QStringLiteral("mkw 1.8.0 update"));
    REQUIRE(mkw.base == QStringLiteral("mkw"));
    REQUIRE(mkw.update);

    const ParsedTitle jam = ParseTitle(QStringLiteral("DragXDrive_GlobalJam"));
    REQUIRE(jam.base == QStringLiteral("Drag XDrive"));
    REQUIRE(jam.demo);

    const ParsedTitle pass = ParseTitle(QStringLiteral("Orbitals_GlobalFriendsPass_v1.0.5"));
    REQUIRE(pass.base == QStringLiteral("Orbitals"));
    REQUIRE(pass.demo);

    const ParsedTitle edition =
        ParseTitle(QStringLiteral("Pokemon Legends Z-A-SW2"));
    REQUIRE(edition.base == QStringLiteral("Pokemon Legends Z-A"));
    REQUIRE(edition.edition);

    const ParsedTitle file =
        ParseTitle(QStringLiteral("Kirby and the Forgotten Land.nsp"));
    REQUIRE(file.base == QStringLiteral("Kirby and the Forgotten Land"));

    const ParsedTitle tagged =
        ParseTitle(QStringLiteral("[0100000000010000] Game (USA) (v1.0)"));
    REQUIRE(tagged.base == QStringLiteral("Game"));
}

TEST_CASE("LaunchBox ranks the intended game", "[launchbox]") {
    const QVector<Game> games{
        MakeGame(1, QStringLiteral("Splatoon Raiders"), QStringLiteral("Nintendo Switch 2"), 2026),
        MakeGame(2, QStringLiteral("Raiders of the Lost Ark"), QStringLiteral("Nintendo Switch"),
                 2000),
        MakeGame(3, QStringLiteral("Fire Emblem: Fortune's Weave"),
                 QStringLiteral("Nintendo Switch 2"), 2026),
        MakeGame(4, QStringLiteral("Drag x Drive"), QStringLiteral("Nintendo Switch 2"), 2025),
        MakeGame(5, QStringLiteral("Mario Kart World"), QStringLiteral("Nintendo Switch 2"), 2025),
        MakeGame(6, QStringLiteral("Orbitals"), QStringLiteral("Nintendo Switch 2"), 2026),
        MakeGame(7, QStringLiteral("Orbitals: Global Friend's Pass"),
                 QStringLiteral("Nintendo Switch 2"), 2026),
        MakeGame(8, QStringLiteral("Pokémon Legends: Z-A – Nintendo Switch 2 Edition"),
                 QStringLiteral("Nintendo Switch 2"), 2025),
        MakeGame(9,
                 QStringLiteral("Kirby and the Forgotten Land – Nintendo Switch 2 Edition + "
                                "Star-Crossed World"),
                 QStringLiteral("Nintendo Switch 2"), 2025),
    };

    SECTION("a short name finds its longer title") {
        const auto ranked = Rank(QStringLiteral("Raiders"), QStringLiteral("Nintendo Switch 2"), games);
        REQUIRE_FALSE(ranked.isEmpty());
        REQUIRE(ranked.front().game.name == QStringLiteral("Splatoon Raiders"));
    }
    SECTION("a typo still matches") {
        const auto ranked = Rank(QStringLiteral("Fire Emblem Fortunes Waves"),
                                 QStringLiteral("Nintendo Switch 2"), games);
        REQUIRE_FALSE(ranked.isEmpty());
        REQUIRE(ranked.front().game.name == QStringLiteral("Fire Emblem: Fortune's Weave"));
    }
    SECTION("a joined name matches a spaced title") {
        const auto ranked =
            Rank(QStringLiteral("DragXDrive_GlobalJam"), QStringLiteral("Nintendo Switch 2"), games);
        REQUIRE_FALSE(ranked.isEmpty());
        REQUIRE(ranked.front().game.name == QStringLiteral("Drag x Drive"));
    }
    SECTION("an abbreviation uses the configured alias") {
        const QHash<QString, QString> aliases{
            {QStringLiteral("mkw"), QStringLiteral("Mario Kart World")}};
        const auto ranked = Rank(QStringLiteral("mkw 1.8.0 update"),
                                 QStringLiteral("Nintendo Switch 2"), games, 20, aliases);
        REQUIRE_FALSE(ranked.isEmpty());
        REQUIRE(ranked.front().game.name == QStringLiteral("Mario Kart World"));
    }
    SECTION("the full game ranks above its pass") {
        const auto ranked = Rank(QStringLiteral("Orbitals_GlobalFriendsPass_v1.0.5"),
                                 QStringLiteral("Nintendo Switch 2"), games);
        REQUIRE(ranked.size() >= 2);
        REQUIRE(ranked[0].game.name == QStringLiteral("Orbitals"));
        REQUIRE(ranked[1].game.name == QStringLiteral("Orbitals: Global Friend's Pass"));
    }
    SECTION("a Switch 2 suffix finds the edition") {
        const auto ranked = Rank(QStringLiteral("Pokemon Legends Z-A-SW2"),
                                 QStringLiteral("Nintendo Switch 2"), games);
        REQUIRE_FALSE(ranked.isEmpty());
        REQUIRE(ranked.front().game.name.startsWith(QStringLiteral("Pokémon Legends: Z-A")));
    }
    SECTION("a typo at the start still matches the edition") {
        const auto ranked =
            Rank(QStringLiteral("irby and the Forgotten Land + Star-Crossed Worlds - Nintendo "
                                "Switch 2 Edition"),
                 QStringLiteral("Nintendo Switch 2"), games);
        REQUIRE_FALSE(ranked.isEmpty());
        REQUIRE(ranked.front().game.name.startsWith(QStringLiteral("Kirby and the Forgotten Land")));
    }
}

TEST_CASE("LaunchBox keeps Switch games and their box art", "[launchbox]") {
    const QByteArray xml = R"(<?xml version="1.0"?>
<LaunchBox>
  <Game>
    <Name>Mario Kart World</Name>
    <Platform>Nintendo Switch 2</Platform>
    <DatabaseID>10</DatabaseID>
    <ReleaseYear>2025</ReleaseYear>
  </Game>
  <Game>
    <Name>Other Game</Name>
    <Platform>PlayStation 5</Platform>
    <DatabaseID>11</DatabaseID>
  </Game>
  <GameAlternateName>
    <AlternateName>MK World</AlternateName>
    <DatabaseID>10</DatabaseID>
  </GameAlternateName>
  <GameImage>
    <DatabaseID>10</DatabaseID>
    <FileName>aaa.jpg</FileName>
    <Type>Box - Front</Type>
    <Region>North America</Region>
  </GameImage>
  <GameImage>
    <DatabaseID>10</DatabaseID>
    <FileName>bbb.jpg</FileName>
    <Type>Box - 3D</Type>
  </GameImage>
  <GameImage>
    <DatabaseID>10</DatabaseID>
    <FileName>ccc.png</FileName>
    <Type>Clear Logo</Type>
  </GameImage>
  <GameImage>
    <DatabaseID>11</DatabaseID>
    <FileName>ddd.jpg</FileName>
    <Type>Box - Front</Type>
  </GameImage>
</LaunchBox>)";

    QBuffer buffer;
    buffer.setData(xml);
    REQUIRE(buffer.open(QIODevice::ReadOnly));

    QVector<Game> games;
    QString error;
    REQUIRE(ParseMetadata(buffer, &games, &error));
    REQUIRE(games.size() == 1);
    const Game& game = games.front();
    REQUIRE(game.database_id == 10);
    REQUIRE(game.name == QStringLiteral("Mario Kart World"));
    REQUIRE(game.year == 2025);
    REQUIRE(game.alternate_names == QStringList{QStringLiteral("MK World")});
    REQUIRE(game.box_front == QStringLiteral("aaa.jpg"));
    REQUIRE(game.box_3d == QStringLiteral("bbb.jpg"));
    REQUIRE(BestImage(game) == QStringLiteral("aaa.jpg"));

    const auto ranked = Rank(QStringLiteral("MK World"), QStringLiteral("Nintendo Switch 2"), games);
    REQUIRE_FALSE(ranked.isEmpty());
    REQUIRE(ranked.front().game.database_id == 10);

    REQUIRE(ImageTypeScore(QStringLiteral("Box - Front")) >
            ImageTypeScore(QStringLiteral("Box - 3D")));
    REQUIRE(ImageTypeScore(QStringLiteral("Clear Logo")) == 0);
    REQUIRE(ImageTypeScore(QStringLiteral("Banner")) == 0);
    REQUIRE(ImageTypeScore(QStringLiteral("Screenshot - Gameplay")) == 0);
    REQUIRE(ImageUrl(QStringLiteral("aaa.jpg")) ==
            QStringLiteral("https://gamesdb-images.launchbox.gg/aaa.jpg"));
}

TEST_CASE("LaunchBox index round-trips", "[launchbox]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("switch-index.bin"));

    const QVector<Game> games{
        MakeGame(10, QStringLiteral("Mario Kart World"), QStringLiteral("Nintendo Switch 2"), 2025,
                 {QStringLiteral("MK World")}, QStringLiteral("aaa.jpg"), QStringLiteral("bbb.jpg")),
        MakeGame(11, QStringLiteral("Kirby"), QStringLiteral("Nintendo Switch"), 2022),
    };
    QString error;
    REQUIRE(SaveIndex(games, path, &error));

    QVector<Game> loaded;
    REQUIRE(LoadIndex(path, &loaded, &error));
    REQUIRE(loaded.size() == games.size());
    REQUIRE(loaded[0].database_id == 10);
    REQUIRE(loaded[0].name == games[0].name);
    REQUIRE(loaded[0].platform == games[0].platform);
    REQUIRE(loaded[0].year == 2025);
    REQUIRE(loaded[0].alternate_names == games[0].alternate_names);
    REQUIRE(loaded[0].box_front == QStringLiteral("aaa.jpg"));
    REQUIRE(loaded[0].box_3d == QStringLiteral("bbb.jpg"));
    REQUIRE(loaded[1].name == QStringLiteral("Kirby"));
}
TEST_CASE("LaunchBox reads the real metadata package and image host", "[.][launchbox-live]") {
    const QString zip = QStringLiteral("C:/Users/Neku/lx/lichii316/launchbox-metadata.zip");
    if (!QFile::exists(zip)) {
        SKIP("no local metadata package");
    }
    QVector<Game> games;
    QString error;
    REQUIRE(BuildIndexFromZip(zip, &games, &error));
    REQUIRE(games.size() > 1000);

    const auto ranked = Rank(QStringLiteral("Mario Kart World"), QStringLiteral("Nintendo Switch 2"),
                             games);
    REQUIRE_FALSE(ranked.isEmpty());
    REQUIRE(ranked.front().game.name == QStringLiteral("Mario Kart World"));
    const QString image_file = BestImage(ranked.front().game);
    REQUIRE_FALSE(image_file.isEmpty());

    std::atomic_bool cancel{false};
    const ImageResult image = FetchImage(image_file, cancel);
    REQUIRE(image.status == ImageResult::Status::Ok);
    REQUIRE_FALSE(image.image.isNull());
}
