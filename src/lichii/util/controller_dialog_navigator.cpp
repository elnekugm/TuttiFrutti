// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

#include "lichii/util/controller_dialog_navigator.h"

namespace {

constexpr float DeadZone = 0.2f;
/// Pixels a second at full tilt.
constexpr double CursorSpeed = 1100.0;
constexpr double ScrollSpeed = 1500.0;
/// The cursor hides this long after the left stick was last used.
constexpr qint64 CursorTimeoutMs = 2500;

/// The floating cursor: a white dot with an accent ring and a soft shadow.
class CursorWidget : public QWidget {
public:
    explicit CursorWidget(QWidget* parent) : QWidget{parent} {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFixedSize(Size, Size);
    }
    static constexpr int Size = 34;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF c(Size / 2.0, Size / 2.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawEllipse(c + QPointF(0, 1.5), 12.0, 12.0);
        painter.setBrush(QColor(0x41, 0x69, 0xe1));
        painter.drawEllipse(c, 11.0, 11.0);
        painter.setBrush(QColor(255, 255, 255, 240));
        painter.drawEllipse(c, 7.5, 7.5);
    }
};

/// The ring around the option the d-pad is on.
class FocusRing : public QWidget {
public:
    explicit FocusRing(QWidget* parent) : QWidget{parent} {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);
        QColor glow(0x41, 0x69, 0xe1, 70);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(glow, 5));
        painter.drawRoundedRect(r, 7, 7);
        painter.setPen(QPen(QColor(0x6f, 0x93, 0xff), 2.5));
        painter.drawRoundedRect(r, 7, 7);
    }
};

void PostKey(QWidget* target, int key) {
    if (target != nullptr) {
        QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier));
    }
}

QAbstractScrollArea* ScrollableAncestor(QWidget* widget) {
    for (; widget != nullptr; widget = widget->parentWidget()) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(widget);
            area != nullptr && area->verticalScrollBar()->maximum() > 0) {
            return area;
        }
    }
    return nullptr;
}

} // namespace

ControllerDialogNavigator::ControllerDialogNavigator(QObject* parent) : QObject{parent} {
    timer = new QTimer(this);
    timer->setInterval(16);
    connect(timer, &QTimer::timeout, this, &ControllerDialogNavigator::Tick);
    connect(qApp, &QApplication::focusChanged, this, [this] {
        QTimer::singleShot(0, this, &ControllerDialogNavigator::UpdateFocusRing);
    });
}

ControllerDialogNavigator::~ControllerDialogNavigator() = default;

void ControllerDialogNavigator::Suspend() {
    left_x = left_y = right_x = right_y = 0.0f;
    timer->stop();
    HideCursor();
    keyboard_mode = false;
    UpdateFocusRing();
}

void ControllerDialogNavigator::SetSticks(float lx, float ly, float rx, float ry) {
    left_x = lx;
    left_y = ly;
    right_x = rx;
    right_y = ry;
    const bool moved = std::hypot(lx, ly) > DeadZone || std::abs(ry) > DeadZone;
    if (moved && !timer->isActive()) {
        frame.restart();
        timer->start();
    }
}

void ControllerDialogNavigator::Tick() {
    QWidget* window = QApplication::activeModalWidget();
    const double dt = std::clamp(frame.restart() / 1000.0, 0.0, 0.05);
    const bool own_handling = window != nullptr && window->property("controllerNavigation").toBool();
    const bool popup = QApplication::activePopupWidget() != nullptr;

    if (window != nullptr && std::abs(right_y) > DeadZone && !popup) {
        Scroll(window, static_cast<float>(right_y * dt));
    }

    const double magnitude = std::hypot(left_x, left_y);
    if (window != nullptr && !own_handling && !popup && magnitude > DeadZone) {
        if (!cursor_shown || cursor_window != window) {
            ShowCursorIn(window);
        }
        // Slow near the centre for precision, fast at full tilt.
        const double speed =
            CursorSpeed * std::pow((magnitude - DeadZone) / (1.0 - DeadZone), 1.7);
        cursor_pos += QPointF(left_x / magnitude, -left_y / magnitude) * speed * dt;
        cursor_pos.setX(std::clamp(cursor_pos.x(), 0.0, static_cast<double>(window->width() - 1)));
        cursor_pos.setY(
            std::clamp(cursor_pos.y(), 0.0, static_cast<double>(window->height() - 1)));
        if (cursor) {
            cursor->move(cursor_pos.toPoint() - QPoint(CursorWidget::Size / 2, CursorWidget::Size / 2));
            cursor->raise();
        }
        cursor_idle.restart();
    }

    if (cursor_shown && (window != cursor_window || cursor_idle.elapsed() > CursorTimeoutMs)) {
        HideCursor();
    }
    if (keyboard_mode) {
        UpdateFocusRing();
    }
    const bool sticks_idle = magnitude <= DeadZone && std::abs(right_y) <= DeadZone;
    if (sticks_idle && !cursor_shown) {
        timer->stop();
    }
}

void ControllerDialogNavigator::ShowCursorIn(QWidget* window) {
    HideCursor();
    cursor_window = window;
    auto* widget = new CursorWidget(window);
    cursor = widget;
    // From the focused option, or the middle of the window.
    QWidget* focus = window->focusWidget();
    if (focus != nullptr && window->isAncestorOf(focus) && focus->isVisible()) {
        cursor_pos = QPointF(focus->mapTo(window, focus->rect().center()));
    } else {
        cursor_pos = QPointF(window->width() / 2.0, window->height() / 2.0);
    }
    widget->move(cursor_pos.toPoint() - QPoint(CursorWidget::Size / 2, CursorWidget::Size / 2));
    widget->show();
    widget->raise();
    cursor_shown = true;
    cursor_idle.restart();
    keyboard_mode = false;
    UpdateFocusRing();
}

void ControllerDialogNavigator::HideCursor() {
    if (cursor) {
        cursor->deleteLater();
    }
    cursor = nullptr;
    cursor_shown = false;
}

void ControllerDialogNavigator::ClickAtCursor() {
    QWidget* window = cursor_window;
    if (window == nullptr) {
        return;
    }
    const QPoint pos = cursor_pos.toPoint();
    QWidget* target = window->childAt(pos);
    if (target == nullptr) {
        target = window;
    }
    const QPointF local = target->mapFrom(window, QPointF(pos));
    const QPointF global = window->mapToGlobal(QPointF(pos));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    // The press can close the window (a Cancel button) or open a pop-up.
    if (QPointer<QWidget> alive = target) {
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(alive, &release);
    }
    cursor_idle.restart();
}

void ControllerDialogNavigator::MoveFocus(QWidget* window, bool forward) {
    // The options in reading order: top to bottom, then left to right. Qt's tab order follows
    // the order widgets were made in, which the settings pages do not keep.
    std::vector<QWidget*> options;
    for (QWidget* widget : window->findChildren<QWidget*>()) {
        if (!widget->isVisible() || !widget->isEnabled() ||
            !(widget->focusPolicy() & Qt::TabFocus) || widget->focusProxy() != nullptr ||
            qobject_cast<QScrollArea*>(widget) != nullptr || widget == cursor ||
            widget == focus_ring) {
            continue;
        }
        options.push_back(widget);
    }
    if (options.empty()) {
        return;
    }
    const auto top_left = [window](const QWidget* widget) { return widget->mapTo(window, QPoint(0, 0)); };
    std::stable_sort(options.begin(), options.end(), [&](const QWidget* a, const QWidget* b) {
        const QPoint pa = top_left(a);
        const QPoint pb = top_left(b);
        // On the same line within a few pixels: left first.
        if (std::abs(pa.y() - pb.y()) > 6) {
            return pa.y() < pb.y();
        }
        return pa.x() < pb.x();
    });

    QWidget* current = window->focusWidget();
    const auto it = std::find(options.begin(), options.end(), current);
    int index;
    if (it == options.end()) {
        index = forward ? 0 : static_cast<int>(options.size()) - 1;
    } else {
        index = static_cast<int>(it - options.begin()) + (forward ? 1 : -1);
        index = std::clamp(index, 0, static_cast<int>(options.size()) - 1);
    }
    QWidget* next = options[static_cast<size_t>(index)];
    next->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    for (QWidget* parent = next->parentWidget(); parent != nullptr; parent = parent->parentWidget()) {
        if (auto* area = qobject_cast<QScrollArea*>(parent)) {
            area->ensureWidgetVisible(next, 40, 60);
            break;
        }
    }
}

void ControllerDialogNavigator::SwitchTab(QWidget* window, int direction) {
    QTabWidget* tabs = nullptr;
    for (QWidget* widget = window->focusWidget(); widget != nullptr && widget != window;
         widget = widget->parentWidget()) {
        if ((tabs = qobject_cast<QTabWidget*>(widget)) != nullptr && tabs->isVisible()) {
            break;
        }
        tabs = nullptr;
    }
    if (tabs == nullptr) {
        // The largest tab set showing.
        int best_area = 0;
        for (QTabWidget* candidate : window->findChildren<QTabWidget*>()) {
            const int area = candidate->width() * candidate->height();
            if (candidate->isVisible() && candidate->count() > 1 && area > best_area) {
                best_area = area;
                tabs = candidate;
            }
        }
    }
    if (tabs == nullptr || tabs->count() < 2) {
        return;
    }
    int index = tabs->currentIndex();
    for (int step = 0; step < tabs->count(); ++step) {
        index = (index + direction + tabs->count()) % tabs->count();
        if (tabs->isTabEnabled(index) && tabs->isTabVisible(index)) {
            break;
        }
    }
    tabs->setCurrentIndex(index);
    tabs->tabBar()->setFocus(Qt::TabFocusReason);
}

void ControllerDialogNavigator::PressOk(QWidget* window) {
    // A folder picker chooses the folder that is open, not one highlighted in it.
    if (qobject_cast<QFileDialog*>(window) != nullptr) {
        for (QAbstractItemView* view : window->findChildren<QAbstractItemView*>()) {
            if (qobject_cast<QFileSystemModel*>(view->model()) != nullptr ||
                view->objectName() == QStringLiteral("listView") ||
                view->objectName() == QStringLiteral("treeView")) {
                view->clearSelection();
            }
        }
    }
    for (QDialogButtonBox* box : window->findChildren<QDialogButtonBox*>()) {
        if (!box->isVisible()) {
            continue;
        }
        QPushButton* ok = box->button(QDialogButtonBox::Ok);
        if (ok == nullptr) {
            for (QAbstractButton* button : box->buttons()) {
                if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
                    ok = qobject_cast<QPushButton*>(button);
                    break;
                }
            }
        }
        if (ok != nullptr && ok->isEnabled()) {
            ok->animateClick();
            return;
        }
    }
}

void ControllerDialogNavigator::Scroll(QWidget* window, float amount) {
    QAbstractScrollArea* area = nullptr;
    if (cursor_shown && cursor_window == window) {
        area = ScrollableAncestor(window->childAt(cursor_pos.toPoint()));
    }
    if (area == nullptr) {
        area = ScrollableAncestor(window->focusWidget());
    }
    if (area == nullptr) {
        int best_area = 0;
        for (QAbstractScrollArea* candidate : window->findChildren<QAbstractScrollArea*>()) {
            const int size = candidate->width() * candidate->height();
            if (candidate->isVisible() && candidate->verticalScrollBar()->maximum() > 0 &&
                size > best_area) {
                best_area = size;
                area = candidate;
            }
        }
    }
    if (area == nullptr) {
        return;
    }
    // Pushed up scrolls up; the amount grows with the tilt.
    const float tilt = std::abs(right_y);
    const double pixels =
        ScrollSpeed * std::pow((tilt - DeadZone) / (1.0 - DeadZone), 1.5) *
        (std::abs(amount) / std::max(tilt, 0.001f));
    scroll_remainder += static_cast<float>(right_y > 0 ? -pixels : pixels);
    const int step = static_cast<int>(scroll_remainder);
    scroll_remainder -= static_cast<float>(step);
    QScrollBar* bar = area->verticalScrollBar();
    bar->setValue(bar->value() + step);
    if (keyboard_mode) {
        UpdateFocusRing();
    }
}

void ControllerDialogNavigator::UpdateFocusRing() {
    QWidget* window = QApplication::activeModalWidget();
    QWidget* focus = window != nullptr ? window->focusWidget() : nullptr;
    const bool show = keyboard_mode && window != nullptr &&
                      !window->property("controllerNavigation").toBool() && focus != nullptr &&
                      focus != window && window->isAncestorOf(focus) && focus->isVisible();
    if (!show) {
        if (focus_ring) {
            focus_ring->hide();
        }
        return;
    }
    if (!focus_ring || focus_ring->parentWidget() != window) {
        if (focus_ring) {
            focus_ring->deleteLater();
        }
        focus_ring = new FocusRing(window);
    }
    const QRect rect(focus->mapTo(window, QPoint(0, 0)), focus->size());
    focus_ring->setGeometry(rect.adjusted(-5, -5, 5, 5));
    focus_ring->show();
    focus_ring->raise();
    if (cursor) {
        cursor->raise();
    }
}

bool ControllerDialogNavigator::WaitingForInput(QWidget* window) const {
    // A button being mapped in the input settings takes the next button press for itself.
    const QStringList waiting{
        QStringLiteral("[waiting]"),
        QCoreApplication::translate("ConfigureInputPlayer", "[waiting]"),
        QCoreApplication::translate("ConfigureRingController", "[waiting]"),
    };
    for (const QPushButton* button : window->findChildren<QPushButton*>()) {
        if (button->isVisible() && waiting.contains(button->text())) {
            return true;
        }
    }
    return false;
}

void ControllerDialogNavigator::HandleKey(QWidget* window, int key) {
    if (window == nullptr || WaitingForInput(window)) {
        return;
    }
    QWidget* focus = window->focusWidget();
    if (focus != nullptr && focus != window && !window->isAncestorOf(focus)) {
        focus = nullptr;
    }

    // A folder picker: A opens the highlighted folder, B goes up one.
    if (auto* files = qobject_cast<QFileDialog*>(window); files != nullptr && !cursor_shown) {
        auto* view = qobject_cast<QAbstractItemView*>(focus);
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && view != nullptr) {
            const QString path =
                view->currentIndex().data(QFileSystemModel::FilePathRole).toString();
            if (!path.isEmpty() && QFileInfo(path).isDir()) {
                files->setDirectory(path);
                keyboard_mode = true;
                QTimer::singleShot(0, this, &ControllerDialogNavigator::UpdateFocusRing);
                return;
            }
        }
        if (key == Qt::Key_Escape) {
            QDir dir = files->directory();
            if (dir.cdUp() && dir.absolutePath() != files->directory().absolutePath()) {
                files->setDirectory(dir);
                return;
            }
        }
    }

    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        HideCursor();
        keyboard_mode = true;
        if (auto* view = qobject_cast<QAbstractItemView*>(focus)) {
            PostKey(view, key);
        } else {
            MoveFocus(window, key == Qt::Key_Down);
        }
        break;
    case Qt::Key_Left:
    case Qt::Key_Right: {
        HideCursor();
        keyboard_mode = true;
        const bool right = key == Qt::Key_Right;
        if (qobject_cast<QTabBar*>(focus) != nullptr ||
            qobject_cast<QAbstractSlider*>(focus) != nullptr) {
            PostKey(focus, key);
        } else if (qobject_cast<QAbstractSpinBox*>(focus) != nullptr) {
            PostKey(focus, right ? Qt::Key_Up : Qt::Key_Down);
        } else if (qobject_cast<QComboBox*>(focus) != nullptr) {
            PostKey(focus, right ? Qt::Key_Down : Qt::Key_Up);
        } else {
            MoveFocus(window, right);
        }
        break;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (cursor_shown) {
            ClickAtCursor();
            return;
        }
        keyboard_mode = true;
        if (auto* button = qobject_cast<QAbstractButton*>(focus)) {
            button->animateClick();
        } else if (auto* combo = qobject_cast<QComboBox*>(focus)) {
            combo->showPopup();
        } else if (focus != nullptr) {
            PostKey(focus, Qt::Key_Return);
        }
        break;
    case Qt::Key_Escape:
        if (cursor_shown) {
            HideCursor();
            return;
        }
        PostKey(focus != nullptr ? focus : window, Qt::Key_Escape);
        break;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        HideCursor();
        keyboard_mode = true;
        SwitchTab(window, key == Qt::Key_PageDown ? 1 : -1);
        break;
    case Qt::Key_Menu:
        PressOk(window);
        break;
    default:
        PostKey(focus != nullptr ? focus : window, key);
        break;
    }
    QTimer::singleShot(0, this, &ControllerDialogNavigator::UpdateFocusRing);
}
