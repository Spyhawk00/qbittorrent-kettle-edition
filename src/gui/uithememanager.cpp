/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2023-2026  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2019, 2021  Prince Gupta <jagannatharjun11@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If
 * you modify file(s), you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version.
 */

#include "uithememanager.h"

#include <QApplication>
#include <QEvent>
#include <QList>
#include <QPalette>
#include <QPixmapCache>
#include <QResource>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>

#include "base/global.h"
#include "base/logger.h"
#include "base/path.h"
#include "base/profile.h"
#include "base/preferences.h"
#include "appstyleproxy.h"
#include "uithemecommon.h"

#ifdef Q_OS_WIN
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProxyStyle>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStyleOption>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>

#include <windows.h>
#include <dwmapi.h>

#include "base/utils/reg.h"
#endif

using namespace Qt::Literals::StringLiterals;

namespace
{
    bool isDarkTheme()
    {
        const QPalette palette = qApp->palette();
        const QColor &color = palette.color(QPalette::Active, QPalette::Base);
        return (color.lightness() < 127);
    }

#ifdef Q_OS_WIN
    // DWMWA_CAPTION_COLOR and DWMWA_TEXT_COLOR (Windows 11 and later)
    // DWMWA_USE_IMMERSIVE_DARK_MODE
    const DWORD DWM_DARK_MODE = 20;
    const DWORD DWM_CAPTION_COLOR = 35;
    const DWORD DWM_TEXT_COLOR = 36;

    QColor mixColors(const QColor &base, const QColor &top, const int topPercent)
    {
        return QColor(((base.red() * (100 - topPercent)) + (top.red() * topPercent)) / 100
            , ((base.green() * (100 - topPercent)) + (top.green() * topPercent)) / 100
            , ((base.blue() * (100 - topPercent)) + (top.blue() * topPercent)) / 100);
    }

    COLORREF toColorRef(const QColor &color)
    {
        return RGB(color.red(), color.green(), color.blue());
    }

    // the outline of every field and list
    QColor outlineColor(const QStyleOption *option)
    {
        return OutlineColor::get(option->palette);
    }

    void drawOutline(const QStyleOption *option, QPainter *painter, const qreal radius)
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(outlineColor(option));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        painter->restore();
    }

    // the heavier outline of the default button
    void drawDefaultButtonOutline(const QStyleOption *option, QPainter *painter, const qreal radius)
    {
        QStyleOption inner {*option};
        inner.rect = option->rect.adjusted(1, 1, -1, -1);
        drawOutline(option, painter, radius);
        drawOutline(&inner, painter, (radius - 1));
    }

    // square push buttons, a little larger than the Windows 11 style makes them, and square fields with an outline
    // all around (the Windows 11 style draws only a line under them)
    class ControlStyle final : public QProxyStyle
    {
    public:
        ControlStyle()
            : QProxyStyle(u"fusion"_s)
        {
        }

        QSize sizeFromContents(const ContentsType type, const QStyleOption *option
            , const QSize &size, const QWidget *widget) const override
        {
            const QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
            if (type == CT_PushButton)
                return (result + QSize(6, 2));
            return result;
        }

        int styleHint(const StyleHint hint, const QStyleOption *option, const QWidget *widget
            , QStyleHintReturn *returnData) const override
        {
            // Fusion shows the list of a drop-down box like a menu, with grey rows: a plain white list instead
            if (hint == SH_ComboBox_Popup)
                return 0;
            return QProxyStyle::styleHint(hint, option, widget, returnData);
        }

        void drawPrimitive(const PrimitiveElement element, const QStyleOption *option, QPainter *painter
            , const QWidget *widget) const override
        {
            // the default button is marked by a heavier outline in the color of the others, not in the accent color of Windows
            if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)
                ; button && (element == PE_PanelButtonCommand) && button->features.testFlag(QStyleOptionButton::DefaultButton))
            {
                QStyleOptionButton plainButton {*button};
                plainButton.features &= ~QStyleOptionButton::DefaultButton;
                drawPrimitive(element, &plainButton, painter, widget);
                if (option->state.testFlag(State_Enabled))
                    drawDefaultButtonOutline(option, painter, 2);
                return;
            }

            if (hasClearWindow(option))
            {
                if (const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option))
                {
                    const QStyleOptionFrame opaqueFrame = withOpaqueWindow(*frame);
                    drawPrimitive(element, &opaqueFrame, painter, widget);
                    return;
                }
                if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option))
                {
                    const QStyleOptionButton opaqueButton = withOpaqueWindow(*button);
                    drawPrimitive(element, &opaqueButton, painter, widget);
                    return;
                }
            }

            QProxyStyle::drawPrimitive(element, option, painter, widget);
            if (element == PE_FrameLineEdit)
                drawFieldOutline(option, painter, 2);
            else if ((element == PE_Frame) && (qobject_cast<const QTextEdit *>(widget) || qobject_cast<const QPlainTextEdit *>(widget)))
                drawFieldOutline(option, painter, 0);
        }

        void drawComplexControl(const ComplexControl control, const QStyleOptionComplex *option, QPainter *painter
            , const QWidget *widget) const override
        {
            if (hasClearWindow(option))
            {
                if (const auto *spinBox = qstyleoption_cast<const QStyleOptionSpinBox *>(option))
                {
                    const QStyleOptionSpinBox opaqueSpinBox = withOpaqueWindow(*spinBox);
                    drawComplexControl(control, &opaqueSpinBox, painter, widget);
                    return;
                }
                if (const auto *comboBox = qstyleoption_cast<const QStyleOptionComboBox *>(option))
                {
                    const QStyleOptionComboBox opaqueComboBox = withOpaqueWindow(*comboBox);
                    drawComplexControl(control, &opaqueComboBox, painter, widget);
                    return;
                }
            }

            QProxyStyle::drawComplexControl(control, option, painter, widget);
            if (const auto *spinBox = qstyleoption_cast<const QStyleOptionSpinBox *>(option); spinBox && spinBox->frame)
            {
                // Fusion draws the frame of a spin box a row inside its top and bottom: the outline goes on that frame
                QStyleOptionSpinBox frameOption {*spinBox};
                frameOption.rect.adjust(0, 1, 0, -1);
                drawFieldOutline(&frameOption, painter, 2);
            }
            else if (const auto *comboBox = qstyleoption_cast<const QStyleOptionComboBox *>(option); comboBox && comboBox->editable && comboBox->frame)
                drawFieldOutline(option, painter, 2);
        }

    private:
        // the Windows 11 style makes the window color inside a scroll area (every Options page) transparent, and Fusion
        // draws its outlines in a shade of the window color: those controls are drawn with the window color of the application
        static bool hasClearWindow(const QStyleOption *option)
        {
            return (option->palette.color(QPalette::Window).alpha() == 0);
        }

        template <typename Option>
        static Option withOpaqueWindow(const Option &option)
        {
            Option opaqueOption {option};
            opaqueOption.palette.setBrush(QPalette::Window, QApplication::palette().window());
            return opaqueOption;
        }

        // a field with the focus keeps the accent outline of Fusion
        static void drawFieldOutline(const QStyleOption *option, QPainter *painter, const qreal radius)
        {
            if (!option->state.testFlag(State_HasFocus))
                drawOutline(option, painter, radius);
        }
    };

    // a frame that sits next to lists (a read-only text view, the filter sidebar) asks for their outline with the property
    // "outlined"
    bool isOutlinedFrame(const QObject *object)
    {
        return qobject_cast<const QFrame *>(object) && object->property("outlined").toBool();
    }

    // the fill of an outlined frame: the one of the fields, or the one of the window for a page with a box around it
    QBrush outlinedFill(const QObject *object, const QPalette &palette)
    {
        return (object->property("outlined").toString() == u"window") ? palette.window() : palette.base();
    }

    // lists get the outline of the fields (not the column headers or the list of a drop-down list)
    bool isOutlinedList(const QObject *object)
    {
        const auto *view = qobject_cast<const QAbstractItemView *>(object);
        return view && !qobject_cast<const QHeaderView *>(view)
            && !(view->parentWidget() && view->parentWidget()->inherits("QComboBoxPrivateContainer"));
    }

    // lists keep the Windows 11 look, inside the square outline of the fields
    class ListStyle final : public QProxyStyle
    {
    public:
        ListStyle()
            : QProxyStyle(u"windows11"_s)
        {
        }

        void drawPrimitive(const PrimitiveElement element, const QStyleOption *option, QPainter *painter
            , const QWidget *widget) const override
        {
            if ((element == PE_Frame) && (qobject_cast<const QAbstractItemView *>(widget) || isOutlinedFrame(widget)))
            {
                painter->fillRect(option->rect, outlinedFill(widget, option->palette));
                drawOutline(option, painter, 0);
                return;
            }

            QProxyStyle::drawPrimitive(element, option, painter, widget);
        }

        void drawControl(const ControlElement element, const QStyleOption *option, QPainter *painter
            , const QWidget *widget) const override
        {
            // the Windows 11 style draws the frame of a text view as a dark line under it
            if ((element == CE_ShapedFrame) && isOutlinedFrame(widget))
            {
                drawPrimitive(PE_Frame, option, painter, widget);
                return;
            }

            if (const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(option); header && (element == CE_HeaderLabel))
            {
                const QStyleOptionHeader padded = ItemViewSpacing::paddedHeaderLabel(*header);
                QProxyStyle::drawControl(element, &padded, painter, widget);
                return;
            }

            QProxyStyle::drawControl(element, option, painter, widget);
        }

        QSize sizeFromContents(const ContentsType type, const QStyleOption *option, const QSize &size
            , const QWidget *widget) const override
        {
            return ItemViewSpacing::sizeFromContents(type, QProxyStyle::sizeFromContents(type, option, size, widget), widget);
        }

        int pixelMetric(const PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
        {
            return ItemViewSpacing::pixelMetric(metric, widget, QProxyStyle::pixelMetric(metric, option, widget));
        }
    };

    // the scroll bars of the application, all the same with every style: a flat bar of 8 px with a track and a thumb in the
    // colors of the window, a line next to the content, no arrow buttons and no bars that hide when not used
    class ScrollBarStyle final : public AppStyleProxy
    {
    public:
        using AppStyleProxy::AppStyleProxy;

        int pixelMetric(const PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
        {
            if (metric == PM_ScrollBarExtent)
                return 8;
            return QProxyStyle::pixelMetric(metric, option, widget);
        }

        int styleHint(const StyleHint hint, const QStyleOption *option, const QWidget *widget
            , QStyleHintReturn *returnData) const override
        {
            if (hint == SH_ScrollBar_Transient)
                return 0;
            return QProxyStyle::styleHint(hint, option, widget, returnData);
        }

        QRect subControlRect(const ComplexControl control, const QStyleOptionComplex *option, const SubControl subControl
            , const QWidget *widget) const override
        {
            const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option);
            if ((control != CC_ScrollBar) || !bar)
                return QProxyStyle::subControlRect(control, option, subControl, widget);

            const bool horizontal = (bar->orientation == Qt::Horizontal);
            const int length = horizontal ? bar->rect.width() : bar->rect.height();
            const int range = bar->maximum - bar->minimum;
            int thumbLength = length;
            if (range > 0)
            {
                thumbLength = static_cast<int>((static_cast<qint64>(bar->pageStep) * length) / (range + bar->pageStep));
                thumbLength = std::min(std::max(thumbLength, QProxyStyle::pixelMetric(PM_ScrollBarSliderMin, bar, widget)), length);
            }
            const int thumbPos = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition
                , (length - thumbLength), bar->upsideDown);
            const auto part = [bar, horizontal](const int start, const int size)
            {
                return (horizontal ? QRect((bar->rect.x() + start), bar->rect.y(), size, bar->rect.height())
                    : QRect(bar->rect.x(), (bar->rect.y() + start), bar->rect.width(), size));
            };

            switch (subControl)
            {
            case SC_ScrollBarGroove:
                return bar->rect;
            case SC_ScrollBarSlider:
                return part(thumbPos, thumbLength);
            case SC_ScrollBarSubPage:
            case SC_ScrollBarAddPage:
                // (the page that is first in the bar depends on the direction)
                return (((subControl == SC_ScrollBarSubPage) != bar->upsideDown)
                    ? part(0, thumbPos) : part((thumbPos + thumbLength), (length - thumbPos - thumbLength)));
            case SC_ScrollBarSubLine:
            case SC_ScrollBarAddLine:
                return {};
            default:
                return QProxyStyle::subControlRect(control, option, subControl, widget);
            }
        }

        SubControl hitTestComplexControl(const ComplexControl control, const QStyleOptionComplex *option, const QPoint &position
            , const QWidget *widget) const override
        {
            if (control != CC_ScrollBar)
                return QProxyStyle::hitTestComplexControl(control, option, position, widget);

            for (const SubControl subControl : {SC_ScrollBarSlider, SC_ScrollBarAddPage, SC_ScrollBarSubPage})
            {
                if (subControlRect(control, option, subControl, widget).contains(position))
                    return subControl;
            }
            return SC_None;
        }

        void drawComplexControl(const ComplexControl control, const QStyleOptionComplex *option, QPainter *painter
            , const QWidget *widget) const override
        {
            const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option);
            if ((control != CC_ScrollBar) || !bar)
            {
                QProxyStyle::drawComplexControl(control, option, painter, widget);
                return;
            }

            // (the window color of a page in a scroll area can be clear with the Windows 11 style)
            const QColor window = QApplication::palette().color(QPalette::Window);
            const bool dark = isDarkTheme();
            const bool horizontal = (bar->orientation == Qt::Horizontal);

            painter->save();
            painter->fillRect(bar->rect, (dark ? window.lighter(112) : window.darker(104)));
            painter->setPen(outlineColor(bar));
            if (horizontal)
                painter->drawLine(bar->rect.topLeft(), bar->rect.topRight());
            else
                painter->drawLine(bar->rect.topLeft(), bar->rect.bottomLeft());

            if (bar->maximum > bar->minimum)
            {
                const bool pressed = bar->state.testFlag(State_Sunken) && (bar->activeSubControls == SC_ScrollBarSlider);
                const bool hovered = bar->state.testFlag(State_MouseOver) && (bar->activeSubControls == SC_ScrollBarSlider);
                const int factor = dark ? (pressed ? 320 : (hovered ? 270 : 220)) : (pressed ? 190 : (hovered ? 160 : 130));
                // (the line next to the content is not part of the thumb's room)
                const QRect room = horizontal ? bar->rect.adjusted(0, 1, 0, 0) : bar->rect.adjusted(1, 0, 0, 0);
                const QRect thumb = subControlRect(control, bar, SC_ScrollBarSlider, widget).intersected(room)
                    .adjusted((horizontal ? 2 : 1), (horizontal ? 1 : 2), (horizontal ? -2 : -1), (horizontal ? -1 : -2));
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(Qt::NoPen);
                painter->setBrush(dark ? window.lighter(factor) : window.darker(factor));
                painter->drawRoundedRect(thumb, 2, 2);
            }
            painter->restore();
        }
    };

    // the grip of the handles between the panes of the window, in the same place and colors with every style: six squares
    // centered on the handle (the page shows around them), dark enough to be seen on the page in a light scheme too
    class SplitterStyle final : public AppStyleProxy
    {
    public:
        using AppStyleProxy::AppStyleProxy;

        void drawControl(const ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
        {
            if (element != CE_Splitter)
            {
                QProxyStyle::drawControl(element, option, painter, widget);
                return;
            }

            // a handle that cannot be moved has no grip
            if (!option->state.testFlag(State_Enabled))
                return;

            const QPalette &palette = option->palette;

            constexpr int count = 6;
            constexpr int size = 2;
            constexpr int gap = 2;
            constexpr int length = (count * size) + ((count - 1) * gap);
            const bool lengthwise = (option->rect.height() >= option->rect.width());
            const QPointF center = QRectF(option->rect).center();
            const QColor color = mixColors(palette.color(QPalette::Window), palette.color(QPalette::WindowText), 60);
            for (int i = 0; i < count; ++i)
            {
                const qreal offset = (i * (size + gap)) - (length / 2.0);
                const QRectF square = lengthwise
                    ? QRectF((center.x() - (size / 2.0)), (center.y() + offset), size, size)
                    : QRectF((center.x() + offset), (center.y() - (size / 2.0)), size, size);
                painter->fillRect(square, color);
            }
        }
    };

    // the outline of the lists and of the frames that sit next to them with the other styles (see isOutlinedFrame()): the one
    // of the Windows 11 style, so every box has the same
    class OutlinedFrameStyle final : public AppStyleProxy
    {
    public:
        using AppStyleProxy::AppStyleProxy;

        void drawControl(const ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
        {
            if ((element == CE_ShapedFrame) && isOutlinedFrame(widget))
            {
                painter->fillRect(option->rect, outlinedFill(widget, option->palette));
                drawOutline(option, painter, 0);
                return;
            }
            QProxyStyle::drawControl(element, option, painter, widget);
        }
    };

    // a list of the application with the other styles: its spacing, and the outline of OutlinedFrameStyle
    class OutlinedListStyle final : public ItemViewSpacingStyle
    {
    public:
        using ItemViewSpacingStyle::ItemViewSpacingStyle;

        void drawPrimitive(const PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
        {
            if ((element == PE_Frame) && isOutlinedList(widget))
            {
                drawOutline(option, painter, 0);
                return;
            }

            // the Windows Vista style draws a selected row as a grey gradient with a dark edge while the list does not have the
            // focus: a flat fill in the color of the selection, like the other styles
            if (const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option)
                ; item && (element == PE_PanelItemViewItem) && (baseStyle()->name() == u"windowsvista"_s))
            {
                if (item->backgroundBrush.style() != Qt::NoBrush)
                    painter->fillRect(item->rect, item->backgroundBrush);
                if (item->state.testFlag(State_Selected))
                {
                    painter->fillRect(item->rect, item->palette.color(QPalette::Active, QPalette::Highlight));
                }
                else if (item->state.testFlag(State_MouseOver))
                {
                    QColor hover = item->palette.color(QPalette::Active, QPalette::Highlight);
                    hover.setAlphaF(0.15);
                    painter->fillRect(item->rect, hover);
                }
                return;
            }
            QProxyStyle::drawPrimitive(element, option, painter, widget);
        }

        void drawControl(const ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
        {
            // (the text of a selected row is drawn in the color for the active window too)
            if (const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option)
                ; item && (element == CE_ItemViewItem) && item->state.testFlag(State_Selected) && (baseStyle()->name() == u"windowsvista"_s))
            {
                QStyleOptionViewItem active {*item};
                active.state |= State_Active;
                active.palette.setCurrentColorGroup(QPalette::Active);
                QProxyStyle::drawControl(element, &active, painter, widget);
                return;
            }
            QProxyStyle::drawControl(element, option, painter, widget);
        }
    };

    // Fusion marks the default button with an outline in the accent color of Windows: the heavier outline of the others instead
    class DefaultButtonStyle final : public AppStyleProxy
    {
    public:
        using AppStyleProxy::AppStyleProxy;

        void drawPrimitive(const PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
        {
            if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)
                ; button && (element == PE_PanelButtonCommand) && button->features.testFlag(QStyleOptionButton::DefaultButton))
            {
                QStyleOptionButton plainButton {*button};
                plainButton.features &= ~QStyleOptionButton::DefaultButton;
                QProxyStyle::drawPrimitive(element, &plainButton, painter, widget);
                if (option->state.testFlag(State_Enabled))
                    drawDefaultButtonOutline(option, painter, 2);
                return;
            }
            QProxyStyle::drawPrimitive(element, option, painter, widget);
        }
    };

    // the classic Windows style draws a drop-down list as a button in the color of the window, so it blends in with the
    // page: it gets the white of the other fields
    class ClassicDropDownStyle final : public QProxyStyle
    {
    public:
        ClassicDropDownStyle()
            : QProxyStyle(u"windows"_s)
        {
        }

        void drawComplexControl(const ComplexControl control, const QStyleOptionComplex *option, QPainter *painter
            , const QWidget *widget) const override
        {
            if (const auto *comboBox = qstyleoption_cast<const QStyleOptionComboBox *>(option))
            {
                QStyleOptionComboBox whiteComboBox {*comboBox};
                whiteComboBox.palette.setBrush(QPalette::Button, option->palette.base());
                QProxyStyle::drawComplexControl(control, &whiteComboBox, painter, widget);
                return;
            }
            QProxyStyle::drawComplexControl(control, option, painter, widget);
        }
    };

    // push buttons and every field are square: text, number and time boxes, multi-line text boxes and drop-down lists
    // (not the read-only text views)
    bool isSquareControl(const QObject *object)
    {
        return qobject_cast<const QPushButton *>(object) || qobject_cast<const QLineEdit *>(object)
            || qobject_cast<const QAbstractSpinBox *>(object) || qobject_cast<const QComboBox *>(object)
            || qobject_cast<const QPlainTextEdit *>(object)
            || (qobject_cast<const QTextEdit *>(object) && !qobject_cast<const QTextBrowser *>(object));
    }

    // a widget that got one of the Windows 11 adjustments (a style of its own that is not a copy of the application style)
    bool hasSquareStyle(const QWidget *widget)
    {
        return widget->testAttribute(Qt::WA_SetStyle) && !dynamic_cast<const AppStyleProxy *>(widget->style())
            && (qobject_cast<const QMenu *>(widget) || qobject_cast<const QMenuBar *>(widget) || isSquareControl(widget)
                || isOutlinedList(widget) || qobject_cast<const QHeaderView *>(widget) || isOutlinedFrame(widget));
    }

    // The Windows 11 style draws round menus, a spread-out menu bar, and fields and lists with only a line under or no
    // outline. The widgets get the square styles of the rest of the application while that style is the one in use.
    void applySquareStyle(QObject *watched)
    {
        // every outlined frame is the same plain 1 px frame, wherever it is
        if (auto *frame = qobject_cast<QFrame *>(watched); frame && isOutlinedFrame(frame))
        {
            frame->setFrameShape(QFrame::Panel);
            frame->setLineWidth(1);
            frame->setFrameShadow(QFrame::Plain);
        }

        // every scroll bar is the same, with every style
        if (auto *scrollBar = qobject_cast<QScrollBar *>(watched))
        {
            static auto *scrollBarStyle = new ScrollBarStyle(nullptr);
            if (scrollBar->style() != scrollBarStyle)
                scrollBar->setStyle(scrollBarStyle);
            return;
        }

        // every handle between panes is the same, with every style (a handle is drawn with the style of its splitter)
        if (auto *splitter = qobject_cast<QSplitter *>(watched))
        {
            static auto *splitterStyle = new SplitterStyle(nullptr);
            if (splitter->style() != splitterStyle)
                splitter->setStyle(splitterStyle);
            return;
        }

        if ((qApp->style()->name() == u"windows"_s) && qobject_cast<QComboBox *>(watched))
        {
            static auto *dropDownStyle = new ClassicDropDownStyle;
            auto *comboBox = static_cast<QWidget *>(watched);
            if (comboBox->style() != dropDownStyle)
                comboBox->setStyle(dropDownStyle);
            return;
        }

        // with another style the lists and their headers get the spacing of the lists of the application by a style of their own
        // (with the Windows 11 style it is part of the list style)
        if (qApp->style()->name() != u"windows11"_s)
        {
            if (auto *pushButton = qobject_cast<QPushButton *>(watched)
                ; pushButton && (qApp->style()->name() == u"fusion"_s) && (pushButton->style() == qApp->style()))
            {
                pushButton->setStyle(new DefaultButtonStyle(pushButton));
            }
            else if (auto *view = qobject_cast<QAbstractItemView *>(watched);
                view && (view->style() == qApp->style())
                && !(view->parentWidget() && view->parentWidget()->inherits("QComboBoxPrivateContainer")))
            {
                view->setStyle(new OutlinedListStyle(view));
            }
            // (a list with the spacing style of its own, see ListViewLayout, gets the outline too)
            else if (auto *spacedView = qobject_cast<QAbstractItemView *>(watched);
                spacedView && isOutlinedList(spacedView) && dynamic_cast<ItemViewSpacingStyle *>(spacedView->style())
                && !dynamic_cast<OutlinedListStyle *>(spacedView->style()))
            {
                spacedView->setStyle(new OutlinedListStyle(spacedView));
            }
            else if (auto *frame = qobject_cast<QFrame *>(watched); frame && isOutlinedFrame(frame) && (frame->style() == qApp->style()))
                frame->setStyle(new OutlinedFrameStyle(frame));
            return;
        }

        // the Windows 11 style draws menus and their selected item with rounded corners, the rest of the app is square
        if (auto *menu = qobject_cast<QMenu *>(watched))
        {
            static auto *squareStyle = new QProxyStyle(u"fusion"_s);
            if (menu->style() != squareStyle)
                menu->setStyle(squareStyle);
        }
        // the Windows 11 style also pads every menu bar entry so wide that File, Edit, View... look spread out
        else if (auto *menuBar = qobject_cast<QMenuBar *>(watched))
        {
            static auto *compactStyle = new QProxyStyle(u"fusion"_s);
            if (menuBar->style() != compactStyle)
                menuBar->setStyle(compactStyle);
        }
        else if (isSquareControl(watched))
        {
            static auto *controlStyle = new ControlStyle;
            auto *control = static_cast<QWidget *>(watched);
            if (control->style() != controlStyle)
                control->setStyle(controlStyle);
        }
        else if (isOutlinedFrame(watched))
        {
            static auto *listStyle = new ListStyle;
            auto *frame = static_cast<QWidget *>(watched);
            if (frame->style() != listStyle)
                frame->setStyle(listStyle);
        }
        // a list with a style of its own on top of the application style (the spacing of ListViewLayout) keeps it, over
        // the list style; the column headers share the spacing of the lists
        else if (auto *view = qobject_cast<QAbstractItemView *>(watched); view && (isOutlinedList(view) || qobject_cast<QHeaderView *>(view)))
        {
            static auto *listStyle = new ListStyle;
            if (view->style() == qApp->style())
                view->setStyle(listStyle);
            else if (auto *ownStyle = dynamic_cast<AppStyleProxy *>(view->style())
                ; ownStyle && !dynamic_cast<ListStyle *>(ownStyle->baseStyle()))
            {
                ownStyle->setBaseStyle(new ListStyle);
            }
        }
    }
#endif

    Path resolveThemePath(const Path &themePath)
    {
        return (themePath.isAbsolute() ? themePath : (Profile::instance()->location(SpecialFolder::Config) / themePath));
    }
}

UIThemeManager *UIThemeManager::m_instance = nullptr;

void UIThemeManager::freeInstance()
{
    delete m_instance;
    m_instance = nullptr;
}

void UIThemeManager::initInstance()
{
    if (!m_instance)
        m_instance = new UIThemeManager;
}

UIThemeManager::UIThemeManager()
    : m_useCustomTheme {Preferences::instance()->useCustomUITheme()}
    , m_systemStyleName {QApplication::style()->name()}
#ifdef QBT_HAS_COLORSCHEME_OPTION
    , m_colorSchemeSetting {u"Appearance/ColorScheme"_s}
#endif
    , m_trayIconStyleSetting {u"Appearance/TrayIconStyle"_s}
#if (defined(Q_OS_UNIX) && !defined(Q_OS_MACOS))
    , m_useSystemIcons {Preferences::instance()->useSystemIcons()}
#endif
{
#ifdef Q_OS_WIN
    updateSystemColorMode();
#endif

    applyStyle();

#ifdef QBT_HAS_COLORSCHEME_OPTION
    applyColorScheme();
#endif

    connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &UIThemeManager::onColorSchemeChanged);

#ifdef Q_OS_WIN
    // give every window a title bar that stands out from the window body
    qApp->installEventFilter(this);

    applyNeutralAccent();
#endif

    if (m_useCustomTheme)
    {
        const Path themePath = resolveThemePath(Preferences::instance()->customUIThemePath());

        if (themePath.hasExtension(u".qbtheme"_s))
        {
            if (QResource::registerResource(themePath.data(), u"/uitheme"_s))
                m_themeSource = std::make_unique<QRCThemeSource>();
            else
                LogMsg(tr("Failed to load UI theme from file: \"%1\"").arg(themePath.toString()), Log::WARNING);
        }
        else if (themePath.filename() == CONFIG_FILE_NAME)
        {
            m_themeSource = std::make_unique<FolderThemeSource>(themePath.parentPath());
        }
    }

    if (!m_themeSource)
        m_themeSource = std::make_unique<DefaultThemeSource>();

    if (m_useCustomTheme)
    {
        applyPalette();
        applyStyleSheet();
    }
}

UIThemeManager *UIThemeManager::instance()
{
    return m_instance;
}

#ifdef QBT_HAS_COLORSCHEME_OPTION
ColorScheme UIThemeManager::colorScheme() const
{
    return m_colorSchemeSetting.get(ColorScheme::System);
}

void UIThemeManager::setColorScheme(const ColorScheme value)
{
    // Always apply, even when the saved value is unchanged: the window may still show the
    // scheme from startup (for example when an older build saved the value without applying it)
    if (value != colorScheme())
        m_colorSchemeSetting = value;

    applyColorScheme();
}

void UIThemeManager::applyColorScheme() const
{
    switch (colorScheme())
    {
    case ColorScheme::System:
    default:
        qApp->styleHints()->unsetColorScheme();
        break;
    case ColorScheme::Light:
        qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
        break;
    case ColorScheme::Dark:
        qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        break;
    }
}
#endif

TrayIconStyle UIThemeManager::trayIconStyle() const
{
    return m_trayIconStyleSetting.get(TrayIconStyle::Normal);
}

void UIThemeManager::setTrayIconStyle(const TrayIconStyle value)
{
    if (value == trayIconStyle())
        return;

    m_trayIconStyleSetting = value;
}

void UIThemeManager::applyStyleSheet() const
{
    qApp->setStyleSheet(QString::fromUtf8(m_themeSource->readStyleSheet()));
}

QString UIThemeManager::styleName() const
{
    // "System" is the style the application started with
    const QString name = Preferences::instance()->getStyle();
    return ((name.compare(u"system", Qt::CaseInsensitive) == 0) ? m_systemStyleName : name);
}

bool UIThemeManager::applyStyle()
{
    const QString name = styleName();
    if (name.compare(QApplication::style()->name(), Qt::CaseInsensitive) == 0)
        return false;

    if (!QApplication::setStyle(name))
    {
        LogMsg(tr("Set app style failed. Unknown style: \"%1\"").arg(name), Log::WARNING);
        return false;
    }
    return true;
}

void UIThemeManager::updateStyle()
{
    if (styleName().compare(QApplication::style()->name(), Qt::CaseInsensitive) == 0)
        return;

    const QList<QWidget *> widgets = QApplication::allWidgets();

    // The widgets with a style of their own that is a copy of the application style are left behind by the change of the
    // application style, and a style that changed a widget is taken off by being replaced (and not by changing what it is
    // based on), so the widget is as it would be without it. They get the new style again below.
#ifdef Q_OS_WIN
    for (QWidget *widget : widgets)
    {
        if (hasSquareStyle(widget))
            widget->setStyle(nullptr);
    }
#endif
    QList<AppStyleProxy *> copies;
    QList<QWidget *> copyWidgets;
    for (QWidget *widget : widgets)
    {
        if (auto *copy = dynamic_cast<AppStyleProxy *>(widget->style()))
        {
            copy->unpolish(widget);
            copyWidgets.append(widget);
            if (!copies.contains(copy))
                copies.append(copy);
        }
    }

    const bool changed = applyStyle();

    // (a style shared by widgets is changed once)
    if (changed)
    {
        for (AppStyleProxy *copy : asConst(copies))
            copy->followApplicationStyle();
    }
    for (QWidget *widget : asConst(copyWidgets))
    {
        widget->style()->polish(widget);
        QEvent styleChange {QEvent::StyleChange};
        QApplication::sendEvent(widget, &styleChange);
        widget->updateGeometry();
        widget->update();
    }

#ifdef Q_OS_WIN
    for (QWidget *widget : widgets)
        applySquareStyle(widget);
#endif

    if (!changed)
        return;

#ifdef Q_OS_WIN
    applyNeutralAccent();
#endif
    emit themeChanged();
    // what was sized by the style of the application is sized again
    emit styleChanged();

#ifdef Q_OS_WIN
    const auto refreshTitleBars = [this]()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
            applyTitleBarColor(widget);
    };
    refreshTitleBars();
    QTimer::singleShot(0, this, refreshTitleBars);
#endif
}

void UIThemeManager::onColorSchemeChanged()
{
#ifdef Q_OS_WIN
    applyNeutralAccent();
#endif

    emit themeChanged();

    // workaround to refresh styled controls once color scheme is changed
    QApplication::setStyle(QApplication::style()->name());

#ifdef Q_OS_WIN
    // the palette of the windows may follow the new scheme a moment later, so color them again then
    const auto refreshTitleBars = [this]()
    {
        for (QWidget *widget : QApplication::topLevelWidgets())
            applyTitleBarColor(widget);
    };
    refreshTitleBars();
    QTimer::singleShot(0, this, refreshTitleBars);
    QTimer::singleShot(300, this, refreshTitleBars);
#endif
}

#ifdef Q_OS_WIN
bool UIThemeManager::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Show)
    {
        if (auto *widget = qobject_cast<QWidget *>(watched))
            applyTitleBarColor(widget);
    }
    else if (event->type() == QEvent::Polish)
    {
        applySquareStyle(watched);
    }
    // the palette follows the color scheme a moment after the change, and the accent follows the palette
    else if ((event->type() == QEvent::ApplicationPaletteChange) && (watched == qApp))
    {
        QTimer::singleShot(0, this, &UIThemeManager::updateNeutralAccent);
    }
    // a frame that asks for the outline of the lists after it was polished
    else if ((event->type() == QEvent::DynamicPropertyChange)
        && (static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "outlined"))
    {
        applySquareStyle(watched);
    }

    return QObject::eventFilter(watched, event);
}

void UIThemeManager::updateNeutralAccent()
{
    // (applying it changes the palette again: only when it doesn't fit the scheme yet)
    if (!m_useCustomTheme && ((isDarkTheme() != m_neutralAccentApplied)
        || (m_neutralAccentApplied && (qApp->palette().color(QPalette::Highlight) != QColor(0x00, 0x78, 0xd4)))))
    {
        applyNeutralAccent();
    }
}

void UIThemeManager::applyNeutralAccent()
{
    // A custom theme sets the whole palette itself
    if (m_useCustomTheme)
        return;

    if (isDarkTheme())
    {
        // The Windows accent color (a strong red, for example) would fill every check box, color the
        // selection and tint every other row of a list. The dark scheme uses Windows blue and a faint
        // stripe instead. Only these roles are overridden, the others keep following the system.
        const QPalette systemPalette = qApp->palette();
        const QColor accent {0x00, 0x78, 0xd4};
        QPalette palette;
        palette.setColor(QPalette::Highlight, accent);
        palette.setColor(QPalette::Accent, accent);
        palette.setColor(QPalette::AlternateBase
            , mixColors(systemPalette.color(QPalette::Base), systemPalette.color(QPalette::Text), 6));
        qApp->setPalette(palette);
        m_neutralAccentApplied = true;
    }
    else if (m_neutralAccentApplied)
    {
        // back to the system colors
        qApp->setPalette(QPalette());
        m_neutralAccentApplied = false;
    }
}

void UIThemeManager::applyTitleBarColor(QWidget *window) const
{
    if (!window->isWindow() || window->testAttribute(Qt::WA_DontShowOnScreen))
        return;

    // only windows with a regular title bar, not menus, tooltips, splash screens or frameless windows
    switch (window->windowType())
    {
    case Qt::Window:
    case Qt::Dialog:
    case Qt::Tool:
        break;
    default:
        return;
    }
    if (window->windowFlags().testFlag(Qt::FramelessWindowHint))
        return;

    // a shade away from the window color: lighter on a dark window, darker on a light one
    const QPalette palette = window->palette();
    const QColor captionColor = mixColors(palette.color(QPalette::Window), palette.color(QPalette::WindowText), 10);
    const QColor textColor = palette.color(QPalette::WindowText);

    // unsupported before Windows 11, then the title bar just keeps the system color
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    // the title buttons and frame follow the scheme, not only the caption color
    const BOOL darkMode = (palette.color(QPalette::Window).lightness() < 127);
    ::DwmSetWindowAttribute(hwnd, DWM_DARK_MODE, &darkMode, sizeof(darkMode));
    const COLORREF caption = toColorRef(captionColor);
    const COLORREF text = toColorRef(textColor);
    ::DwmSetWindowAttribute(hwnd, DWM_CAPTION_COLOR, &caption, sizeof(caption));
    ::DwmSetWindowAttribute(hwnd, DWM_TEXT_COLOR, &text, sizeof(text));
}
#endif

QIcon UIThemeManager::getIcon(const QString &iconId, [[maybe_unused]] const QString &fallback, const ColorMode colorMode) const
{
    auto &icons = (colorMode == ColorMode::Dark) ? m_darkModeIcons : m_icons;

    const auto iter = icons.find(iconId);
    if (iter != icons.end())
        return *iter;

#if (defined(Q_OS_UNIX) && !defined(Q_OS_MACOS))
    // Don't cache system icons because users might change them at run time
    if (m_useSystemIcons)
    {
        auto icon = QIcon::fromTheme(iconId);
        if (icon.isNull() || icon.availableSizes().isEmpty())
            icon = QIcon::fromTheme(fallback, QIcon(m_themeSource->getIconPath(iconId, colorMode).data()));
        return icon;
    }
#endif

    const QIcon icon {m_themeSource->getIconPath(iconId, colorMode).data()};
    icons[iconId] = icon;
    return icon;
}

QIcon UIThemeManager::getIcon(const QString &iconId, const QString &fallback) const
{
    const auto colorMode = isDarkTheme() ? ColorMode::Dark : ColorMode::Light;
    return getIcon(iconId, fallback, colorMode);
}

QIcon UIThemeManager::getSystrayIcon() const
{
#ifdef Q_OS_WIN
    const auto colorMode = m_systemColorMode;
#else
    const auto colorMode = (qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark)
            ? ColorMode::Dark : ColorMode::Light;
#endif
    const QString fallback = (colorMode == ColorMode::Light)
            ? u"qbittorrent-tray-light"_s : u"qbittorrent-tray-dark"_s;

    switch (trayIconStyle())
    {
    case TrayIconStyle::Normal:
        return getIcon(u"qbittorrent-tray"_s, {}, colorMode);

    case TrayIconStyle::Monochrome:
        return getIcon(u"qbittorrent-tray-mono"_s, fallback, colorMode);
    }

    Q_UNREACHABLE_RETURN({});
}

QIcon UIThemeManager::getFlagIcon(const QString &countryIsoCode) const
{
    if (countryIsoCode.isEmpty())
        return {};

    const QString key = countryIsoCode.toLower();
    const auto iter = m_flags.constFind(key);
    if (iter != m_flags.cend())
        return *iter;

    const QIcon icon {u":/icons/flags/" + key + u".svg"};
    m_flags[key] = icon;
    return icon;
}

QPixmap UIThemeManager::getScaledPixmap(const QString &iconId, const int height) const
{
    // (workaround) svg images require the use of `QIcon()` to load and scale losslessly,
    // otherwise other image classes will convert it to pixmap first and follow-up scaling will become lossy.

    Q_ASSERT(height > 0);

    const QString cacheKey = iconId + u'@' + QString::number(height);

    QPixmap pixmap;
    if (!QPixmapCache::find(cacheKey, &pixmap))
    {
        pixmap = getIcon(iconId).pixmap(height);
        QPixmapCache::insert(cacheKey, pixmap);
    }

    return pixmap;
}

QColor UIThemeManager::getColor(const QString &id) const
{
    const QColor color = m_themeSource->getColor(id, (isDarkTheme() ? ColorMode::Dark : ColorMode::Light));
    return color;
}

#ifdef Q_OS_WIN
void UIThemeManager::updateSystemColorMode()
{
    const HKEY hkRoot = HKEY_CURRENT_USER;
    const REGSAM samDesired = KEY_READ;

    HKEY hkPersonalize = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(hkRoot, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, samDesired, &hkPersonalize);
    if (status != ERROR_SUCCESS)
        return;

    [[maybe_unused]] const auto hkPersonalizeGuard = qScopeGuard([&hkPersonalize] { ::RegCloseKey(hkPersonalize); });

    const ulong val = Utils::Reg::getULongValue(hkPersonalize, u"SystemUsesLightTheme"_s);
    const auto systemUsesLightTheme = static_cast<bool>(val);
    const auto systemColorMode = systemUsesLightTheme ? ColorMode::Light : ColorMode::Dark;
    if (systemColorMode != m_systemColorMode)
    {
        m_systemColorMode = systemColorMode;
        onColorSchemeChanged();
    }
}
#endif

void UIThemeManager::applyPalette() const
{
    struct ColorDescriptor
    {
        QString id;
        QPalette::ColorRole colorRole;
        QPalette::ColorGroup colorGroup;
    };

    const ColorDescriptor paletteColorDescriptors[] =
    {
        {u"Palette.Window"_s, QPalette::Window, QPalette::Normal},
        {u"Palette.WindowText"_s, QPalette::WindowText, QPalette::Normal},
        {u"Palette.Base"_s, QPalette::Base, QPalette::Normal},
        {u"Palette.AlternateBase"_s, QPalette::AlternateBase, QPalette::Normal},
        {u"Palette.Text"_s, QPalette::Text, QPalette::Normal},
        {u"Palette.ToolTipBase"_s, QPalette::ToolTipBase, QPalette::All},
        {u"Palette.ToolTipText"_s, QPalette::ToolTipText, QPalette::All},
        {u"Palette.BrightText"_s, QPalette::BrightText, QPalette::Normal},
        {u"Palette.Highlight"_s, QPalette::Highlight, QPalette::Normal},
        {u"Palette.HighlightedText"_s, QPalette::HighlightedText, QPalette::Normal},
        {u"Palette.Button"_s, QPalette::Button, QPalette::Normal},
        {u"Palette.ButtonText"_s, QPalette::ButtonText, QPalette::Normal},
        {u"Palette.Link"_s, QPalette::Link, QPalette::Normal},
        {u"Palette.LinkVisited"_s, QPalette::LinkVisited, QPalette::Normal},
        {u"Palette.Light"_s, QPalette::Light, QPalette::Normal},
        {u"Palette.Midlight"_s, QPalette::Midlight, QPalette::Normal},
        {u"Palette.Mid"_s, QPalette::Mid, QPalette::Normal},
        {u"Palette.Dark"_s, QPalette::Dark, QPalette::Normal},
        {u"Palette.Shadow"_s, QPalette::Shadow, QPalette::Normal},
        {u"Palette.WindowTextDisabled"_s, QPalette::WindowText, QPalette::Disabled},
        {u"Palette.TextDisabled"_s, QPalette::Text, QPalette::Disabled},
        {u"Palette.ToolTipTextDisabled"_s, QPalette::ToolTipText, QPalette::Disabled},
        {u"Palette.BrightTextDisabled"_s, QPalette::BrightText, QPalette::Disabled},
        {u"Palette.HighlightedTextDisabled"_s, QPalette::HighlightedText, QPalette::Disabled},
        {u"Palette.ButtonTextDisabled"_s, QPalette::ButtonText, QPalette::Disabled}
    };

    QPalette palette = qApp->palette();
    for (const ColorDescriptor &colorDescriptor : paletteColorDescriptors)
    {
        // For backward compatibility, the palette color overrides are read from the section of the "light mode" colors
        const QColor newColor = m_themeSource->getColor(colorDescriptor.id, ColorMode::Light);
        if (newColor.isValid())
            palette.setColor(colorDescriptor.colorGroup, colorDescriptor.colorRole, newColor);
    }

    qApp->setPalette(palette);
}
