// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QAction>
#include <QEasingCurve>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "frontend_common/play_time_manager.h"
#include "lichii/game/common.h"
#include "lichii/game/game_home.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/util/game.h"

namespace {

const QColor Accent{0x41, 0x69, 0xe1};
const QColor AccentLight{0x9d, 0xb8, 0xff};

QColor Mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t),
                            static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t));
}

bool IsGame(const QModelIndex& index) {
    return index.data(GameListItem::TypeRole).toInt() == static_cast<int>(GameListItemType::Game);
}

/// The status bar shows its values in capitals ("NO AA", "FSR 1 SOFT"); the chips read better in
/// title case ("No AA", "FSR 1 Soft"), keeping acronyms as they are.
QString ChipCase(const QString& text) {
    if (text != text.toUpper()) {
        return text;
    }
    static const QStringList acronyms{
        QStringLiteral("AA"),   QStringLiteral("FX"),  QStringLiteral("FG"),
        QStringLiteral("FSR"),  QStringLiteral("SMAA"), QStringLiteral("FXAA"),
        QStringLiteral("TAA"),  QStringLiteral("MSAA"), QStringLiteral("DLSS"),
        QStringLiteral("FPS"),  QStringLiteral("GPU"),  QStringLiteral("CPU"),
        QStringLiteral("HDR"),  QStringLiteral("SGSR"), QStringLiteral("CAS"),
        QStringLiteral("XESS"), QStringLiteral("FSR1"), QStringLiteral("FSR2"),
        QStringLiteral("FSR3"), QStringLiteral("FSR4"), QStringLiteral("CRT"),
    };
    QStringList words = text.split(QLatin1Char{' '});
    for (QString& word : words) {
        if (word.isEmpty() || acronyms.contains(word) || word.front().isDigit()) {
            continue;
        }
        if (word == QStringLiteral("OPENGL")) {
            word = QStringLiteral("OpenGL");
            continue;
        }
        word = word.front() + word.mid(1).toLower();
    }
    return words.join(QLatin1Char{' '});
}

void Repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

// The home screen keeps its own dark look whatever the theme, like the console's.
const QString HomeStyle = QStringLiteral(R"(
GameHome { background-color: #101114; border: 0; }
QWidget#HomePillBar, QWidget#HomeHeader, QWidget#HomeChipBar, QWidget#HomeHintBar {
    background: transparent;
}
QPushButton#HomePill {
    background-color: #1a1b1f; color: #e6e7ea; border: 2px solid #2a2c31;
    border-radius: 10px; font-size: 15px; padding: 0 22px; min-height: 42px; max-height: 42px;
}
QPushButton#HomePill:hover { background-color: #23252a; }
QPushButton#HomePill[homeFocus="true"] {
    border: 2px solid #4169e1; background-color: #1d2640; color: #ffffff;
}
QLabel#HomeStatus { color: #7f9fff; font-size: 14px; font-weight: 600; background: transparent; }
QLabel#HomeTitle { color: #f2f3f5; font-size: 23px; background: transparent; }
QLabel#HomeChipCaption { color: #8b8d94; font-size: 12px; background: transparent; }
QPushButton#HomeHint {
    background: transparent; border: 0; color: #e6e7ea; font-size: 16px; padding: 4px 6px;
}
QPushButton#HomeHint:hover { color: #ffffff; }
QLabel#HomeHintText { color: #e6e7ea; font-size: 16px; background: transparent; }
)");

/// A controller button as it is printed on the controller player 1 uses: Switch letters in a
/// ring, Xbox letters on colored buttons, PlayStation symbols, or a keyboard key. The buttons are
/// matched by position (SDL maps them that way): the right one confirms, the bottom one goes back
/// and the top one opens options.
class HomeGlyph : public QWidget {
public:
    HomeGlyph(GameHome::ButtonStyle style_, GameHome::HintRole role_, QWidget* parent)
        : QWidget{parent}, style{style_}, role{role_} {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        if (style == GameHome::ButtonStyle::Keyboard) {
            QFont key_font = font();
            key_font.setPixelSize(12);
            key_font.setBold(true);
            const int text_w = QFontMetrics(key_font).horizontalAdvance(KeyText());
            setFixedSize(std::max(26, text_w + 14), 24);
        } else {
            setFixedSize(role == GameHome::HintRole::Adjust ? 34 : 26, 26);
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor light(0xe6, 0xe7, 0xea);
        const QRectF box = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);

        if (style == GameHome::ButtonStyle::Keyboard) {
            painter.setPen(QPen(light, 1.5));
            painter.setBrush(QColor(0x22, 0x23, 0x28));
            painter.drawRoundedRect(box, 5, 5);
            QFont key_font = font();
            key_font.setPixelSize(12);
            key_font.setBold(true);
            painter.setFont(key_font);
            painter.drawText(rect(), Qt::AlignCenter, KeyText());
            return;
        }
        if (role == GameHome::HintRole::Adjust) {
            // Left and right on the d-pad or stick.
            painter.setPen(Qt::NoPen);
            painter.setBrush(light);
            const qreal cy = height() / 2.0;
            painter.drawPolygon(QPolygonF{{2, cy}, {13, cy - 7}, {13, cy + 7}});
            painter.drawPolygon(QPolygonF{{width() - 2.0, cy}, {width() - 13.0, cy - 7},
                                          {width() - 13.0, cy + 7}});
            return;
        }

        QFont letter_font = font();
        letter_font.setPixelSize(13);
        letter_font.setBold(true);
        painter.setFont(letter_font);

        switch (style) {
        case GameHome::ButtonStyle::Switch:
            painter.setPen(QPen(light, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(box);
            painter.drawText(rect(), Qt::AlignCenter, Letter());
            break;
        case GameHome::ButtonStyle::Xbox: {
            // Xbox prints A green (bottom), B red (right), Y yellow (top), X blue (left).
            QColor fill;
            switch (role) {
            case GameHome::HintRole::Confirm:
                fill = QColor(0xe0, 0x52, 0x52);
                break;
            case GameHome::HintRole::Back:
                fill = QColor(0x5c, 0xb8, 0x5c);
                break;
            default:
                fill = QColor(0xf0, 0xc3, 0x3c);
                break;
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawEllipse(box);
            painter.setPen(role == GameHome::HintRole::Options ? QColor(0x1a, 0x1a, 0x1a)
                                                               : QColor(Qt::white));
            painter.drawText(rect(), Qt::AlignCenter, Letter());
            break;
        }
        case GameHome::ButtonStyle::PlayStation: {
            painter.setPen(QPen(QColor(0x5c, 0x5e, 0x65), 1.5));
            painter.setBrush(QColor(0x1e, 0x1f, 0x23));
            painter.drawEllipse(box);
            const QPointF c = QRectF(rect()).center();
            const qreal r = width() * 0.22;
            switch (role) {
            case GameHome::HintRole::Confirm: // circle, right
                painter.setPen(QPen(QColor(0xff, 0x6b, 0x7a), 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(c, r, r);
                break;
            case GameHome::HintRole::Back: // cross, bottom
                painter.setPen(QPen(QColor(0x7a, 0xa7, 0xff), 2, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(c + QPointF(-r, -r), c + QPointF(r, r));
                painter.drawLine(c + QPointF(-r, r), c + QPointF(r, -r));
                break;
            default: // triangle, top
                painter.setPen(QPen(QColor(0x3d, 0xdc, 0x97), 2, Qt::SolidLine, Qt::RoundCap,
                                    Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(QPolygonF{c + QPointF(0, -r * 1.1),
                                              c + QPointF(r * 1.05, r * 0.75),
                                              c + QPointF(-r * 1.05, r * 0.75)});
                break;
            }
            break;
        }
        default:
            break;
        }
    }

private:
    QString Letter() const {
        // By position: Switch A/B/X, and the same places on Xbox are B/A/Y.
        const bool xbox = style == GameHome::ButtonStyle::Xbox;
        switch (role) {
        case GameHome::HintRole::Confirm:
            return xbox ? QStringLiteral("B") : QStringLiteral("A");
        case GameHome::HintRole::Back:
            return xbox ? QStringLiteral("A") : QStringLiteral("B");
        default:
            return xbox ? QStringLiteral("Y") : QStringLiteral("X");
        }
    }

    QString KeyText() const {
        switch (role) {
        case GameHome::HintRole::Confirm:
            return QStringLiteral("Enter");
        case GameHome::HintRole::Back:
            return QStringLiteral("Esc");
        case GameHome::HintRole::Options:
            return QStringLiteral("X");
        default:
            return QStringLiteral("\u2190 \u2192");
        }
    }

    GameHome::ButtonStyle style;
    GameHome::HintRole role;
};

} // namespace

/// Paints one game as a large square cover with rounded corners, and the selection ring around the
/// selected one.
class HomeCard : public QStyledItemDelegate {
public:
    explicit HomeCard(GameHome* home_) : QStyledItemDelegate{home_}, home{home_} {}

    void SetGeometry(int side_, int ring_) {
        side = side_;
        ring = ring_;
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {side + 2 * ring, side + 2 * ring};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        const QRect card = option.rect.adjusted(ring, ring, -ring, -ring);
        const qreal radius = std::max(8.0, card.width() * 0.045);
        QPainterPath path;
        path.addRoundedRect(QRectF(card), radius, radius);

        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const QPixmap cover = Cover(index, card.width(), dpr);
        if (!cover.isNull()) {
            painter->setClipPath(path);
            painter->drawPixmap(card, cover);
            painter->setClipping(false);
        } else {
            // No artwork: the name on a plain tile.
            painter->fillPath(path, QColor(0x22, 0x23, 0x28));
            painter->setPen(QColor(0xc4, 0xc5, 0xca));
            QFont font = painter->font();
            font.setPixelSize(std::max(14, card.width() / 14));
            painter->setFont(font);
            painter->drawText(card.adjusted(16, 16, -16, -16),
                              Qt::AlignCenter | Qt::TextWordWrap,
                              index.data(GameListItemPath::TitleRole).toString());
        }

        const bool selected = index == home->currentIndex();
        if (selected) {
            const QRectF ring_rect = QRectF(card).adjusted(-6, -6, 6, 6);
            if (home->CardsFocused()) {
                // A soft glow, then the ring, pulsing between the accent and a lighter blue.
                const QColor color = Mix(Accent, AccentLight, home->Pulse());
                QColor glow = color;
                glow.setAlpha(60);
                painter->setPen(QPen(glow, 10));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(ring_rect, radius + 6, radius + 6);
                painter->setPen(QPen(color, 4));
                painter->drawRoundedRect(ring_rect, radius + 6, radius + 6);
            } else {
                QColor dim = Accent;
                dim.setAlpha(110);
                painter->setPen(QPen(dim, 2));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(ring_rect, radius + 6, radius + 6);
            }
        }
        painter->restore();
    }

private:
    /// The game's cover at the card's size: the artwork the user set or downloaded, else the
    /// game's own icon scaled up. Cached by program and by the icon the model holds, so a new
    /// download (which replaces the icon) shows at once.
    static QPixmap Cover(const QModelIndex& index, int side, qreal dpr) {
        const u64 program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
        const QPixmap icon = index.data(Qt::DecorationRole).value<QPixmap>();
        const int pixels = static_cast<int>(side * dpr);
        const QString key = QStringLiteral("home:%1:%2:%3")
                                .arg(program_id, 16, 16, QLatin1Char{'0'})
                                .arg(icon.cacheKey())
                                .arg(pixels);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached)) {
            return cached;
        }

        QImage image;
        if (program_id != 0) {
            const std::vector<u8> artwork = QtCommon::Game::ReadLibraryArtwork(program_id);
            if (!artwork.empty()) {
                image.loadFromData(artwork.data(), static_cast<int>(artwork.size()));
            }
        }
        if (image.isNull() && !icon.isNull() &&
            index.data(GameListItemPath::HasIconRole).toBool()) {
            image = icon.toImage();
        }
        if (image.isNull()) {
            return {};
        }
        image = image.scaled(pixels, pixels, Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation);
        image = image.copy((image.width() - pixels) / 2, (image.height() - pixels) / 2, pixels,
                           pixels);
        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(dpr);
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }

    GameHome* home;
    int side = 280;
    int ring = 12;
};

GameHome::GameHome(QWidget* parent) : QListView{parent} {
    setObjectName(QStringLiteral("GameHome"));
    setStyleSheet(HomeStyle);

    // Covers are up to 440 px a side, 1-2 MiB each at high DPI: room for a library's worth.
    QPixmapCache::setCacheLimit(std::max(QPixmapCache::cacheLimit(), 96 * 1024));

    card_delegate = new HomeCard(this);
    setItemDelegate(card_delegate);

    setViewMode(QListView::ListMode);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    pill_bar = new QWidget(this);
    pill_bar->setObjectName(QStringLiteral("HomePillBar"));
    auto* pill_layout = new QHBoxLayout(pill_bar);
    pill_layout->setContentsMargins(0, 0, 0, 0);
    pill_layout->setSpacing(12);
    pill_layout->addStretch(1);

    header = new QWidget(this);
    header->setObjectName(QStringLiteral("HomeHeader"));
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(4);
    header_layout->addStretch(1);
    status_label = new QLabel(header);
    status_label->setObjectName(QStringLiteral("HomeStatus"));
    title_label = new QLabel(header);
    title_label->setObjectName(QStringLiteral("HomeTitle"));
    title_label->setTextFormat(Qt::RichText);
    header_layout->addWidget(status_label);
    header_layout->addWidget(title_label);

    chip_bar = new QWidget(this);
    chip_bar->setObjectName(QStringLiteral("HomeChipBar"));
    auto* chip_layout = new QGridLayout(chip_bar);
    chip_layout->setContentsMargins(0, 0, 0, 0);
    chip_layout->setHorizontalSpacing(14);
    chip_layout->setVerticalSpacing(6);

    hint_bar = new QWidget(this);
    hint_bar->setObjectName(QStringLiteral("HomeHintBar"));
    auto* hint_layout = new QHBoxLayout(hint_bar);
    hint_layout->setContentsMargins(0, 0, 0, 0);
    hint_layout->setSpacing(22);

    scroll_animation = new QVariantAnimation(this);
    scroll_animation->setDuration(220);
    scroll_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(scroll_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { horizontalScrollBar()->setValue(value.toInt()); });

    pulse_animation = new QVariantAnimation(this);
    pulse_animation->setStartValue(0.0);
    pulse_animation->setKeyValueAt(0.5, 1.0);
    pulse_animation->setEndValue(0.0);
    pulse_animation->setDuration(1800);
    pulse_animation->setLoopCount(-1);
    pulse_animation->setEasingCurve(QEasingCurve::InOutSine);
    connect(pulse_animation, &QVariantAnimation::valueChanged, this, [this] {
        const QModelIndex current = currentIndex();
        if (current.isValid() && row == Row::Cards) {
            viewport()->update(visualRect(current).adjusted(-2, -2, 2, 2));
        }
    });

    // Settings change elsewhere too (Configure, the hotkeys): keep the chips current.
    refresh_timer = new QTimer(this);
    refresh_timer->setInterval(1000);
    connect(refresh_timer, &QTimer::timeout, this, &GameHome::RefreshChips);

    UpdateHints();
}

GameHome::~GameHome() = default;

void GameHome::SetModel(GameListModel* game_model) {
    QListView::setModel(game_model);
    // New or re-sorted rows: hide the ones that are not games, keep the search applied.
    const auto settle = [this, game_model] {
        QTimer::singleShot(0, this, [this, game_model] { ApplyFilter(filter_text, game_model); });
    };
    connect(game_model, &QAbstractItemModel::rowsInserted, this, settle);
    connect(game_model, &QAbstractItemModel::modelReset, this, settle);
    connect(game_model, &QAbstractItemModel::layoutChanged, this, settle);
    connect(game_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& top_left) {
                if (top_left.row() == currentIndex().row()) {
                    UpdateHeader();
                }
            });
    UpdateIconSize();
}

void GameHome::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    filter_text = edit_filter_text;
    const int row_count = model->rowCount();
    for (int i = 0; i < row_count; ++i) {
        QStandardItem* item = model->item(i, 0);
        const bool show = item != nullptr && IsGame(item->index()) &&
                          Lichii::FilterMatches(edit_filter_text, item);
        setRowHidden(i, !show);
    }
    EnsureCurrent();
    UpdateHeader();
}

void GameHome::UpdateIconSize() {
    LayoutChrome();
}

void GameHome::SetMenuBar(QMenuBar* menu_bar_) {
    menu_bar = menu_bar_;
    if (isVisible()) {
        RebuildPills();
    }
}

void GameHome::SetChips(std::vector<HomeChip> chips_) {
    chips = std::move(chips_);
    for (QPushButton* button : chip_buttons) {
        button->deleteLater();
    }
    for (QLabel* caption : chip_captions) {
        caption->deleteLater();
    }
    chip_buttons.clear();
    chip_captions.clear();

    auto* layout = static_cast<QGridLayout*>(chip_bar->layout());
    for (size_t i = 0; i < chips.size(); ++i) {
        auto* caption = new QLabel(chips[i].caption, chip_bar);
        caption->setObjectName(QStringLiteral("HomeChipCaption"));
        auto* button = new QPushButton(chip_bar);
        button->setObjectName(QStringLiteral("HomeChip"));
        button->setFocusPolicy(Qt::NoFocus);
        const int column = static_cast<int>(i);
        connect(button, &QPushButton::clicked, this, [this, column] {
            focused_chip = column;
            SetRow(Row::Chips);
            ActivateChip(column);
        });
        layout->addWidget(caption, 0, column);
        layout->addWidget(button, 1, column);
        chip_captions.push_back(caption);
        chip_buttons.push_back(button);
    }
    layout->setColumnStretch(static_cast<int>(chips.size()), 1);
    focused_chip = std::clamp(focused_chip, 0, std::max(0, static_cast<int>(chips.size()) - 1));
    LayoutChrome();
    RefreshChips();
}

void GameHome::RefreshChips() {
    for (size_t i = 0; i < chips.size() && i < chip_buttons.size(); ++i) {
        QPushButton* button = chip_buttons[i];
        const QString text = chips[i].text ? ChipCase(chips[i].text()) : QString{};
        if (button->text() != text) {
            button->setText(text);
        }
        const bool inactive = chips[i].active && !chips[i].active();
        if (button->property("inactive").toBool() != inactive ||
            button->styleSheet().isEmpty()) {
            button->setProperty("inactive", inactive);
            StyleChip(i);
        }
    }
}

void GameHome::StyleChip(size_t index) {
    if (index >= chip_buttons.size() || index >= chips.size()) {
        return;
    }
    QPushButton* button = chip_buttons[index];
    const QColor color = chips[index].color;
    const bool inactive = button->property("inactive").toBool();
    const bool focus = button->property("homeFocus").toBool();
    const bool adjust = button->property("adjusting").toBool();
    const auto rgba = [&color](double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(alpha);
    };

    // The status bar's colors: a tint of the category color, grey when the setting is off. The
    // focused chip gets a border in its own color, white while it is being adjusted.
    QString background = inactive ? QStringLiteral("#17181c") : rgba(focus ? 0.28 : 0.14);
    const QString text = inactive ? QStringLiteral("#5c5e65") : color.name();
    QString border = QStringLiteral("transparent");
    if (adjust) {
        border = QStringLiteral("#ffffff");
    } else if (focus) {
        border = inactive ? QStringLiteral("#8b8d94") : color.name();
    }
    if (inactive && focus) {
        background = QStringLiteral("#202227");
    }
    button->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; color: %2; border: 2px solid %3;"
                       " border-radius: 8px; font-size: 15px; min-height: 44px;"
                       " max-height: 44px; padding: 0 10px; }"
                       "QPushButton:hover { background-color: %4; }")
            .arg(background, text, border, inactive ? QStringLiteral("#202227") : rgba(0.24)));
}

void GameHome::SetButtonStyle(ButtonStyle style) {
    if (button_style == style) {
        return;
    }
    button_style = style;
    UpdateHints();
}

void GameHome::RebuildPills() {
    for (QPushButton* pill : pills) {
        pill->deleteLater();
    }
    pills.clear();
    pill_menus.clear();
    if (!menu_bar) {
        return;
    }
    auto* layout = static_cast<QHBoxLayout*>(pill_bar->layout());
    for (QAction* action : menu_bar->actions()) {
        QMenu* menu = action->menu();
        if (menu == nullptr || !action->isVisible()) {
            continue;
        }
        QString text = action->text();
        text.remove(QLatin1Char{'&'});
        auto* pill = new QPushButton(text, pill_bar);
        pill->setObjectName(QStringLiteral("HomePill"));
        pill->setFocusPolicy(Qt::NoFocus);
        pill->setMinimumWidth(118);
        const int index = static_cast<int>(pills.size());
        connect(pill, &QPushButton::clicked, this, [this, index] { OpenMenu(index); });
        layout->insertWidget(index, pill);
        pills.push_back(pill);
        pill_menus.emplace_back(menu);
    }
    focused_pill = std::clamp(focused_pill, 0, std::max(0, static_cast<int>(pills.size()) - 1));
    UpdateFocusStyles();
}

bool GameHome::CardsFocused() const {
    return row == Row::Cards && hasFocus();
}

qreal GameHome::Pulse() const {
    return pulse_animation->currentValue().toReal();
}

void GameHome::LayoutChrome() {
    const int w = width();
    const int h = height();

    constexpr int pill_top = 20;
    constexpr int pill_h = 44;
    constexpr int header_h = 66;
    constexpr int caption_h = 18;
    constexpr int chip_h = 46;
    constexpr int cards_to_chips = 26;
    constexpr int chips_to_hints = 18;
    constexpr int hint_h = 44;
    constexpr int bottom_pad = 14;
    ring_pad = 12;

    const int top_fixed = pill_top + pill_h;
    const int chips_block = caption_h + 6 + chip_h;
    const int bottom_fixed = cards_to_chips + chips_block + chips_to_hints + hint_h + bottom_pad;
    const int available = h - top_fixed - header_h - bottom_fixed - 2 * ring_pad;
    card_side = std::clamp(available, 120, 440);
    const int extra = std::max(0, available - card_side);
    const int spacer_top = extra * 55 / 100;

    const int top = top_fixed + spacer_top + header_h;
    const int view_h = card_side + 2 * ring_pad;
    const int bottom = std::max(0, h - top - view_h);

    gap = std::max(14, card_side / 14);
    const int side_pad = gap + ring_pad;

    card_delegate->SetGeometry(card_side, ring_pad);
    if (spacing() != gap) {
        setSpacing(gap);
    }
    if (viewportMargins() != QMargins(0, top, 0, bottom)) {
        setViewportMargins(0, top, 0, bottom);
    }
    scheduleDelayedItemsLayout();

    const int inner_w = std::max(0, w - 2 * side_pad);
    pill_bar->setGeometry(side_pad, pill_top, inner_w, pill_h);
    header->setGeometry(side_pad, top - header_h, inner_w, header_h - 4);
    chip_bar->setGeometry(side_pad, top + view_h + cards_to_chips, inner_w, chips_block);
    // Up to 136 px a chip; narrower windows share the width out.
    if (!chip_buttons.empty()) {
        constexpr int chip_spacing = 14;
        const int count = static_cast<int>(chip_buttons.size());
        const int chip_w =
            std::clamp((inner_w - (count - 1) * chip_spacing) / count, 64, 136);
        for (QPushButton* button : chip_buttons) {
            button->setFixedWidth(chip_w);
        }
        for (QLabel* caption : chip_captions) {
            caption->setFixedWidth(chip_w);
        }
    }
    hint_bar->setGeometry(side_pad, h - bottom_pad - hint_h, inner_w, hint_h);
}

void GameHome::resizeEvent(QResizeEvent* event) {
    QListView::resizeEvent(event);
    LayoutChrome();
}

void GameHome::showEvent(QShowEvent* event) {
    QListView::showEvent(event);
    if (menu_bar) {
        // The pills replace the menu bar here. Its actions keep their shortcuts while it is
        // hidden only if the window holds them too.
        if (!shortcuts_moved) {
            QList<QAction*> with_shortcuts;
            const std::function<void(const QList<QAction*>&)> collect =
                [&](const QList<QAction*>& actions) {
                    for (QAction* action : actions) {
                        if (QMenu* menu = action->menu()) {
                            collect(menu->actions());
                        } else if (!action->shortcut().isEmpty()) {
                            with_shortcuts.append(action);
                        }
                    }
                };
            collect(menu_bar->actions());
            window()->addActions(with_shortcuts);
            shortcuts_moved = true;
        }
        RebuildPills();
    }
    LayoutChrome();
    RefreshChips();
    EnsureCurrent();
    UpdateHeader();
    pulse_animation->start();
    refresh_timer->start();
    QTimer::singleShot(0, this, [this] {
        if (isVisible() && isEnabled()) {
            setFocus(Qt::OtherFocusReason);
        }
    });
}

void GameHome::hideEvent(QHideEvent* event) {
    QListView::hideEvent(event);
    pulse_animation->stop();
    refresh_timer->stop();
    adjusting = false;
}

void GameHome::SetRow(Row row_) {
    if (row != row_) {
        row = row_;
        adjusting = false;
    }
    UpdateFocusStyles();
    UpdateHints();
    viewport()->update();
}

void GameHome::UpdateFocusStyles() {
    for (size_t i = 0; i < pills.size(); ++i) {
        const bool focus = row == Row::Menus && static_cast<int>(i) == focused_pill;
        if (pills[i]->property("homeFocus").toBool() != focus) {
            pills[i]->setProperty("homeFocus", focus);
            Repolish(pills[i]);
        }
    }
    for (size_t i = 0; i < chip_buttons.size(); ++i) {
        const bool focus = row == Row::Chips && static_cast<int>(i) == focused_chip;
        const bool adjust = focus && adjusting;
        if (chip_buttons[i]->property("homeFocus").toBool() != focus ||
            chip_buttons[i]->property("adjusting").toBool() != adjust) {
            chip_buttons[i]->setProperty("homeFocus", focus);
            chip_buttons[i]->setProperty("adjusting", adjust);
            StyleChip(i);
        }
    }
}

void GameHome::UpdateHeader() {
    const QModelIndex current = currentIndex();
    if (!current.isValid() || isRowHidden(current.row())) {
        status_label->clear();
        title_label->setText(tr("No games"));
        return;
    }
    const QString title = current.data(GameListItemPath::TitleRole).toString();
    const QString file_type = current.data(GameListItemPath::FileTypeRole).toString();
    const u64 play_time =
        model()
            ->index(current.row(), GameListModel::COLUMN_PLAY_TIME)
            .data(GameListItemPlayTime::PlayTimeRole)
            .toULongLong();
    const QString played =
        play_time > 0
            ? tr("Played %1")
                  .arg(QString::fromStdString(
                      PlayTime::PlayTimeManager::GetReadablePlayTime(play_time)))
            : tr("Not played yet");
    status_label->setText(file_type.isEmpty() ? played
                                              : QStringLiteral("%1    %2").arg(file_type, played));
    title_label->setText(
        QStringLiteral("<span style=\"color:#4169e1;\">&#9679;</span>&nbsp;&nbsp;%1")
            .arg(title.toHtmlEscaped()));
}

void GameHome::UpdateHints() {
    struct Hint {
        HintRole role;
        QString text;
        std::function<void()> action;
    };
    std::vector<Hint> hints;
    switch (row) {
    case Row::Cards:
        hints = {{HintRole::Options, tr("Options"), [this] { OpenOptions(); }},
                 {HintRole::Confirm, tr("Play"), [this] { ActivateCurrent(); }}};
        break;
    case Row::Chips:
        if (adjusting) {
            hints = {{HintRole::Adjust, tr("Adjust"), nullptr},
                     {HintRole::Confirm, tr("Done"), [this] { ActivateChip(focused_chip); }}};
        } else {
            hints = {{HintRole::Back, tr("Back"), [this] { SetRow(Row::Cards); }},
                     {HintRole::Confirm, tr("Change"), [this] { ActivateChip(focused_chip); }}};
        }
        break;
    case Row::Menus:
        hints = {{HintRole::Back, tr("Back"), [this] { SetRow(Row::Cards); }},
                 {HintRole::Confirm, tr("Open"), [this] { OpenMenu(focused_pill); }}};
        break;
    }

    auto* layout = static_cast<QHBoxLayout*>(hint_bar->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    layout->addStretch(1);
    for (const Hint& hint : hints) {
        auto* button = new QPushButton(hint_bar);
        button->setObjectName(QStringLiteral("HomeHint"));
        button->setFocusPolicy(Qt::NoFocus);
        button->setFlat(true);
        auto* inner = new QHBoxLayout(button);
        inner->setContentsMargins(6, 0, 6, 0);
        inner->setSpacing(8);
        auto* glyph = new HomeGlyph(button_style, hint.role, button);
        auto* text = new QLabel(hint.text, button);
        text->setObjectName(QStringLiteral("HomeHintText"));
        text->setAttribute(Qt::WA_TransparentForMouseEvents);
        inner->addWidget(glyph);
        inner->addWidget(text);
        button->setMinimumWidth(glyph->width() + text->sizeHint().width() + 32);
        button->setMinimumHeight(36);
        if (hint.action) {
            connect(button, &QPushButton::clicked, this, hint.action);
        } else {
            button->setEnabled(false);
        }
        layout->addWidget(button);
    }
}

void GameHome::EnsureCurrent() {
    if (model() == nullptr) {
        return;
    }
    const QModelIndex current = currentIndex();
    if (current.isValid() && !isRowHidden(current.row())) {
        return;
    }
    for (int i = 0; i < model()->rowCount(); ++i) {
        if (!isRowHidden(i)) {
            setCurrentIndex(model()->index(i, 0));
            return;
        }
    }
}

void GameHome::MoveSelection(int delta) {
    if (model() == nullptr) {
        return;
    }
    std::vector<int> visible;
    for (int i = 0; i < model()->rowCount(); ++i) {
        if (!isRowHidden(i)) {
            visible.push_back(i);
        }
    }
    if (visible.empty()) {
        return;
    }
    const auto it = std::find(visible.begin(), visible.end(), currentIndex().row());
    const int position = it == visible.end() ? 0 : static_cast<int>(it - visible.begin());
    const int next = std::clamp(position + delta, 0, static_cast<int>(visible.size()) - 1);
    setCurrentIndex(model()->index(visible[static_cast<size_t>(next)], 0));
}

void GameHome::StepSelection(int delta) {
    SetRow(Row::Cards);
    MoveSelection(delta);
}

void GameHome::ActivateCurrent() {
    const QModelIndex current = currentIndex();
    if (current.isValid() && !isRowHidden(current.row())) {
        emit activated(current);
    }
}

void GameHome::OpenOptions() {
    const QModelIndex current = currentIndex();
    if (current.isValid() && !isRowHidden(current.row())) {
        emit customContextMenuRequested(visualRect(current).center());
    }
}

void GameHome::OpenMenu(int index) {
    if (index < 0 || index >= static_cast<int>(pills.size()) || !pill_menus[index]) {
        return;
    }
    focused_pill = index;
    SetRow(Row::Menus);
    QPushButton* pill = pills[static_cast<size_t>(index)];
    pill_menus[static_cast<size_t>(index)]->exec(pill->mapToGlobal(QPoint(0, pill->height() + 6)));
    // Actions in the menu can switch the view away, or start a game.
    if (isVisible()) {
        setFocus(Qt::OtherFocusReason);
    }
}

void GameHome::ActivateChip(int index) {
    if (index < 0 || index >= static_cast<int>(chips.size())) {
        return;
    }
    const HomeChip& chip = chips[static_cast<size_t>(index)];
    if (chip.adjust && !chip.activate) {
        adjusting = !adjusting;
        UpdateFocusStyles();
        UpdateHints();
        return;
    }
    if (chip.activate) {
        chip.activate();
    }
    RefreshChips();
}

void GameHome::scrollTo(const QModelIndex& index, ScrollHint) {
    if (!index.isValid()) {
        return;
    }
    QScrollBar* bar = horizontalScrollBar();
    const QRect rect = visualRect(index);
    const int look_ahead = card_side / 2;
    const int value = scroll_animation->state() == QAbstractAnimation::Running
                          ? scroll_animation->endValue().toInt()
                          : bar->value();
    const int shown = value - bar->value();
    int target = value;
    if (rect.right() - shown + look_ahead > viewport()->width()) {
        target = value + rect.right() - shown + look_ahead - viewport()->width();
    } else if (rect.left() - shown - look_ahead < 0) {
        target = value + rect.left() - shown - look_ahead;
    }
    target = std::clamp(target, 0, bar->maximum());
    if (target == value) {
        return;
    }
    scroll_animation->stop();
    scroll_animation->setStartValue(bar->value());
    scroll_animation->setEndValue(target);
    scroll_animation->start();
}

void GameHome::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QListView::currentChanged(current, previous);
    if (current.isValid()) {
        scrollTo(current);
    }
    UpdateHeader();
    viewport()->update();
}

void GameHome::mousePressEvent(QMouseEvent* event) {
    SetRow(Row::Cards);
    QListView::mousePressEvent(event);
}

void GameHome::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const bool confirm = key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space;
    const bool back = key == Qt::Key_Escape || key == Qt::Key_Backspace;

    switch (row) {
    case Row::Cards:
        if (key == Qt::Key_Left) {
            MoveSelection(-1);
        } else if (key == Qt::Key_Right) {
            MoveSelection(1);
        } else if (key == Qt::Key_PageUp) {
            MoveSelection(-5);
        } else if (key == Qt::Key_PageDown) {
            MoveSelection(5);
        } else if (key == Qt::Key_Home) {
            MoveSelection(-(1 << 20));
        } else if (key == Qt::Key_End) {
            MoveSelection(1 << 20);
        } else if (key == Qt::Key_Up) {
            if (!pills.empty()) {
                SetRow(Row::Menus);
            }
        } else if (key == Qt::Key_Down) {
            if (!chip_buttons.empty()) {
                SetRow(Row::Chips);
            }
        } else if (confirm) {
            ActivateCurrent();
        } else if (key == Qt::Key_Menu || key == Qt::Key_X || key == Qt::Key_Plus) {
            // The controller's X and + arrive as the menu key; on a keyboard X and + work too.
            OpenOptions();
        } else if (back) {
            // Nothing to go back to from the cards.
        } else {
            QListView::keyPressEvent(event);
            return;
        }
        break;
    case Row::Chips: {
        const int count = static_cast<int>(chip_buttons.size());
        if (adjusting) {
            const HomeChip& chip = chips[static_cast<size_t>(focused_chip)];
            if (key == Qt::Key_Left || key == Qt::Key_Down) {
                chip.adjust(-1);
                RefreshChips();
            } else if (key == Qt::Key_Right || key == Qt::Key_Up) {
                chip.adjust(1);
                RefreshChips();
            } else if (confirm || back) {
                adjusting = false;
                UpdateFocusStyles();
                UpdateHints();
            }
        } else if (key == Qt::Key_Left) {
            focused_chip = std::max(0, focused_chip - 1);
            UpdateFocusStyles();
        } else if (key == Qt::Key_Right) {
            focused_chip = std::min(count - 1, focused_chip + 1);
            UpdateFocusStyles();
        } else if (key == Qt::Key_Up || back) {
            SetRow(Row::Cards);
        } else if (confirm) {
            ActivateChip(focused_chip);
        }
        break;
    }
    case Row::Menus: {
        const int count = static_cast<int>(pills.size());
        if (key == Qt::Key_Left) {
            focused_pill = std::max(0, focused_pill - 1);
            UpdateFocusStyles();
        } else if (key == Qt::Key_Right) {
            focused_pill = std::min(count - 1, focused_pill + 1);
            UpdateFocusStyles();
        } else if (key == Qt::Key_Down || back) {
            SetRow(Row::Cards);
        } else if (confirm) {
            OpenMenu(focused_pill);
        }
        break;
    }
    }
    event->accept();
}
