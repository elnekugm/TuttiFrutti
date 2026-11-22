// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <QKeyEvent>
#include <QObject>

#include "common/input.h"
#include "common/settings_input.h"

namespace Core::HID {
using ButtonValues = std::array<Common::Input::ButtonStatus, Settings::NativeButton::NumButtons>;
using SticksValues = std::array<Common::Input::StickStatus, Settings::NativeAnalog::NumAnalogs>;
enum class ControllerTriggerType;
class EmulatedController;
class HIDCore;
} // namespace Core::HID

class ControllerNavigation : public QObject {
    Q_OBJECT

public:
    explicit ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent = nullptr);
    ~ControllerNavigation();

    /// Disables events from the emulated controller
    void UnloadController();

signals:
    void TriggerKeyboardEvent(Qt::Key key);
    /// A button or direction that sent TriggerKeyboardEvent was let go. Qt::Key_unknown: the
    /// stick went back to the centre (every direction is released).
    void TriggerKeyboardRelease(Qt::Key key);
    /// A stick pushed in a direction, as an arrow key (the d-pad sends TriggerKeyboardEvent).
    void TriggerStickDirection(Qt::Key key);
    /// Both sticks' positions, -1..1, y up; sent when one changes.
    void SticksMoved(float left_x, float left_y, float right_x, float right_y);

private:
    void TriggerButton(Settings::NativeButton::Values native_button, Qt::Key key);
    void ControllerUpdateEvent(Core::HID::ControllerTriggerType type);

    void ControllerUpdateButton();

    void ControllerUpdateStick();

    Core::HID::ButtonValues button_values{};
    Core::HID::SticksValues stick_values{};
    std::array<float, 4> stick_positions{};

    int player1_callback_key{};
    int handheld_callback_key{};
    bool is_controller_set{};
    mutable std::mutex mutex;
    Core::HID::EmulatedController* player1_controller;
    Core::HID::EmulatedController* handheld_controller;
};
