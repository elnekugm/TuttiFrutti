// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>

#include "common/common_types.h"

namespace Loader {
class AppLoader;
}

namespace Ui {
class LoadingScreen;
}

namespace VideoCore {
enum class LoadCallbackStage;
}

class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QPushButton;
class QMessageBox;
class QAbstractButton;
class ControllerNavigation;

/// The screen shown while a game loads: the game's art fills the window and pans slowly, with its
/// title at the bottom left, the loading stage at the bottom right and a progress line along the
/// bottom edge.
class LoadingScreen : public QWidget {
    Q_OBJECT

public:
    explicit LoadingScreen(QWidget* parent = nullptr);

    ~LoadingScreen();

    /// The details shown for the game that is about to load. Call before Prepare; the title, the
    /// developer and the version come from the updated game's control data.
    void SetGameDetails(u64 program_id, const QString& title, const QString& developer,
                        const QString& version);

    /// Call before showing the loading screen to load the art of the application being loaded:
    /// its downloaded banner, else its custom artwork, else its downloaded cover, else its icon.
    void Prepare(Loader::AppLoader& loader);

    /// Shows the program's art again, after its banner or cover was downloaded while it loads.
    void ReloadArt(u64 program_id);

    /// After the loading screen is hidden, the owner of this class can call this to clean up any
    /// used resources such as the game's art.
    void Clear();

    /// Slot used to update the status of the progress bar
    void OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);

    /// Hides the LoadingScreen with a fade out effect
    void OnLoadComplete();

    /// Asks before skipping the shader preparation the loading screen is showing
    void ConfirmSkip();

    /// A: ask to skip, or confirm an open skip dialog; B: close it
    void OnControllerKey(Qt::Key key);

    // In order to use a custom widget with a stylesheet, you need to override the paintEvent
    // See https://wiki.qt.io/How_to_Change_the_Background_Color_of_QWidget
    void paintEvent(QPaintEvent* event) override;

signals:
    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);
    /// Signals that this widget is completely hidden now and should be replaced with the other
    /// widget
    void Hidden();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// Scales fonts and margins with the window, so the layout looks the same at any size.
    void ApplyScale();
    /// Renders the art once for the current size: cropped to fill, softened and darkened, with a
    /// margin for the pan.
    void RebuildBackdrop();
    /// Picks the art for art_program_id: banner, custom artwork, cover, else the game's icon.
    void LoadArt();
    void PaintBackdrop(QWidget* target);

    std::unique_ptr<Ui::LoadingScreen> ui;
    std::size_t previous_total = 0;
    std::size_t current_value = 0;
    VideoCore::LoadCallbackStage previous_stage;

    QGraphicsOpacityEffect* opacity_effect = nullptr;
    QPushButton* skip_button = nullptr;
    QPointer<QMessageBox> skip_confirm;
    QPointer<QAbstractButton> skip_confirm_primary;
    ControllerNavigation* controller_navigation = nullptr;
    std::unique_ptr<QPropertyAnimation> fadeout_animation;

    u64 details_program_id = 0;
    QString details_title;
    QString details_developer;
    QString details_version;

    u64 art_program_id = 0;
    std::vector<u8> fallback_icon;
    QImage art;
    QPixmap backdrop;
    /// Drives the pan and the moving segment of the progress line while nothing is counted.
    QTimer frame_timer;
    QElapsedTimer clock;

    // newly generated shaders are added to the end of the file, so when loading and compiling
    // shaders, it will start quickly but end slow if new shaders were added since previous launch.
    // These variables are used to detect the change in speed so we can generate an ETA
    bool slow_shader_compile_start = false;
    std::chrono::steady_clock::time_point slow_shader_start;
    std::chrono::steady_clock::time_point previous_time;
    std::size_t slow_shader_first_value = 0;
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
Q_DECLARE_METATYPE(VideoCore::LoadCallbackStage);
#endif
