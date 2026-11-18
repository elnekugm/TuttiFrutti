// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/art/launchbox_dialog.h"

#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

LaunchBoxArtDialog::LaunchBoxArtDialog(QWidget* parent, const QString& title,
                                       QVector<LaunchBox::Candidate> candidates_)
    : QDialog(parent), candidates(std::move(candidates_)) {
    setWindowTitle(tr("Artwork for %1").arg(title));
    resize(760, 460);

    auto* layout = new QVBoxLayout(this);
    auto* body = new QHBoxLayout;
    layout->addLayout(body, 1);

    list = new QListWidget(this);
    list->setMinimumWidth(380);
    for (const LaunchBox::Candidate& candidate : candidates) {
        const LaunchBox::Game& game = candidate.game;
        QString text = game.name;
        if (!game.platform.isEmpty()) {
            text += QStringLiteral(" — ") + game.platform;
        }
        if (game.year != 0) {
            text += QStringLiteral(" (%1)").arg(game.year);
        }
        text += QStringLiteral("  ·  %1%").arg(int(candidate.score * 100 + 0.5));
        list->addItem(text);
    }
    body->addWidget(list);

    auto* side = new QVBoxLayout;
    preview = new QLabel(this);
    preview->setMinimumSize(320, 320);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameShape(QFrame::StyledPanel);
    preview->setText(tr("Choose a result"));
    side->addWidget(preview, 1);
    detail = new QLabel(this);
    detail->setWordWrap(true);
    side->addWidget(detail);
    body->addLayout(side, 1);

    auto* buttons = new QDialogButtonBox(this);
    use = buttons->addButton(tr("Use This Artwork"), QDialogButtonBox::AcceptRole);
    use->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(list, &QListWidget::currentRowChanged, this,
            [this](int row) { Select(row); });
    connect(use, &QPushButton::clicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&watcher, &QFutureWatcher<LaunchBox::ImageResult>::finished, this,
            &LaunchBoxArtDialog::OnPreviewFinished);

    if (!candidates.isEmpty()) {
        list->setCurrentRow(0);
    }
}

LaunchBoxArtDialog::~LaunchBoxArtDialog() {
    cancel.store(true);
    watcher.waitForFinished();
}

void LaunchBoxArtDialog::Select(int row) {
    if (row < 0 || row >= candidates.size()) {
        return;
    }
    const LaunchBox::Game& game = candidates[row].game;
    const QString file = LaunchBox::BestImage(game);
    image = QImage{};
    use->setEnabled(false);
    detail->setText(file.isEmpty() ? tr("This result has no box art.")
                                   : tr("Loading the preview…"));
    preview->setText(file.isEmpty() ? QString{} : tr("Loading…"));
    pending_row = row;
    if (file.isEmpty() || watcher.isRunning()) {
        return;
    }
    cancel.store(false);
    fetching_row = row;
    watcher.setFuture(QtConcurrent::run([file, cancel = &cancel] {
        return LaunchBox::FetchImage(file, *cancel);
    }));
}

void LaunchBoxArtDialog::OnPreviewFinished() {
    const LaunchBox::ImageResult result = watcher.result();
    if (pending_row != fetching_row) {
        Select(pending_row);
        return;
    }
    if (pending_row < 0 || pending_row >= candidates.size()) {
        return;
    }
    if (result.status != LaunchBox::ImageResult::Status::Ok) {
        detail->setText(result.detail.isEmpty() ? tr("The preview could not be loaded.")
                                                : result.detail);
        preview->setText(tr("No preview"));
        return;
    }
    image = result.image;
    preview->setPixmap(QPixmap::fromImage(image).scaled(
        preview->size() - QSize(8, 8), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    detail->setText(tr("%1 × %2").arg(image.width()).arg(image.height()));
    selected = pending_row;
    use->setEnabled(true);
}