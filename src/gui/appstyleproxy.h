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

#pragma once

#include <algorithm>

#include <QAbstractItemView>
#include <QApplication>
#include <QHeaderView>
#include <QProxyStyle>
#include <QStyleOption>
#include <QStyle>
#include <QStyleFactory>

// The color of every line of the application that outlines a field, a list or a section: the one Fusion draws in a
// light scheme; in a dark one Fusion's is about as dark as the window, so it is drawn lighter there, as light as the drop-down lists
namespace OutlineColor
{
    inline QColor get(const QPalette &palette)
    {
        const bool dark = (QApplication::palette().color(QPalette::Active, QPalette::Base).lightness() < 127);
        return dark ? palette.color(QPalette::Button).lighter(130) : QApplication::palette().color(QPalette::Window).darker(140);
    }
}

// The spacing of every list of the application: rows as tall as the text plus 6 px, column headers the text plus 8 px,
// and 8 px between the edge of a cell and its content (the style adds 1 px itself)
namespace ItemViewSpacing
{
    inline int pixelMetric(const QStyle::PixelMetric metric, const QWidget *widget, const int styleValue)
    {
        if ((metric == QStyle::PM_FocusFrameHMargin) && qobject_cast<const QAbstractItemView *>(widget))
            return 7;
        return styleValue;
    }

    // the height of a column header, and of the tabs of the window
    inline int headerHeight(const QWidget *widget)
    {
        return widget->fontMetrics().height() + 8;
    }

    // the text of a column header, 4 px further from the sides than the style puts it, so it is as far from them as the text of a cell
    inline QStyleOptionHeader paddedHeaderLabel(const QStyleOptionHeader &option)
    {
        QStyleOptionHeader padded {option};
        padded.rect.adjust(4, 0, -4, 0);
        return padded;
    }

    inline QSize sizeFromContents(const QStyle::ContentsType type, const QSize &styleSize, const QWidget *widget)
    {
        QSize size = styleSize;
        if (!widget)
            return size;
        if (type == QStyle::CT_ItemViewItem)
            size.setHeight(std::max(size.height(), (widget->fontMetrics().height() + 6)));
        else if (type == QStyle::CT_HeaderSection)
        {
            // the text of a header is 4 px further from the sides, see paddedHeaderLabel()
            size.setHeight(headerHeight(widget));
            size.rwidth() += 8;
        }
        return size;
    }
}

// The application's style, as the base of a style that changes a few things. A widget with such a style doesn't
// follow the application when its style changes: UIThemeManager calls followApplicationStyle() then.
class AppStyleProxy : public QProxyStyle
{
public:
    explicit AppStyleProxy(QObject *parent)
        : QProxyStyle(QApplication::style()->name())
    {
        setParent(parent);
    }

    void followApplicationStyle()
    {
        setBaseStyle(QStyleFactory::create(QApplication::style()->name()));
    }
};

// The style of the application, with the spacing of every list (see ItemViewSpacing)
class ItemViewSpacingStyle : public AppStyleProxy
{
public:
    using AppStyleProxy::AppStyleProxy;

    QSize sizeFromContents(const ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override
    {
        return ItemViewSpacing::sizeFromContents(type, QProxyStyle::sizeFromContents(type, option, size, widget), widget);
    }

    int pixelMetric(const PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
        return ItemViewSpacing::pixelMetric(metric, widget, QProxyStyle::pixelMetric(metric, option, widget));
    }

    void drawControl(const ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
    {
        if (const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(option); header && (element == CE_HeaderLabel))
        {
            const QStyleOptionHeader padded = ItemViewSpacing::paddedHeaderLabel(*header);
            QProxyStyle::drawControl(element, &padded, painter, widget);
        }
        else
        {
            QProxyStyle::drawControl(element, option, painter, widget);
        }
    }
};
