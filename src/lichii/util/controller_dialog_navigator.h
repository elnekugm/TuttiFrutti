// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>

class QTimer;
class QWidget;

/**
 * Drives a settings window (Configure, a game's Properties, and other modal windows) with a
 * controller, from the keys and stick positions ControllerNavigation reports:
 *
 * - D-pad up/down moves between options (a ring marks the focused one); in a list it moves the
 *   selection. Left/right changes a slider, number, drop-down or tab, else moves between options.
 * - A turns the focused option on or off, presses it, or opens its drop-down.
 * - B closes an open drop-down or, otherwise, the window (Cancel).
 * - L/R switch tabs; + or X presses OK.
 * - The left stick shows a floating cursor (only while it is used) that A clicks with.
 * - The right stick scrolls what is under the cursor, or the page.
 *
 * Windows that handle the controller themselves (property "controllerNavigation") only get the
 * right-stick scrolling.
 */
class ControllerDialogNavigator : public QObject {
    Q_OBJECT

public:
    explicit ControllerDialogNavigator(QObject* parent = nullptr);
    ~ControllerDialogNavigator() override;

    /// A key from the controller's buttons or d-pad for `window`, the active modal window.
    void HandleKey(QWidget* window, int key);
    /// Both sticks, -1..1 with y up.
    void SetSticks(float left_x, float left_y, float right_x, float right_y);
    /// Hides the cursor and stops following the sticks (a game started, say).
    void Suspend();

private:
    void Tick();
    void ShowCursorIn(QWidget* window);
    void HideCursor();
    void ClickAtCursor();
    void MoveFocus(QWidget* window, bool forward);
    void SwitchTab(QWidget* window, int direction);
    void PressOk(QWidget* window);
    void Scroll(QWidget* window, float amount);
    void UpdateFocusRing();
    [[nodiscard]] bool WaitingForInput(QWidget* window) const;

    QTimer* timer = nullptr;
    QPointer<QWidget> cursor_window;
    QPointer<QWidget> cursor;
    QPointer<QWidget> focus_ring;
    QPointF cursor_pos;
    QElapsedTimer cursor_idle;
    QElapsedTimer frame;
    bool cursor_shown = false;
    /// The d-pad was used: the focus ring shows until the mouse or the cursor takes over.
    bool keyboard_mode = false;

    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    /// Scrolling below a whole pixel, kept for the next frame.
    float scroll_remainder = 0.0f;
};
