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

constexpr std::size_t MaxImageBytes = 8 * 1024 * 1024;
constexpr std::size_t MaxSearchBytes = 512 * 1024;
constexpr int CoverSide = 512; // The library shows covers at up to 512 pixels.
constexpr int BannerWidth = 1920;
constexpr int BannerHeight = 1080;

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

bool SaveJpeg(const std::filesystem::path& path, const QImage& image) {
    if (!Common::FS::CreateDirs(path.parent_path())) {
        return false;
    }
    // Written to a temporary file and swapped in, so a failed save never leaves a broken image.
    QSaveFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    return file.open(QIODevice::WriteOnly) &&
           image.convertToFormat(QImage::Format_RGB888).save(&file, "JPG", 90) && file.commit();
}

bool SaveCover(u64 program_id, const QImage& cover) {
    return SaveJpeg(
        QtCommon::Game::DownloadedCoverPath(program_id),
        cover.scaled(CoverSide, CoverSide, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

bool SaveBanner(u64 program_id, const QImage& banner) {
    const QImage wide = banner.width() > BannerWidth || banner.height() > BannerHeight
                            ? banner.scaled(BannerWidth, BannerHeight, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation)
                            : banner;
    return SaveJpeg(QtCommon::Game::DownloadedBannerPath(program_id), wide);
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
/// "PokÃ©monâ„¢ Pokopia" and "Pokemon Pokopia" compare equal.
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

/// The eShop's "500w" picture in another width, or the same URL when it has no width.
std::string ResizedImageUrl(const std::string& url, std::string_view width) {
    std::string out = url;
    if (const auto at = out.rfind("image500w"); at != std::string::npos) {
        out.replace(at + 5, 4, width);
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

/// GETs an eShop picture, first in the given larger width, then as listed.
Fetch GetEshopImage(const std::string& url, std::string_view larger_width,
                    const std::atomic_bool& cancel, QImage& image, std::string& detail) {
    const std::string path = EshopImagePath(url);
    if (path.empty()) {
        return Fetch::NotFound;
    }
    if (!larger_width.empty()) {
        const std::string larger = EshopImagePath(ResizedImageUrl(url, larger_width));
        if (larger != path) {
            const Fetch fetch = GetImage(EshopImageOrigin, larger, cancel, image, detail);
            if (fetch != Fetch::NotFound) {
                return fetch;
            }
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

    // 1. nlib.cc by title ID: Switch games only.
    if (!IsSwitch2(program_id)) {
        const std::string id = fmt::format("{:016X}", program_id);
        QImage cover;
        if (settle(GetImage(NlibOrigin, fmt::format("/nx/{}/icon", id), cancel, cover,
                            result.detail))) {
            return result;
        }
        if (!cover.isNull()) {
            result.cover_saved = SaveCover(program_id, cover);
        }
        QImage banner;
        if (settle(GetImage(NlibOrigin, fmt::format("/nx/{}/banner", id), cancel, banner,
                            result.detail))) {
            return result;
        }
        if (!banner.isNull()) {
            result.banner_saved = SaveBanner(program_id, banner);
        }
    }

    // 2. Nintendo's eShop search by name: Switch 2 games, and what nlib.cc does not have. A game
    // without a readable name (missing keys, say) is not searched, and not marked either: it is
    // tried again once its name can be read.
    const bool has_name = !Normalize(QString::fromStdString(title)).isEmpty();
    if (has_name && (!result.cover_saved || !result.banner_saved)) {
        Fetch search = Fetch::NotFound;
        const auto art = FindEshopArt(program_id, title, cancel, search, result.detail);
        if (settle(search)) {
            return result;
        }
        if (art && !result.cover_saved && !art->square.empty()) {
            QImage cover;
            if (settle(GetEshopImage(art->square, {}, cancel, cover, result.detail))) {
                return result;
            }
            if (!cover.isNull()) {
                result.cover_saved = SaveCover(program_id, cover);
            }
        }
        if (art && !result.banner_saved && !art->wide.empty()) {
            QImage banner;
            if (settle(GetEshopImage(art->wide, "1600w", cancel, banner, result.detail))) {
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
