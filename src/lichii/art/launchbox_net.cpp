// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/art/launchbox_net.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <quazip.h>
#include <quazipfile.h>

#include <cstdlib>
#include <string>
#include <string_view>

#include "common/httplib.h"

namespace LaunchBox {
namespace {

constexpr const char* MetadataHost = "https://gamesdb.launchbox-app.com";
constexpr const char* MetadataPath = "/Metadata.zip";
constexpr const char* ImageHost = "https://gamesdb-images.launchbox.gg/";
constexpr qint64 MaxMetadataBytes = 2LL << 30;

bool SafeImageName(const QString& file_name) {
    if (file_name.isEmpty() || file_name.size() > 260) {
        return false;
    }
    for (const QChar ch : file_name) {
        if (!ch.isLetterOrNumber() && ch != u'.' && ch != u'-' && ch != u'_') {
            return false;
        }
    }
    return !file_name.contains(QStringLiteral(".."));
}

}  // namespace

bool BuildIndexFromZip(const QString& zip_path, QVector<Game>* out, QString* error,
                       const std::function<bool(int)>& report) {
    QuaZip zip(zip_path);
    if (!zip.open(QuaZip::mdUnzip)) {
        if (error) {
            *error = QStringLiteral("cannot open the metadata package");
        }
        return false;
    }
    if (!zip.setCurrentFile(QStringLiteral("Metadata.xml"))) {
        if (error) {
            *error = QStringLiteral("the package has no Metadata.xml");
        }
        return false;
    }
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot read Metadata.xml from the package");
        }
        return false;
    }
    if (file.size() <= 0 || file.size() > MaxMetadataBytes) {
        if (error) {
            *error = QStringLiteral("Metadata.xml has an unusable size");
        }
        return false;
    }
    if (report && !report(5)) {
        if (error) {
            *error = QStringLiteral("cancelled");
        }
        return false;
    }
    const bool ok = ParseMetadata(file, out, error);
    if (ok && report && !report(100)) {
        if (error) {
            *error = QStringLiteral("cancelled");
        }
        return false;
    }
    return ok;
}

bool DownloadMetadata(const QString& dest, const std::atomic_bool& cancel, QString* error,
                      const std::function<bool(qint64, qint64)>& report) {
    httplib::Client client(MetadataHost);
    client.set_follow_location(true);
    client.set_connection_timeout(30);
    client.set_read_timeout(180);
    client.set_write_timeout(180);

    qint64 total = 0;
    if (const auto head = client.Head(MetadataPath); head && head->status == 200) {
        total = std::strtoll(head->get_header_value("Content-Length").c_str(), nullptr, 10);
    }

    if (!QDir().mkpath(QFileInfo(dest).absolutePath())) {
        if (error) {
            *error = QStringLiteral("cannot create the cache folder");
        }
        return false;
    }
    const QString part = dest + QStringLiteral(".part");
    QFile file(part);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    qint64 received = 0;
    bool write_failed = false;
    auto response = client.Get(
        MetadataPath, [&](const char* data, size_t length) {
            if (cancel.load() || write_failed) {
                return false;
            }
            if (file.write(data, qint64(length)) != qint64(length)) {
                write_failed = true;
                return false;
            }
            received += qint64(length);
            return !report || report(received, total);
        });
    file.close();

    if (cancel.load()) {
        QFile::remove(part);
        if (error) {
            *error = QStringLiteral("cancelled");
        }
        return false;
    }
    if (write_failed) {
        QFile::remove(part);
        if (error) {
            *error = QStringLiteral("cannot write the metadata package");
        }
        return false;
    }
    if (!response) {
        QFile::remove(part);
        if (error) {
            *error = QStringLiteral("the metadata package could not be downloaded");
        }
        return false;
    }
    if (response->status != 200) {
        QFile::remove(part);
        if (error) {
            *error = QStringLiteral("the metadata server answered %1").arg(response->status);
        }
        return false;
    }
    if (total > 0 && received != total) {
        QFile::remove(part);
        if (error) {
            *error = QStringLiteral("the metadata package is incomplete");
        }
        return false;
    }
    QFile::remove(dest);
    if (!QFile::rename(part, dest)) {
        QFile::remove(part);
        if (error) {
            *error = QStringLiteral("cannot move the metadata package into place");
        }
        return false;
    }
    if (report) {
        report(received, received);
    }
    return true;
}

ImageResult FetchImage(const QString& file_name, const std::atomic_bool& cancel) {
    ImageResult result;
    if (!SafeImageName(file_name)) {
        result.status = ImageResult::Status::NotFound;
        result.detail = QStringLiteral("the index holds an unusable image name");
        return result;
    }
    if (cancel.load()) {
        result.status = ImageResult::Status::Cancelled;
        return result;
    }

    httplib::Client client(ImageHost);
    client.set_follow_location(true);
    client.set_connection_timeout(15);
    client.set_read_timeout(30);
    auto response = client.Get("/" + file_name.toStdString());
    if (cancel.load()) {
        result.status = ImageResult::Status::Cancelled;
        return result;
    }
    if (!response) {
        result.status = (response.error() == httplib::Error::Read ||
                             response.error() == httplib::Error::ConnectionTimeout)
                            ? ImageResult::Status::Timeout
                            : ImageResult::Status::Network;
        result.detail = QStringLiteral("the image could not be downloaded");
        return result;
    }
    if (response->status == 404) {
        result.status = ImageResult::Status::NotFound;
        result.detail = QStringLiteral("the image no longer exists");
        return result;
    }
    if (response->status != 200) {
        result.status = ImageResult::Status::Network;
        result.detail = QStringLiteral("the image server answered %1").arg(response->status);
        return result;
    }
    const std::string type = response->get_header_value("Content-Type");
    if (!type.starts_with("image/")) {
        result.status = ImageResult::Status::BadType;
        result.detail = QStringLiteral("the server did not return an image");
        return result;
    }
    if (qint64(response->body.size()) > MaxImageBytes) {
        result.status = ImageResult::Status::TooLarge;
        result.detail = QStringLiteral("the image is larger than %1 MiB").arg(MaxImageBytes >> 20);
        return result;
    }
    if (!result.image.loadFromData(reinterpret_cast<const uchar*>(response->body.data()),
                                   int(response->body.size()))) {
        result.status = ImageResult::Status::Decode;
        result.detail = QStringLiteral("the image could not be decoded");
        return result;
    }
    result.status = ImageResult::Status::Ok;
    return result;
}

}  // namespace LaunchBox