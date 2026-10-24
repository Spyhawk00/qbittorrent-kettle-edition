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

#pragma once

#include "appstyleproxy.h"

// Draws group boxes as plain section headings: a bold title (with its check box when checkable) and a thin line
// under it, instead of a frame. A nested group box is drawn as a sub-heading, or a plain check box when
// checkable, with its contents indented. Set the dynamic property "heading" on it to draw a section heading.
// Everything else is left to the application's style, so check boxes, text and colors stay native.
class GroupBoxHeadingStyle final : public AppStyleProxy
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(GroupBoxHeadingStyle)

public:
    explicit GroupBoxHeadingStyle(QObject *parent = nullptr);

    // applies the style to every group box under the widget
    static void apply(QWidget *widget);
    // makes the controls of "label: control" rows under the widget start at the same column,
    // and keeps number boxes and buttons on a row of their own at their natural width
    static void alignRows(QWidget *widget);

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override;
};
