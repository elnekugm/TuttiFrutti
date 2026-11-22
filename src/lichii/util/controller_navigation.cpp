// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>

#include "common/settings_input.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "lichii/util/controller_navigation.h"

ControllerNavigation::ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent) {
    player1_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    handheld_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    Core::HID::ControllerUpdateCallback engine_callback{
        .on_change = [this](Core::HID::ControllerTriggerType type) { ControllerUpdateEvent(type); },
        .is_npad_service = false,
    };
    player1_callback_key = player1_controller->SetCallback(engine_callback);
    handheld_callback_key = handheld_controller->SetCallback(engine_callback);
    is_controller_set = true;
}

ControllerNavigation::~ControllerNavigation() {
    UnloadController();
}

void ControllerNavigation::UnloadController() {
    if (is_controller_set) {
        player1_controller->DeleteCallback(player1_callback_key);
        handheld_controller->DeleteCallback(handheld_callback_key);
        is_controller_set = false;
    }
}

void ControllerNavigation::TriggerButton(Settings::NativeButton::Values native_button,
                                         Qt::Key key) {
    if (button_values[native_button].locked) {
        return;
    }
    if (button_values[native_button].value) {
        emit TriggerKeyboardEvent(key);
    } else {
        emit TriggerKeyboardRelease(key);
    }
}

void ControllerNavigation::ControllerUpdateEvent(Core::HID::ControllerTriggerType type) {
    std::scoped_lock lock{mutex};
    if (!Settings::values.controller_navigation) {
        return;
    }
    if (type == Core::HID::ControllerTriggerType::Button) {
        ControllerUpdateButton();
        return;
    }

    if (type == Core::HID::ControllerTriggerType::Stick) {
        ControllerUpdateStick();
        return;
    }
}

void ControllerNavigation::ControllerUpdateButton() {
    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_buttons = player1_controller->GetButtonsValues();
    const auto& handheld_buttons = handheld_controller->GetButtonsValues();

    for (std::size_t i = 0; i < player1_buttons.size(); ++i) {
        const bool button = player1_buttons[i].value || handheld_buttons[i].value;
        // Trigger only once
        button_values[i].locked = button == button_values[i].value;
        button_values[i].value = button;
    }

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        TriggerButton(Settings::NativeButton::A, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::B, Qt::Key_Escape);
        TriggerButton(Settings::NativeButton::DDown, Qt::Key_Down);
        TriggerButton(Settings::NativeButton::DLeft, Qt::Key_Left);
        TriggerButton(Settings::NativeButton::DRight, Qt::Key_Right);
        TriggerButton(Settings::NativeButton::DUp, Qt::Key_Up);
        // X and + open a game's options, L and R jump through the home screen's games.
        TriggerButton(Settings::NativeButton::X, Qt::Key_Menu);
        TriggerButton(Settings::NativeButton::Plus, Qt::Key_Menu);
        TriggerButton(Settings::NativeButton::L, Qt::Key_PageUp);
        TriggerButton(Settings::NativeButton::R, Qt::Key_PageDown);
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        TriggerButton(Settings::NativeButton::DDown, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::DLeft, Qt::Key_Escape);
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        TriggerButton(Settings::NativeButton::X, Qt::Key_Enter);
        TriggerButton(Settings::NativeButton::A, Qt::Key_Escape);
        break;
    default:
        break;
    }
}

void ControllerNavigation::ControllerUpdateStick() {
    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_sticks = player1_controller->GetSticksValues();
    const auto& handheld_sticks = handheld_controller->GetSticksValues();
    bool update = false;

    // The positions, for a pointer or scrolling: whichever of player 1 and handheld is moved.
    const auto pick = [&](std::size_t stick, bool y) {
        const float p1 = y ? player1_sticks[stick].y.value : player1_sticks[stick].x.value;
        const float hh = y ? handheld_sticks[stick].y.value : handheld_sticks[stick].x.value;
        return std::abs(p1) >= std::abs(hh) ? p1 : hh;
    };
    const std::array<float, 4> positions{
        pick(Settings::NativeAnalog::LStick, false), pick(Settings::NativeAnalog::LStick, true),
        pick(Settings::NativeAnalog::RStick, false), pick(Settings::NativeAnalog::RStick, true)};
    bool moved = false;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        moved |= std::abs(positions[i] - stick_positions[i]) > 0.01f ||
                 (positions[i] == 0.0f) != (stick_positions[i] == 0.0f);
    }
    if (moved) {
        stick_positions = positions;
        emit SticksMoved(positions[0], positions[1], positions[2], positions[3]);
    }

    for (std::size_t i = 0; i < player1_sticks.size(); ++i) {
        const Common::Input::StickStatus stick{
            .left = player1_sticks[i].left || handheld_sticks[i].left,
            .right = player1_sticks[i].right || handheld_sticks[i].right,
            .up = player1_sticks[i].up || handheld_sticks[i].up,
            .down = player1_sticks[i].down || handheld_sticks[i].down,
        };
        // Trigger only once
        if (stick.down != stick_values[i].down || stick.left != stick_values[i].left ||
            stick.right != stick_values[i].right || stick.up != stick_values[i].up) {
            update = true;
        }
        stick_values[i] = stick;
    }

    if (!update) {
        return;
    }

    const auto& lstick = stick_values[Settings::NativeAnalog::LStick];
    const auto& rstick = stick_values[Settings::NativeAnalog::RStick];
    if (!lstick.left && !lstick.right && !lstick.up && !lstick.down && !rstick.left &&
        !rstick.right && !rstick.up && !rstick.down) {
        emit TriggerKeyboardRelease(Qt::Key_unknown);
        return;
    }

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube:
        if (stick_values[Settings::NativeAnalog::LStick].down) {
            emit TriggerStickDirection(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].left) {
            emit TriggerStickDirection(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].right) {
            emit TriggerStickDirection(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].up) {
            emit TriggerStickDirection(Qt::Key_Up);
            return;
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        if (stick_values[Settings::NativeAnalog::LStick].left) {
            emit TriggerStickDirection(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].up) {
            emit TriggerStickDirection(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].down) {
            emit TriggerStickDirection(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::LStick].right) {
            emit TriggerStickDirection(Qt::Key_Up);
            return;
        }
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        if (stick_values[Settings::NativeAnalog::RStick].right) {
            emit TriggerStickDirection(Qt::Key_Down);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].down) {
            emit TriggerStickDirection(Qt::Key_Left);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].up) {
            emit TriggerStickDirection(Qt::Key_Right);
            return;
        }
        if (stick_values[Settings::NativeAnalog::RStick].left) {
            emit TriggerStickDirection(Qt::Key_Up);
            return;
        }
        break;
    default:
        break;
    }
}
