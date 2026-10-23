/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
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

#include "hidabletabwidget.h"

#include <QApplication>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleOption>
#include <QTabBar>

namespace
{
    // The style of the application, with a thin outline around every tab
    class RoomyTabStyle final : public QProxyStyle
    {
    public:
        explicit RoomyTabStyle(QObject *parent)
            : QProxyStyle(QApplication::style()->name())
        {
            setParent(parent);
        }

        // the page as the style paints it when empty
        QImage paintedPage(const QPalette &palette, const QTabBar::Shape shape, const QWidget *widget) const
        {
            QImage image {64, 64, QImage::Format_ARGB32};
            image.fill(Qt::transparent);

            QStyleOptionTabWidgetFrame frame;
            frame.initFrom(widget);
            frame.palette = palette;
            frame.rect = image.rect();
            frame.shape = shape;
            frame.lineWidth = pixelMetric(PM_DefaultFrameWidth, &frame, widget);
            frame.state |= State_Enabled;

            QPainter painter {&image};
            QProxyStyle::drawPrimitive(PE_FrameTabWidget, &frame, &painter, widget);
            painter.end();
            return image;
        }

        // the color the style paints the page with
        QColor pageFillColor(const QPalette &palette, const QTabBar::Shape shape, const QWidget *widget) const
        {
            const QColor color = paintedPage(palette, shape, widget).pixelColor(32, 32);
            return ((color.alpha() == 255) ? color : palette.color(QPalette::Window));
        }

        // the line around the page and the tabs, which are drawn square (the style rounds the page and barely shows its line)
        static QColor lineColor(const QColor &pageColor, const QPalette &palette)
        {
            const QColor textColor = palette.color(QPalette::WindowText);
            return {((pageColor.red() * 78 + textColor.red() * 22) / 100), ((pageColor.green() * 78 + textColor.green() * 22) / 100), ((pageColor.blue() * 78 + textColor.blue() * 22) / 100)};
        }

        void drawControl(const ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override
        {
            const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
            if ((element != CE_TabBarTabShape) || !tab)
            {
                QProxyStyle::drawControl(element, option, painter, widget);
                return;
            }

            // the selected tab has the color of the page and covers its border line, the others are a little darker
            QRect fillRect = tab->rect;
            if (tab->state.testFlag(State_Selected))
            {
                switch (tab->shape)
                {
                case QTabBar::RoundedNorth:
                case QTabBar::TriangularNorth:
                    fillRect.adjust(0, 0, 0, 1);
                    break;
                case QTabBar::RoundedSouth:
                case QTabBar::TriangularSouth:
                    fillRect.adjust(0, -1, 0, 0);
                    break;
                case QTabBar::RoundedWest:
                case QTabBar::TriangularWest:
                    fillRect.adjust(0, 0, 1, 0);
                    break;
                case QTabBar::RoundedEast:
                case QTabBar::TriangularEast:
                    fillRect.adjust(-1, 0, 0, 0);
                    break;
                }
            }
            const QColor pageColor = pageFillColor(tab->palette, tab->shape, widget);
            const QColor textColor = tab->palette.color(QPalette::WindowText);
            const QColor dimColor {((pageColor.red() * 9 + textColor.red()) / 10), ((pageColor.green() * 9 + textColor.green()) / 10), ((pageColor.blue() * 9 + textColor.blue()) / 10)};
            painter->fillRect(fillRect, (tab->state.testFlag(State_Selected) ? pageColor : dimColor));

            // the side facing the page is left open, so the selected tab joins it
            QRect rect = tab->rect.adjusted(0, 0, -1, -1);
            bool north = false, south = false, west = false, east = false;
            switch (tab->shape)
            {
            case QTabBar::RoundedNorth:
            case QTabBar::TriangularNorth:
                north = true;
                break;
            case QTabBar::RoundedSouth:
            case QTabBar::TriangularSouth:
                south = true;
                break;
            case QTabBar::RoundedWest:
            case QTabBar::TriangularWest:
                west = true;
                break;
            case QTabBar::RoundedEast:
            case QTabBar::TriangularEast:
                east = true;
                break;
            }

            const QColor lineColor = RoomyTabStyle::lineColor(pageColor, tab->palette);
            painter->save();
            painter->setPen(lineColor);
            if (!south)
                painter->drawLine(rect.topLeft(), rect.topRight());
            if (!north)
                painter->drawLine(rect.bottomLeft(), rect.bottomRight());
            if (!west)
                painter->drawLine(rect.topRight(), rect.bottomRight());
            if (!east)
                painter->drawLine(rect.topLeft(), rect.bottomLeft());
            painter->restore();
        }
    };
}

HidableTabWidget::HidableTabWidget(QWidget *parent)
    : QTabWidget(parent)
{
    // Make the tabs easier to see with a thin outline
    tabBar()->setStyle(new RoomyTabStyle(tabBar()));

    // Skip single tab in keyboard navigation (no point navigating to it)
    tabBar()->setFocusPolicy(Qt::NoFocus);
}

void HidableTabWidget::tabInserted(const int index)
{
    QTabWidget::tabInserted(index);
    tabsCountChanged();
}

void HidableTabWidget::tabRemoved(const int index)
{
    QTabWidget::tabRemoved(index);
    tabsCountChanged();
}

void HidableTabWidget::tabsCountChanged()
{
    const qsizetype tabsCount = count();
    tabBar()->setVisible(tabsCount != 1);
    // Skip single tab in keyboard navigation (no point navigating to it)
    tabBar()->setFocusPolicy((tabsCount > 1) ? Qt::StrongFocus : Qt::NoFocus);
}

void HidableTabWidget::paintEvent(QPaintEvent *event)
{
#ifdef Q_OS_MACOS
    // Hide the pane for macintosh style
    if (!style()->inherits("QMacStyle"))
        QTabWidget::paintEvent(event);
#else
    // The page gets a square frame in the line color of the tabs, so the selected tab joins it
    const auto *tabStyle = dynamic_cast<const RoomyTabStyle *>(tabBar()->style());
    if (!tabStyle || !tabBar()->isVisible() || (tabBar()->shape() != QTabBar::RoundedNorth))
    {
        QTabWidget::paintEvent(event);
        return;
    }

    const int top = tabBar()->geometry().bottom() + 1;
    const QRect pageRect {0, top, width(), (height() - top)};
    const QColor pageColor = tabStyle->pageFillColor(palette(), tabBar()->shape(), this);

    QPainter painter {this};
    painter.fillRect(pageRect, pageColor);
    painter.setPen(RoomyTabStyle::lineColor(pageColor, palette()));
    painter.drawRect(pageRect.adjusted(0, 0, -1, -1));
#endif
}
