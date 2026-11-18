// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>

#include <atomic>

#include "lichii/art/launchbox.h"
#include "lichii/art/launchbox_net.h"

class QLabel;
class QListWidget;
class QPushButton;

/// Lets the player choose one LaunchBox candidate and its box art, with a preview.
class LaunchBoxArtDialog : public QDialog {
    Q_OBJECT
public:
    LaunchBoxArtDialog(QWidget* parent, const QString& title,
                       QVector<LaunchBox::Candidate> candidates);
    ~LaunchBoxArtDialog() override;

    /// The image of the chosen candidate, once its preview loaded.
    [[nodiscard]] const QImage& Image() const { return image; }

private:
    void Select(int row);
    void OnPreviewFinished();

    QVector<LaunchBox::Candidate> candidates;
    QListWidget* list{};
    QLabel* preview{};
    QLabel* detail{};
    QPushButton* use{};
    QFutureWatcher<LaunchBox::ImageResult> watcher;
    std::atomic_bool cancel{false};
    QImage image;
    int selected{-1};
    int fetching_row{-1};
    int pending_row{-1};
};