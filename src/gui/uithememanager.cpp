/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2023-2024  Vladimir Golovnev <glassez@yandex.ru>
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
#include <QPalette>
#include <QPixmapCache>
#include <QResource>
#include <QStyle>
#include <QStyleHints>

#include "base/global.h"
#include "base/logger.h"
#include "base/path.h"
#include "base/profile.h"
#include "base/preferences.h"
#include "uithemecommon.h"

#ifdef Q_OS_WIN
#include <QEvent>
#include <QWidget>

#include <windows.h>
#include <dwmapi.h>
#endif

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
#ifdef QBT_HAS_COLORSCHEME_OPTION
    , m_colorSchemeSetting {u"Appearance/ColorScheme"_s}
#endif
#if (defined(Q_OS_UNIX) && !defined(Q_OS_MACOS))
    , m_useSystemIcons {Preferences::instance()->useSystemIcons()}
#endif
{
    if (const QString styleName = Preferences::instance()->getStyle(); styleName.compare(u"system", Qt::CaseInsensitive) != 0)
    {
        if (!QApplication::setStyle(styleName))
            LogMsg(tr("Set app style failed. Unknown style: \"%1\"").arg(styleName), Log::WARNING);
    }

#ifdef QBT_HAS_COLORSCHEME_OPTION
    applyColorScheme();
#endif

    // NOTE: Qt::QueuedConnection can be omitted as soon as support for Qt 6.5 is dropped
    connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &UIThemeManager::onColorSchemeChanged, Qt::QueuedConnection);

#ifdef Q_OS_WIN
    // give every window a title bar that stands out from the window body
    qApp->installEventFilter(this);
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
    if (value == colorScheme())
        return;

    m_colorSchemeSetting = value;
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

void UIThemeManager::applyStyleSheet() const
{
    qApp->setStyleSheet(QString::fromUtf8(m_themeSource->readStyleSheet()));
}

void UIThemeManager::onColorSchemeChanged()
{
    emit themeChanged();

    // workaround to refresh styled controls once color scheme is changed
    QApplication::setStyle(QApplication::style()->name());

#ifdef Q_OS_WIN
    for (QWidget *widget : QApplication::topLevelWidgets())
        applyTitleBarColor(widget);
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

    return QObject::eventFilter(watched, event);
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
    const COLORREF caption = toColorRef(captionColor);
    const COLORREF text = toColorRef(textColor);
    ::DwmSetWindowAttribute(hwnd, DWM_CAPTION_COLOR, &caption, sizeof(caption));
    ::DwmSetWindowAttribute(hwnd, DWM_TEXT_COLOR, &text, sizeof(text));
}
#endif

QIcon UIThemeManager::getIcon(const QString &iconId, [[maybe_unused]] const QString &fallback) const
{
    const auto colorMode = isDarkTheme() ? ColorMode::Dark : ColorMode::Light;
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
        {u"Palette.ToolTipBase"_s, QPalette::ToolTipBase, QPalette::Normal},
        {u"Palette.ToolTipText"_s, QPalette::ToolTipText, QPalette::Normal},
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
