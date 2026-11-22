// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/util/cover_art.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/httplib.h"
#include "qt_common/util/game.h"
#ifdef LICHII_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace CoverArt {
namespace {

/// Cover and banner of Switch games, by title ID.
constexpr std::string_view NlibOrigin = "https://api.nlib.cc";
/// Nintendo's eShop search: art of Switch and Switch 2 games, by name.
constexpr std::string_view EshopSearchOrigin = "https://searching.nintendo-europe.com";
/// Where the eShop search's pictures are; no other host is fetched from its results.
constexpr std::string_view EshopImageOrigin = "https://www.nintendo.com";

// The LaunchBox Games Database: community artwork, its "Square" and "Fanart - Background" types.
constexpr std::string_view LaunchBoxOrigin = "https://gamesdb.launchbox-app.com";
constexpr std::string_view LaunchBoxImageOrigin = "https://images.launchbox-app.com";
// Its search and image pages are HTML, 100-300 KiB.
constexpr std::size_t MaxPageBytes = 2 * 1024 * 1024;
// LaunchBox fanart is often a large PNG.
constexpr std::size_t MaxImageBytes = 24 * 1024 * 1024;
constexpr std::size_t MaxSearchBytes = 512 * 1024;
// The home view shows covers at up to 440 logical pixels, ~900 on a high-DPI screen.
constexpr int CoverSide = 1024;
// The loading screen fills the window with the banner: kept up to 1440p so it stays sharp there.
constexpr int BannerWidth = 2560;
constexpr int BannerHeight = 1440;

enum class Fetch { Ok, NotFound, NetworkError, Cancelled };

/// GETs origin + path into body. NotFound for a 404 or a body larger than max_bytes.
Fetch Get(std::string_view origin, const std::string& path, std::size_t max_bytes,
          const std::atomic_bool& cancel, std::string& body, std::string& detail) {
    using namespace std::chrono_literals;
    httplib::Client client{std::string{origin}};
    client.set_connection_timeout(10s);
    client.set_read_timeout(15s);
    client.set_write_timeout(10s);
    client.set_max_timeout(30s);
    client.set_follow_location(true);
#ifdef LICHII_BUNDLED_OPENSSL
    client.load_ca_cert_store(kCert, sizeof(kCert));
#endif
    body.clear();
    int status = 0;
    bool too_large = false;
    const auto result = client.Get(
        path, httplib::Headers{{"User-Agent", "TuttiFrutti"}},
        [&status](const httplib::Response& response) {
            status = response.status;
            return response.status == 200;
        },
        [&](const char* data, std::size_t length) {
            if (cancel.load()) {
                return false;
            }
            if (body.size() + length > max_bytes) {
                too_large = true;
                return false;
            }
            body.append(data, length);
            return true;
        });
    if (cancel.load()) {
        return Fetch::Cancelled;
    }
    if (status == 404) {
        return Fetch::NotFound;
    }
    if (too_large) {
        detail = "response too large";
        return Fetch::NotFound;
    }
    if (!result || status != 200) {
        detail = result ? fmt::format("HTTP {}", status) : httplib::to_string(result.error());
        return Fetch::NetworkError;
    }
    return Fetch::Ok;
}

/// GETs one image. NotFound for a 404 or a body that is no picture.
Fetch GetImage(std::string_view origin, const std::string& path, const std::atomic_bool& cancel,
               QImage& image, std::string& detail) {
    std::string body;
    const Fetch fetch = Get(origin, path, MaxImageBytes, cancel, body, detail);
    if (fetch != Fetch::Ok) {
        return fetch;
    }
    if (!image.loadFromData(reinterpret_cast<const uchar*>(body.data()),
                            static_cast<int>(body.size())) ||
        image.isNull()) {
        detail = "not an image";
        return Fetch::NotFound;
    }
    return Fetch::Ok;
}

bool SaveJpeg(const std::filesystem::path& path, const QImage& image, int quality = 90) {
    if (!Common::FS::CreateDirs(path.parent_path())) {
        return false;
    }
    // Written to a temporary file and swapped in, so a failed save never leaves a broken image.
    QSaveFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    return file.open(QIODevice::WriteOnly) &&
           image.convertToFormat(QImage::Format_RGB888).save(&file, "JPG", quality) &&
           file.commit();
}

bool SaveCover(u64 program_id, const QImage& cover) {
    // Square, never enlarged: an upscaled picture only looks blurrier on a big card.
    const int side = std::min(CoverSide, std::max(cover.width(), cover.height()));
    return SaveJpeg(QtCommon::Game::DownloadedCoverPath(program_id),
                    cover.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                    94);
}

bool SaveBanner(u64 program_id, const QImage& banner) {
    const QImage wide = banner.width() > BannerWidth || banner.height() > BannerHeight
                            ? banner.scaled(BannerWidth, BannerHeight, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation)
                            : banner;
    return SaveJpeg(QtCommon::Game::DownloadedBannerPath(program_id), wide, 92);
}

void TouchMarker(u64 program_id) {
    const auto path = QtCommon::Game::NoCoverMarkerPath(program_id);
    if (Common::FS::CreateDirs(path.parent_path())) {
        QFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
        if (file.open(QIODevice::WriteOnly)) {
            file.close();
        }
    }
}

/// Switch 2 program IDs start with 04.
bool IsSwitch2(u64 program_id) {
    return (program_id >> 56) == 0x04;
}

/// Lower case letters and digits only, accents and trademark signs dropped, so that
/// "Pokémon™ Pokopia" and "Pokemon Pokopia" compare equal.
QString Normalize(const QString& title) {
    QString text = title;
    text.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    text = text.normalized(QString::NormalizationForm_KD).toLower();
    QString out;
    for (const QChar c : text) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
            (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            out.append(c);
        }
    }
    return out;
}

/// 3 for the same name, 2 when one name starts with the other and they are close in length
/// (an edition suffix), else 0.
int MatchScore(const QString& wanted, const QString& candidate) {
    if (wanted.isEmpty() || candidate.isEmpty()) {
        return 0;
    }
    if (wanted == candidate) {
        return 3;
    }
    const QString& shorter = wanted.size() < candidate.size() ? wanted : candidate;
    const QString& longer = wanted.size() < candidate.size() ? candidate : wanted;
    if (longer.startsWith(shorter) && shorter.size() * 10 >= longer.size() * 6) {
        return 2;
    }
    return 0;
}

/// The eShop's "500w" picture in another width, or the same URL when it has no width. "full" is
/// the picture as uploaded (the name without "_image500w"; 1080x1080 for square covers).
std::string ResizedImageUrl(const std::string& url, std::string_view width) {
    std::string out = url;
    if (const auto at = out.rfind("image500w"); at != std::string::npos) {
        if (width == "full") {
            const auto start = at > 0 && out[at - 1] == '_' ? at - 1 : at;
            out.erase(start, at + 9 - start);
        } else {
            out.replace(at + 5, 4, width);
        }
    }
    return out;
}

/// The path of an eShop picture, or empty for a URL on any other host.
std::string EshopImagePath(const std::string& url) {
    const std::string prefix = std::string{EshopImageOrigin} + "/";
    if (!url.starts_with(prefix) || url.size() > 2048) {
        return {};
    }
    return url.substr(EshopImageOrigin.size());
}

struct EshopArt {
    std::string square;
    std::string wide;
};

/// Finds the game by name in Nintendo's eShop search, among games of the same console.
std::optional<EshopArt> FindEshopArt(u64 program_id, const std::string& title,
                                     const std::atomic_bool& cancel, Fetch& fetch,
                                     std::string& detail) {
    const QString wanted = Normalize(QString::fromStdString(title));
    if (wanted.isEmpty()) {
        fetch = Fetch::NotFound;
        return std::nullopt;
    }
    // Characters the search reads as syntax are sent as spaces.
    QString query = QString::fromStdString(title);
    query.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    for (const QChar c : QStringLiteral("+-&|!(){}[]^\"~*?:\\/")) {
        query.replace(c, QLatin1Char(' '));
    }
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("q"), query.simplified());
    params.addQueryItem(QStringLiteral("fq"),
                        IsSwitch2(program_id)
                            ? QStringLiteral("type:GAME AND system_type:nintendoswitch2*")
                            : QStringLiteral("type:GAME AND system_type:nintendoswitch_*"));
    params.addQueryItem(QStringLiteral("rows"), QStringLiteral("8"));
    params.addQueryItem(QStringLiteral("wt"), QStringLiteral("json"));
    params.addQueryItem(
        QStringLiteral("fl"),
        QStringLiteral("title,title_extras_txt,image_url_sq_s,image_url_h16x9_s,image_url_h2x1_s"));
    const std::string path = "/en/select?" + params.query(QUrl::FullyEncoded).toStdString();

    std::string body;
    fetch = Get(EshopSearchOrigin, path, MaxSearchBytes, cancel, body, detail);
    if (fetch != Fetch::Ok) {
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(
        body, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) { return depth < 16; },
        false);
    if (json.is_discarded() || !json.contains("response") || !json["response"].is_object() ||
        !json["response"].contains("docs") || !json["response"]["docs"].is_array()) {
        fetch = Fetch::NotFound;
        return std::nullopt;
    }
    const auto text = [](const nlohmann::json& doc, const char* key) {
        return doc.contains(key) && doc[key].is_string() ? doc[key].get<std::string>()
                                                         : std::string{};
    };

    int best_score = 0;
    EshopArt best;
    for (const auto& doc : json["response"]["docs"]) {
        if (!doc.is_object()) {
            continue;
        }
        int score = MatchScore(wanted, Normalize(QString::fromStdString(text(doc, "title"))));
        if (doc.contains("title_extras_txt") && doc["title_extras_txt"].is_array()) {
            for (const auto& extra : doc["title_extras_txt"]) {
                if (extra.is_string()) {
                    score = std::max(score, MatchScore(wanted, Normalize(QString::fromStdString(
                                                                   extra.get<std::string>()))));
                }
            }
        }
        // The first result wins a tie: the search ranks the closest match first.
        if (score > best_score) {
            std::string wide = text(doc, "image_url_h16x9_s");
            if (wide.empty()) {
                wide = text(doc, "image_url_h2x1_s");
            }
            best_score = score;
            best = {text(doc, "image_url_sq_s"), std::move(wide)};
        }
    }
    if (best_score < 2) {
        fetch = Fetch::NotFound;
        return std::nullopt;
    }
    return best;
}

QString HtmlText(QString text) {
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"))
        .replace(QStringLiteral("&#39;"), QStringLiteral("'"))
        .replace(QStringLiteral("&#x27;"), QStringLiteral("'"))
        .replace(QStringLiteral("&quot;"), QStringLiteral("\""))
        .replace(QStringLiteral("&lt;"), QStringLiteral("<"))
        .replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return text.trimmed();
}

/// Cuts a "Nintendo Switch 2 Edition" suffix off a name ("The Legend of Zelda: Breath of the
/// Wild: Nintendo Switch 2 Edition", "Kirby and the Forgotten Land: Nintendo Switch 2 Edition +
/// Star-Crossed World") and says whether there was one.
bool StripSwitch2Edition(QString& title) {
    static const QRegularExpression edition_re(
        QStringLiteral(R"re(\s*[:\-\x{2013}\x{2014}]?\s*Nintendo\s+Switch\s*2\s+Edition\b.*$)re"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = edition_re.match(title);
    if (!match.hasMatch()) {
        return false;
    }
    title.truncate(match.capturedStart());
    return true;
}

struct LaunchBoxArt {
    /// Paths on LaunchBoxImageOrigin, full size.
    std::string square;
    std::string fanart;
};

/// Finds the game by name in the LaunchBox Games Database and picks its largest "Square" and
/// "Fanart - Background" pictures, preferring the world, American and European releases. Of the
/// entries with the game's name, its Nintendo Switch 2 Edition wins (LaunchBox lists those as
/// their own Switch 2 games), then any other Switch 2 entry, then the Switch one.
std::optional<LaunchBoxArt> FindLaunchBoxArt(const std::string& title,
                                             const std::atomic_bool& cancel, Fetch& fetch,
                                             std::string& detail) {
    // The name without an edition suffix: it matches the base game and its Switch 2 Edition.
    QString base_title = QString::fromStdString(title);
    StripSwitch2Edition(base_title);
    const QString wanted = Normalize(base_title);
    QString query = base_title;
    query.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    for (const QChar c : QStringLiteral("/\\?#%&:")) {
        query.replace(c, QLatin1Char(' '));
    }
    query = query.simplified().toLower();
    if (wanted.isEmpty() || query.isEmpty()) {
        fetch = Fetch::NotFound;
        return std::nullopt;
    }

    std::string body;
    fetch = Get(LaunchBoxOrigin, "/games/results/" + QUrl::toPercentEncoding(query).toStdString(),
                MaxPageBytes, cancel, body, detail);
    if (fetch != Fetch::Ok) {
        return std::nullopt;
    }

    // <a ... href="/games/details/423668-mario-kart-world"> ... <h3>Mario Kart World</h3>
    // <p>Nintendo Switch 2</p>
    static const QRegularExpression card_re(
        QStringLiteral(R"re(href="/games/details/(\d+)-[^"]*"[\s\S]{0,2000}?<h3[^>]*>([^<]*)</h3>\s*<p[^>]*>([^<]*)</p>)re"));
    int best_score = 0;
    QString best_id;
    auto cards = card_re.globalMatch(QString::fromStdString(body));
    while (cards.hasNext()) {
        const auto card = cards.next();
        const QString platform = HtmlText(card.captured(3));
        const bool switch2 = platform == QStringLiteral("Nintendo Switch 2");
        if (!switch2 && platform != QStringLiteral("Nintendo Switch")) {
            continue;
        }
        QString name = HtmlText(card.captured(2));
        const bool edition = StripSwitch2Edition(name);
        const int name_score = MatchScore(wanted, Normalize(name));
        if (name_score < 2) {
            continue;
        }
        // The Switch 2 Edition first, then Switch 2, then the closer name; the first result
        // wins a tie (LaunchBox ranks the closest match first).
        const int score = name_score + (switch2 ? 10 : 0) + (switch2 && edition ? 20 : 0);
        if (score > best_score) {
            best_score = score;
            best_id = card.captured(1);
        }
    }
    if (best_id.isEmpty()) {
        fetch = Fetch::NotFound;
        return std::nullopt;
    }

    fetch = Get(LaunchBoxOrigin, "/games/images/" + best_id.toStdString(), MaxPageBytes, cancel,
                body, detail);
    if (fetch != Fetch::Ok) {
        return std::nullopt;
    }

    // <a href="https://images.launchbox-app.com/r2_....png" ... data-title="Mario Kart World -
    // Square Image (Japan)" data-footer="1024 x 1024 PNG, 1.84 MB...">
    static const QRegularExpression image_re(
        QStringLiteral(R"re(<a\b[^>]*?href="https://images\.launchbox-app\.com(/[^"]+)"[^>]*?data-title="([^"]*)"[^>]*?data-footer="(\d+) x (\d+))re"));
    const auto region_rank = [](const QString& type) {
        static const QStringList preferred{
            QStringLiteral("(World)"),         QStringLiteral("(North America)"),
            QStringLiteral("(United States)"), QStringLiteral("(Europe)"),
            QStringLiteral("(United Kingdom)"),
        };
        for (qsizetype i = 0; i < preferred.size(); ++i) {
            if (type.endsWith(preferred[i])) {
                return static_cast<int>(i);
            }
        }
        return type.endsWith(QLatin1Char(')')) ? 10 : 5; // No region: about as good as any.
    };
    struct Pick {
        int rank = 1000;
        qint64 area = 0;
        std::string path;
    };
    Pick square;
    Pick fanart;
    const auto consider = [&region_rank](Pick& pick, const QString& type, qint64 area,
                                         const QString& path) {
        const int rank = region_rank(type);
        // A picture under 600 px a side is only taken when there is nothing larger.
        const int size_penalty = area < 600LL * 600LL ? 100 : 0;
        if (rank + size_penalty < pick.rank ||
            (rank + size_penalty == pick.rank && area > pick.area)) {
            pick = {rank + size_penalty, area, path.toStdString()};
        }
    };
    auto images = image_re.globalMatch(QString::fromStdString(body));
    while (images.hasNext()) {
        const auto image = images.next();
        const QString type = HtmlText(image.captured(2));
        const qint64 area = image.captured(3).toLongLong() * image.captured(4).toLongLong();
        if (type.contains(QStringLiteral("- Square Image"))) {
            consider(square, type, area, image.captured(1));
        } else if (type.contains(QStringLiteral("- Fanart - Background Image"))) {
            // Backgrounds carry no text worth a region: the largest one is the sharpest.
            consider(fanart, QStringLiteral("(World)"), area, image.captured(1));
        }
    }
    if (square.path.empty() && fanart.path.empty()) {
        fetch = Fetch::NotFound;
        return std::nullopt;
    }
    fetch = Fetch::Ok;
    return LaunchBoxArt{std::move(square.path), std::move(fanart.path)};
}

/// GETs an eShop picture, first in the given larger widths in order, then as listed.
Fetch GetEshopImage(const std::string& url, std::initializer_list<std::string_view> larger_widths,
                    const std::atomic_bool& cancel, QImage& image, std::string& detail) {
    const std::string path = EshopImagePath(url);
    if (path.empty()) {
        return Fetch::NotFound;
    }
    for (const std::string_view width : larger_widths) {
        const std::string larger = EshopImagePath(ResizedImageUrl(url, width));
        if (larger.empty() || larger == path) {
            continue;
        }
        const Fetch fetch = GetImage(EshopImageOrigin, larger, cancel, image, detail);
        if (fetch != Fetch::NotFound) {
            return fetch;
        }
    }
    return GetImage(EshopImageOrigin, path, cancel, image, detail);
}

} // Anonymous namespace

bool IsSettled(u64 program_id) {
    std::error_code ec;
    return std::filesystem::exists(QtCommon::Game::DownloadedCoverPath(program_id), ec) ||
           std::filesystem::exists(QtCommon::Game::NoCoverMarkerPath(program_id), ec);
}

Result Download(u64 program_id, const std::string& title, const std::atomic_bool& cancel) {
    Result result;
    if (program_id == 0) {
        return result;
    }
    bool network_error = false;
    const auto settle = [&](Fetch fetch) {
        if (fetch == Fetch::Cancelled) {
            result.status = Status::Cancelled;
            return true;
        }
        network_error |= fetch == Fetch::NetworkError;
        return false;
    };

    const bool has_name = !Normalize(QString::fromStdString(title)).isEmpty();

    // 1. The LaunchBox Games Database by name: its "Square" picture for the cover and its
    // background fanart for the banner, both full size.
    if (has_name) {
        Fetch search = Fetch::NotFound;
        const auto art = FindLaunchBoxArt(title, cancel, search, result.detail);
        if (settle(search)) {
            return result;
        }
        if (art && !art->square.empty()) {
            QImage cover;
            if (settle(GetImage(LaunchBoxImageOrigin, art->square, cancel, cover,
                                result.detail))) {
                return result;
            }
            if (!cover.isNull()) {
                result.cover_saved = SaveCover(program_id, cover);
            }
        }
        if (art && !art->fanart.empty()) {
            QImage banner;
            if (settle(GetImage(LaunchBoxImageOrigin, art->fanart, cancel, banner,
                                result.detail))) {
                return result;
            }
            if (!banner.isNull()) {
                result.banner_saved = SaveBanner(program_id, banner);
            }
        }
    }

    // 2. nlib.cc by title ID: Switch games only, for what LaunchBox does not have.
    if (!IsSwitch2(program_id) && (!result.cover_saved || !result.banner_saved)) {
        const std::string id = fmt::format("{:016X}", program_id);
        if (!result.cover_saved) {
            QImage cover;
            if (settle(GetImage(NlibOrigin, fmt::format("/nx/{}/icon", id), cancel, cover,
                                result.detail))) {
                return result;
            }
            if (!cover.isNull()) {
                result.cover_saved = SaveCover(program_id, cover);
            }
        }
        if (!result.banner_saved) {
            QImage banner;
            if (settle(GetImage(NlibOrigin, fmt::format("/nx/{}/banner", id), cancel, banner,
                                result.detail))) {
                return result;
            }
            if (!banner.isNull()) {
                result.banner_saved = SaveBanner(program_id, banner);
            }
        }
    }

    // 3. Nintendo's eShop search by name: what neither LaunchBox nor nlib.cc has. A game
    // without a readable name (missing keys, say) is not searched, and not marked either: it is
    // tried again once its name can be read.
    if (has_name && (!result.cover_saved || !result.banner_saved)) {
        Fetch search = Fetch::NotFound;
        const auto art = FindEshopArt(program_id, title, cancel, search, result.detail);
        if (settle(search)) {
            return result;
        }
        if (art && !result.cover_saved && !art->square.empty()) {
            QImage cover;
            // The listed square picture is 500 pixels wide: the full one (1080) or the 950 one is
            // sharp on a big card.
            if (settle(GetEshopImage(art->square, {"full", "950w"}, cancel, cover,
                                     result.detail))) {
                return result;
            }
            if (!cover.isNull()) {
                result.cover_saved = SaveCover(program_id, cover);
            }
        }
        if (art && !result.banner_saved && !art->wide.empty()) {
            QImage banner;
            if (settle(GetEshopImage(art->wide, {"1600w"}, cancel, banner, result.detail))) {
                return result;
            }
            if (!banner.isNull()) {
                result.banner_saved = SaveBanner(program_id, banner);
            }
        }
    }

    if (result.cover_saved || result.banner_saved) {
        result.status = Status::Saved;
        std::error_code ec;
        std::filesystem::remove(QtCommon::Game::NoCoverMarkerPath(program_id), ec);
    } else if (network_error) {
        result.status = Status::NetworkError;
    } else if (!has_name) {
        result.status = Status::NotFound;
    } else {
        // Neither service has this game: it is not asked again on every launch.
        result.status = Status::NotFound;
        TouchMarker(program_id);
    }
    return result;
}

} // namespace CoverArt
