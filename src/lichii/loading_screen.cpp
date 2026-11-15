// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>
#include <QColor>
#include <QEvent>
#include <QFontDatabase>
#include <QGraphicsOpacityEffect>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyleOption>
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "qt_common/config/shared_translation.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "ui_loading_screen.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "lichii/loading_screen.h"
#include "lichii/util/controller_navigation.h"

namespace {

/// Royal blue, the Lichii theme's accent.
const QColor Accent{0x41, 0x69, 0xe1};
/// About 30 frames a second: smooth enough for a slow pan, light on a CPU busy with shaders.
constexpr int FrameIntervalMs = 33;
/// One pan there and back.
constexpr double PanPeriodSeconds = 44.0;
/// The backdrop is this much larger than the window, the room the pan moves in.
constexpr double PanMargin = 0.06;
/// The moving segment of the progress line crosses the window in this time.
constexpr double SweepSeconds = 1.2;

bool IsCounting(VideoCore::LoadCallbackStage stage) {
    return stage == VideoCore::LoadCallbackStage::Build ||
           stage == VideoCore::LoadCallbackStage::Warm;
}

/// Blurs by scaling down to 1/factor of the size and back up in two steps, which leaves no
/// blocky edges.
QImage Soften(const QImage& image, double factor) {
    const QSize size = image.size();
    const QSize small = (QSizeF(size) / factor).toSize().expandedTo(QSize{1, 1});
    const QSize middle = (QSizeF(size) / std::sqrt(factor)).toSize().expandedTo(QSize{1, 1});
    return image.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(middle, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QString FormatTimeLeft(long msecs) {
    const long seconds = std::max<long>(msecs / 1000, 1);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // Anonymous namespace

LoadingScreen::LoadingScreen(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::LoadingScreen>()),
      previous_stage(VideoCore::LoadCallbackStage::Complete) {
    ui->setupUi(this);
    setMinimumSize(Layout::MinimumSize::Width, Layout::MinimumSize::Height);

    // Fades to black: the opacity effect is on an inner widget, while this widget stays black.
    // The effect is only enabled for the fade, so the panning art is not drawn twice a frame.
    opacity_effect = new QGraphicsOpacityEffect(this);
    opacity_effect->setOpacity(1);
    opacity_effect->setEnabled(false);
    ui->fade_parent->setGraphicsEffect(opacity_effect);
    fadeout_animation = std::make_unique<QPropertyAnimation>(opacity_effect, "opacity");
    fadeout_animation->setDuration(500);
    fadeout_animation->setStartValue(1);
    fadeout_animation->setEndValue(0);
    fadeout_animation->setEasingCurve(QEasingCurve::OutBack);

    // After the fade completes, hide the widget and reset the opacity
    connect(fadeout_animation.get(), &QPropertyAnimation::finished, [this] {
        hide();
        opacity_effect->setOpacity(1);
        opacity_effect->setEnabled(false);
        emit Hidden();
    });
    connect(this, &LoadingScreen::LoadProgress, this, &LoadingScreen::OnLoadProgress,
            Qt::QueuedConnection);

// The art, the shades and the progress line are painted under the labels.
    ui->fade_parent->installEventFilter(this);
    frame_timer.setInterval(FrameIntervalMs);
    connect(&frame_timer, &QTimer::timeout, ui->fade_parent, qOverload<>(&QWidget::update));

    ui->eyebrow->setText(tr("Now loading").toUpper());
    ui->title->setWordWrap(true);
    ui->meta->setWordWrap(true);
    // The title block and the status sit on the same baseline at the bottom.
    ui->bottom_row->setAlignment(ui->info, Qt::AlignLeft | Qt::AlignBottom);
    ui->bottom_row->setAlignment(ui->status, Qt::AlignRight | Qt::AlignBottom);
    ui->stage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // Shown while shaders are precached: starts the game without waiting for the rest. Its tooltip
    // is set per stage (OnLoadProgress).
    skip_button = new QPushButton(tr("Skip"), this);
    skip_button->setCursor(Qt::PointingHandCursor);
    skip_button->hide();
    ui->top_row->addWidget(skip_button, 0, Qt::AlignRight | Qt::AlignTop);
    connect(skip_button, &QPushButton::clicked, this, &LoadingScreen::ConfirmSkip);

    // A controller can skip too: A asks and A again confirms, B goes back.
    if (QtCommon::system) {
        controller_navigation = new ControllerNavigation(QtCommon::system->HIDCore(), this);
        connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
                &LoadingScreen::OnControllerKey);
    }
    qRegisterMetaType<VideoCore::LoadCallbackStage>();

    ApplyScale();
}

LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::SetGameDetails(u64 program_id, const QString& title, const QString& developer,
                                   const QString& version) {
    details_program_id = program_id;
    details_title = title;
    details_developer = developer;
    details_version = version;
}

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    u64 program_id = 0;
    loader.ReadProgramId(program_id);
    const bool have_details = program_id != 0 && program_id == details_program_id;

    QString title = have_details ? details_title : QString{};
    if (title.isEmpty()) {
        std::string name;
        if (loader.ReadTitle(name) == Loader::ResultStatus::Success) {
            title = QString::fromStdString(name);
        }
    }
    ui->title->setText(title);
    ui->title->setVisible(!title.isEmpty());

    QStringList meta;
    if (have_details && !details_developer.isEmpty()) {
        meta.append(details_developer);
    }
    if (have_details && !details_version.isEmpty()) {
        meta.append(tr("v%1").arg(details_version));
    }
    ui->meta->setText(meta.join(QStringLiteral(" · ")));
    ui->meta->setVisible(!meta.isEmpty());

    art_program_id = program_id;
    fallback_icon.clear();
    if (loader.ReadIcon(fallback_icon) != Loader::ResultStatus::Success) {
        fallback_icon.clear();
    }
    LoadArt();
    RebuildBackdrop();

    slow_shader_compile_start = false;
    current_value = 0;
    clock.restart();
    OnLoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
}

void LoadingScreen::LoadArt() {
    // The widest picture first: the downloaded banner fills the screen best.
    const auto load = [this](const std::vector<u8>& bytes) {
        if (bytes.empty()) {
            return false;
        }
        QImage image;
        if (!image.loadFromData(bytes.data(), static_cast<int>(bytes.size())) || image.isNull()) {
            return false;
        }
        art = std::move(image);
        return true;
    };
    art = QImage{};
    if (!load(QtCommon::Game::ReadDownloadedBanner(art_program_id)) &&
        !load(QtCommon::Game::ReadCustomArtwork(art_program_id)) &&
        !load(QtCommon::Game::ReadDownloadedCover(art_program_id))) {
        load(fallback_icon);
    }
}

void LoadingScreen::ReloadArt(u64 program_id) {
    if (program_id == 0 || program_id != art_program_id) {
        return;
    }
    LoadArt();
    RebuildBackdrop();
    ui->fade_parent->update();
}

void LoadingScreen::OnLoadComplete() {
    opacity_effect->setEnabled(true);
    fadeout_animation->start(QPropertyAnimation::KeepWhenStopped);
}

void LoadingScreen::OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value,
                                   std::size_t total) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    // reset the timer if the stage changes
    if (stage != previous_stage) {
skip_button->setVisible(
            stage == VideoCore::LoadCallbackStage::Warm ||
            (stage == VideoCore::LoadCallbackStage::Build &&
             VideoCommon::ShaderWarm::PrecompileCachedWaiting()));
        skip_button->setEnabled(true);
        // Preparation can finish while its confirmation is still open.
        if (skip_confirm) {
            skip_confirm->close();
        }
        previous_stage = stage;
        // reset back to fast shader compiling since the stage changed
        slow_shader_compile_start = false;
    }
    previous_total = total;
    current_value = value;

    QString time_left;
    // If there's a drastic slowdown in the rate, then display an estimate
    if (now - previous_time > milliseconds{50} || slow_shader_compile_start) {
        if (!slow_shader_compile_start) {
            slow_shader_start = steady_clock::now();
            slow_shader_compile_start = true;
            slow_shader_first_value = value;
        }
        // only calculate an estimate time after a second has passed since stage change
        const auto diff = duration_cast<milliseconds>(now - slow_shader_start);
        if (diff > seconds{1} && value > slow_shader_first_value) {
            const auto eta_mseconds =
                static_cast<long>(static_cast<double>(total - slow_shader_first_value) /
                                  (value - slow_shader_first_value) * diff.count());
            time_left = tr("%1 left").arg(
                FormatTimeLeft(std::max<long>(eta_mseconds - diff.count() + 1000, 1000)));
        }
    }

switch (stage) {
    case VideoCore::LoadCallbackStage::Prepare:
        ui->stage->setText(tr("Loading..."));
        break;
    case VideoCore::LoadCallbackStage::Build:
        ui->stage->setText(tr("Loading shaders"));
        break;
    case VideoCore::LoadCallbackStage::Warm:
        ui->stage->setText(tr("Precaching shaders"));
        break;
    case VideoCore::LoadCallbackStage::Complete:
    default:
        ui->stage->setText(tr("Launching..."));
        break;
    }
    if (IsCounting(stage)) {
        QString count = tr("%1 / %2").arg(value).arg(total);
        if (!time_left.isEmpty()) {
            count += QStringLiteral(" · ") + time_left;
        }
        ui->value->setText(count);
    } else {
        ui->value->clear();
    }
    ui->fade_parent->update();
    previous_time = now;
}

void LoadingScreen::ConfirmSkip() {
    if (skip_confirm) {
        return;
    }
    // On the main window: the loading screen's own style sheet would otherwise apply to the dialog.
    auto* const box = new QMessageBox(
        QMessageBox::Warning, tr("Skip Preparing Shaders?"),
        tr("The game starts right away, but it can stutter while the shaders that are not prepared "
           "yet are prepared during play.\n\nA later launch continues where this one stops.\n\n"
           "Controller: A skips, B goes back."),
        QMessageBox::Cancel, window());
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* const skip = box->addButton(tr("Skip"), QMessageBox::AcceptRole);
    skip->setDefault(true);
    connect(box, &QMessageBox::buttonClicked, this, [this, skip](QAbstractButton* button) {
        if (button == skip) {
            VideoCommon::ShaderWarm::RequestSkipWarm();
            skip_button->setEnabled(false);
        }
    });
    skip_confirm = box;
    skip_confirm_primary = skip;
    box->open();
}

void LoadingScreen::OnControllerKey(Qt::Key key) {
    if (!isVisible()) {
        return;
    }
    const bool a = key == Qt::Key_Return || key == Qt::Key_Enter;
    const bool b = key == Qt::Key_Escape;
    if (skip_confirm) {
        if (a && skip_confirm_primary) {
            skip_confirm_primary->click();
        } else if (b) {
            skip_confirm->close();
        }
        return;
    }
    if (a && skip_button->isVisible() && skip_button->isEnabled()) {
        ConfirmSkip();
    }
}

void LoadingScreen::ApplyScale() {
    // Laid out for a 1280 pixel wide window and scaled from there.
    const double scale = std::clamp(width() / 1280.0, 0.55, 2.5);
    const auto px = [scale](double value) { return std::max(1, qRound(value * scale)); };
    const QString mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();

    const int bar_height = std::max(4, qRound(width() * 0.005));
    ui->fadeLayout->setContentsMargins(px(58), px(38), px(58), px(59) + bar_height);
    ui->info->setSpacing(px(8));
    ui->status->setSpacing(px(6));
    ui->bottom_row->setSpacing(px(32));
    ui->title->setMaximumWidth(std::max(200, qRound(width() * 0.6)));
    ui->meta->setMaximumWidth(std::max(200, qRound(width() * 0.6)));

    QFont eyebrow_font = ui->eyebrow->font();
    eyebrow_font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0 * scale);
    ui->eyebrow->setFont(eyebrow_font);

    setStyleSheet(QStringLiteral(R"(
LoadingScreen { background-color: black; }
LoadingScreen QWidget#fade_parent { background: transparent; }
LoadingScreen QLabel { background: transparent; color: #ffffff; }
LoadingScreen QLabel#eyebrow { color: %1; font-family: "%2"; font-size: %3px; font-weight: 600; }
LoadingScreen QLabel#title { font-size: %4px; font-weight: 700; }
LoadingScreen QLabel#meta { color: rgba(255, 255, 255, 184); font-size: %5px; }
LoadingScreen QLabel#stage { font-size: %6px; font-weight: 600; }
LoadingScreen QLabel#value { color: rgba(255, 255, 255, 166); font-family: "%2"; font-size: %7px; }
LoadingScreen QPushButton {
  background-color: rgba(255, 255, 255, 20);
  color: #ffffff;
  border: 1px solid rgba(255, 255, 255, 115);
  border-radius: %8px;
  padding: %9px %10px;
  font-size: %11px;
}
LoadingScreen QPushButton:hover { background-color: rgba(255, 255, 255, 41); }
LoadingScreen QPushButton:focus { border-color: rgba(255, 255, 255, 200); }
LoadingScreen QPushButton:disabled { color: rgba(255, 255, 255, 110); border-color: rgba(255, 255, 255, 60); }
)")
                      .arg(Accent.name(), mono)
                      .arg(px(13))
                      .arg(px(69))
                      .arg(px(18))
                      .arg(px(20))
                      .arg(px(15))
                      .arg(px(17))
                      .arg(px(7))
                      .arg(px(22))
                      .arg(px(15)));
}

void LoadingScreen::RebuildBackdrop() {
    const QSize window_size = ui->fade_parent->size();
    if (art.isNull() || window_size.isEmpty()) {
        backdrop = QPixmap{};
        return;
    }
    const qreal ratio = devicePixelRatioF();
    const QSize target = (QSizeF(window_size) * (1.0 + PanMargin) * ratio).toSize();

    // Fill the area, cropping what is left over around the center.
    const QImage filled =
        art.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QImage cropped =
        filled.copy((filled.width() - target.width()) / 2, (filled.height() - target.height()) / 2,
                    target.width(), target.height());
    // How far the art is stretched to fill the window. A downloaded banner is close to the
    // screen's size and stays sharp. Art stretched a lot, like a game's 256 pixel icon, would show
    // its pixels: it is blurred strongly instead, into a soft backdrop.
    const double stretch = std::max(static_cast<double>(target.width()) / art.width(),
                                    static_cast<double>(target.height()) / art.height());
    int darken = 51;
    QImage soft;
    if (stretch <= 2.2) {
        soft = cropped;
    } else if (stretch <= 4.0) {
        soft = Soften(cropped, 2.0);
    } else {
        soft = Soften(cropped, 16.0);
        darken = 90;
    }
    soft = soft.convertToFormat(QImage::Format_RGB32);
    {
        QPainter painter(&soft);
        painter.fillRect(soft.rect(), QColor(0, 0, 0, darken));
    }
    backdrop = QPixmap::fromImage(soft);
    backdrop.setDevicePixelRatio(ratio);
}

void LoadingScreen::PaintBackdrop(QWidget* target) {
    QPainter painter(target);
    const QRect area = target->rect();
    painter.fillRect(area, Qt::black);
    const double seconds = clock.isValid() ? clock.elapsed() / 1000.0 : 0.0;

    if (!backdrop.isNull()) {
        // Pans diagonally there and back, easing at both ends.
        const double phase =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * seconds / PanPeriodSeconds);
        const QSizeF backdrop_size = QSizeF(backdrop.size()) / backdrop.devicePixelRatio();
        const double extra_x = std::max(0.0, backdrop_size.width() - area.width());
        const double extra_y = std::max(0.0, backdrop_size.height() - area.height());
        painter.drawPixmap(QPointF(-extra_x * phase, -extra_y * (1.0 - phase)), backdrop);
    } else {
        // No art: a dark blue gradient instead of a flat black screen.
        QLinearGradient plain(area.topLeft(), area.bottomRight());
        plain.setColorAt(0.0, QColor(0x14, 0x1a, 0x33));
        plain.setColorAt(1.0, QColor(0x05, 0x06, 0x0a));
        painter.fillRect(area, plain);
    }

    // Shades under the text at the bottom and the Skip button at the top.
    QLinearGradient bottom(0, area.height(), 0, 0);
    bottom.setColorAt(0.0, QColor(0, 0, 0, 237));
    bottom.setColorAt(0.34, QColor(0, 0, 0, 140));
    bottom.setColorAt(0.66, QColor(0, 0, 0, 0));
    painter.fillRect(area, bottom);
    const int top_height = qRound(area.height() * 0.22);
    QLinearGradient top(0, 0, 0, top_height);
    top.setColorAt(0.0, QColor(0, 0, 0, 115));
    top.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(QRect(0, 0, area.width(), top_height), top);

    // The progress line along the bottom edge.
    const int bar_height = std::max(4, qRound(area.width() * 0.005));
    const QRect track(0, area.height() - bar_height, area.width(), bar_height);
    painter.fillRect(track, QColor(255, 255, 255, 31));
    if (IsCounting(previous_stage) && previous_total > 0) {
        const double done =
            std::min(1.0, static_cast<double>(current_value) / static_cast<double>(previous_total));
        painter.fillRect(QRectF(0, track.top(), track.width() * done, bar_height), Accent);
    } else {
        // Nothing to count yet (loading, launching): a segment sweeps across.
        const double segment = track.width() * 0.28;
        const double t = std::fmod(seconds / SweepSeconds, 1.0);
        const double eased = t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
        const double left = -segment + (track.width() + segment) * eased;
        painter.fillRect(QRectF(left, track.top(), segment, bar_height), Accent);
    }
}

bool LoadingScreen::eventFilter(QObject* watched, QEvent* event) {
    if (watched == ui->fade_parent && event->type() == QEvent::Paint) {
        PaintBackdrop(ui->fade_parent);
    }
    return QWidget::eventFilter(watched, event);
}

void LoadingScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    ApplyScale();
    // The inner widget is laid out to this size once the layout runs; build for that size.
    QTimer::singleShot(0, this, [this] {
        RebuildBackdrop();
        ui->fade_parent->update();
    });
}

void LoadingScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!clock.isValid()) {
        clock.start();
    }
    frame_timer.start();
}

void LoadingScreen::hideEvent(QHideEvent* event) {
    frame_timer.stop();
    QWidget::hideEvent(event);
}

void LoadingScreen::paintEvent(QPaintEvent* event) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    QWidget::paintEvent(event);
}

void LoadingScreen::Clear() {
    art = QImage{};
    fallback_icon.clear();
    backdrop = QPixmap{};
}
