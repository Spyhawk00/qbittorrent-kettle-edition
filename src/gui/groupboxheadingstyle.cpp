/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Spyhawk
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "groupboxheadingstyle.h"

#include <algorithm>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleOption>

namespace
{
    // space between a title and the line under it, and around the contents
    const int HEADING_GAP = 4;

    bool isNested(const QWidget *widget)
    {
        for (const QWidget *parent = (widget ? widget->parentWidget() : nullptr); parent; parent = parent->parentWidget())
        {
            if (qobject_cast<const QGroupBox *>(parent))
                return true;
        }
        return false;
    }

    bool isHeading(const QWidget *widget)
    {
        if (!widget)
            return true;
        if (const QVariant heading = widget->property("heading"); heading.isValid())
            return heading.toBool();
        return !isNested(widget);
    }

    bool isCheckable(const QStyleOptionGroupBox *box)
    {
        return box->subControls.testFlag(QStyle::SC_GroupBoxCheckBox);
    }

    // the text that starts a "label: control" row
    bool isRowLabel(const QWidget *widget)
    {
        if (const auto *label = qobject_cast<const QLabel *>(widget))
            return !label->text().isEmpty();
        return qobject_cast<const QCheckBox *>(widget) || qobject_cast<const QRadioButton *>(widget);
    }

    bool isControl(QLayoutItem *item)
    {
        if (!item)
            return false;
        if (item->layout())
            return true;
        const QWidget *widget = item->widget();
        return qobject_cast<const QAbstractSpinBox *>(widget) || qobject_cast<const QComboBox *>(widget)
            || qobject_cast<const QLineEdit *>(widget) || (widget && widget->inherits("FileSystemPathEdit"));
    }

    QList<QWidget *> rowLabels(QLayout *layout)
    {
        QList<QWidget *> labels;
        if (auto *grid = qobject_cast<QGridLayout *>(layout))
        {
            for (int row = 0; row < grid->rowCount(); ++row)
            {
                QLayoutItem *first = grid->itemAtPosition(row, 0);
                if (!first || !isRowLabel(first->widget()) || !isControl(grid->itemAtPosition(row, 1)))
                    continue;
                int itemRow = 0, column = 0, rowSpan = 0, columnSpan = 0;
                grid->getItemPosition(grid->indexOf(first), &itemRow, &column, &rowSpan, &columnSpan);
                if (columnSpan == 1)
                    labels.append(first->widget());
            }
        }
        else if (auto *form = qobject_cast<QFormLayout *>(layout))
        {
            for (int row = 0; row < form->rowCount(); ++row)
            {
                QLayoutItem *label = form->itemAt(row, QFormLayout::LabelRole);
                if (label && isRowLabel(label->widget()) && form->itemAt(row, QFormLayout::FieldRole))
                    labels.append(label->widget());
            }
        }
        else if (auto *box = qobject_cast<QHBoxLayout *>(layout); box && (box->count() >= 2))
        {
            if (isRowLabel(box->itemAt(0)->widget()) && isControl(box->itemAt(1)))
                labels.append(box->itemAt(0)->widget());
        }
        return labels;
    }

    QFont titleFont(const QStyleOptionGroupBox *box, const QWidget *widget)
    {
        QFont font = widget ? widget->font() : QApplication::font();
        // a check box inside a section reads as an ordinary option
        font.setBold(isHeading(widget) || !isCheckable(box));
        return font;
    }
}

GroupBoxHeadingStyle::GroupBoxHeadingStyle(QObject *parent)
    : QProxyStyle(QApplication::style()->name())
{
    setParent(parent);
}

void GroupBoxHeadingStyle::apply(QWidget *widget)
{
    auto *style = new GroupBoxHeadingStyle(widget);
    for (QGroupBox *groupBox : widget->findChildren<QGroupBox *>())
        groupBox->setStyle(style);
}

void GroupBoxHeadingStyle::alignRows(QWidget *widget)
{
    // measure with the fonts the widgets are shown with, not the ones they had before being polished
    widget->ensurePolished();

    // contents of a nested group box are indented by one check box width
    const int indentStep = widget->style()->pixelMetric(PM_IndicatorWidth, nullptr, widget)
        + widget->style()->pixelMetric(PM_CheckBoxLabelSpacing, nullptr, widget);
    const auto indentOf = [widget, indentStep](const QWidget *label)
    {
        int indent = 0;
        for (const QWidget *parent = label->parentWidget(); parent && (parent != widget); parent = parent->parentWidget())
        {
            if (qobject_cast<const QGroupBox *>(parent) && !isHeading(parent))
                indent += indentStep;
        }
        return indent;
    };

    QList<QWidget *> labels;
    for (QLayout *layout : widget->findChildren<QLayout *>())
    {
        for (QWidget *label : rowLabels(layout))
        {
            if (label->isVisibleTo(widget))
                labels.append(label);
        }
    }

    int controlColumn = 0;
    for (const QWidget *label : labels)
        controlColumn = std::max(controlColumn, (indentOf(label) + label->sizeHint().width()));
    for (QWidget *label : labels)
        label->setMinimumWidth(controlColumn - indentOf(label));

    // Some styles (classic Windows) size these boxes to their text with no room to spare, which clips the text
    // at some display scales, so they get a little more
    const int textRoom = 2 * widget->fontMetrics().averageCharWidth();
    for (QAbstractSpinBox *spinBox : widget->findChildren<QAbstractSpinBox *>())
    {
        spinBox->setSizePolicy(QSizePolicy::Fixed, spinBox->sizePolicy().verticalPolicy());
        spinBox->setMinimumWidth(spinBox->sizeHint().width() + textRoom);
    }
    for (QComboBox *comboBox : widget->findChildren<QComboBox *>())
    {
        // a size measured before the box is first shown can predate its items
        comboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        comboBox->setMinimumWidth(comboBox->sizeHint().width() + textRoom);
    }

    // a button on a row of its own keeps its own width, at the left
    for (QVBoxLayout *layout : widget->findChildren<QVBoxLayout *>())
    {
        for (int i = 0; i < layout->count(); ++i)
        {
            if (auto *button = qobject_cast<QPushButton *>(layout->itemAt(i)->widget()))
                layout->setAlignment(button, Qt::AlignLeft);
        }
    }
}

void GroupBoxHeadingStyle::drawComplexControl(const ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    const auto *box = qstyleoption_cast<const QStyleOptionGroupBox *>(option);
    if ((control != CC_GroupBox) || !box)
    {
        QProxyStyle::drawComplexControl(control, option, painter, widget);
        return;
    }

    const QRect checkBoxRect = subControlRect(control, box, SC_GroupBoxCheckBox, widget);
    const QRect labelRect = subControlRect(control, box, SC_GroupBoxLabel, widget);

    if (isCheckable(box))
    {
        QStyleOptionButton checkBox;
        checkBox.QStyleOption::operator=(*box);
        checkBox.rect = checkBoxRect;
        drawPrimitive(PE_IndicatorCheckBox, &checkBox, painter, widget);
    }

    if (!box->text.isEmpty())
    {
        painter->save();
        painter->setFont(titleFont(box, widget));
        drawItemText(painter, labelRect, (Qt::AlignLeft | Qt::AlignVCenter | Qt::TextShowMnemonic)
            , box->palette, box->state.testFlag(State_Enabled), box->text, QPalette::WindowText);
        painter->restore();
    }

    if (box->state.testFlag(State_HasFocus))
    {
        QStyleOptionFocusRect focusRect;
        focusRect.QStyleOption::operator=(*box);
        focusRect.rect = labelRect.united(checkBoxRect).adjusted(-2, 0, 2, 0);
        drawPrimitive(PE_FrameFocusRect, &focusRect, painter, widget);
    }

    if (isHeading(widget))
    {
        QColor lineColor = box->palette.color(QPalette::WindowText);
        lineColor.setAlphaF(0.2);
        painter->fillRect(QRect(box->rect.left(), (labelRect.bottom() + 1 + HEADING_GAP), box->rect.width(), 1), lineColor);
    }
}

QRect GroupBoxHeadingStyle::subControlRect(const ComplexControl control, const QStyleOptionComplex *option
    , const SubControl subControl, const QWidget *widget) const
{
    const auto *box = qstyleoption_cast<const QStyleOptionGroupBox *>(option);
    if ((control != CC_GroupBox) || !box)
        return QProxyStyle::subControlRect(control, option, subControl, widget);

    const bool checkable = isCheckable(box);
    const bool heading = isHeading(widget);
    const QFontMetrics metrics {titleFont(box, widget)};
    const int indicatorWidth = pixelMetric(PM_IndicatorWidth, option, widget);
    const int indicatorHeight = pixelMetric(PM_IndicatorHeight, option, widget);
    const int labelSpacing = pixelMetric(PM_CheckBoxLabelSpacing, option, widget);
    const int titleHeight = (box->text.isEmpty() && !checkable) ? 0 : std::max(metrics.height(), (checkable ? indicatorHeight : 0));

    // a section inside another box gets the space a section on the page has, and a sub-heading a little
    int spaceAbove = 0;
    if (isNested(widget))
        spaceAbove = heading ? metrics.height() : (checkable ? 0 : HEADING_GAP);
    const QRect rect = box->rect.adjusted(0, spaceAbove, 0, 0);

    switch (subControl)
    {
    case SC_GroupBoxCheckBox:
        if (!checkable)
            return {};
        return {rect.left(), (rect.top() + ((titleHeight - indicatorHeight) / 2)), indicatorWidth, indicatorHeight};
    case SC_GroupBoxLabel:
        {
            const int left = rect.left() + (checkable ? (indicatorWidth + labelSpacing) : 0);
            const int width = std::min((metrics.size(Qt::TextShowMnemonic, box->text).width() + 2), (rect.right() - left + 1));
            return {left, rect.top(), width, titleHeight};
        }
    case SC_GroupBoxContents:
        {
            // sections keep their contents at the left edge, under the line; nested boxes indent theirs
            const int left = rect.left() + (heading ? 0 : (indicatorWidth + labelSpacing));
            const int top = rect.top() + titleHeight + (heading ? ((3 * HEADING_GAP) + 1) : HEADING_GAP);
            return {left, top, (rect.right() - left + 1), (rect.bottom() - top + 1)};
        }
    case SC_GroupBoxFrame:
        return box->rect;
    default:
        return QProxyStyle::subControlRect(control, option, subControl, widget);
    }
}

int GroupBoxHeadingStyle::pixelMetric(const PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    // the contents rectangle already places the contents, so the layout inside adds no margins of its own
    if (qobject_cast<const QGroupBox *>(widget))
    {
        switch (metric)
        {
        case PM_LayoutLeftMargin:
        case PM_LayoutTopMargin:
        case PM_LayoutRightMargin:
        case PM_LayoutBottomMargin:
            return 0;
        default:
            break;
        }
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}
