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
#include <QPalette>
#include <QPixmapCache>
#include <QResource>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>

#include "base/logger.h"
#include "base/path.h"
#include "base/profile.h"
#include "base/preferences.h"
#include "uithemecommon.h"

#ifdef Q_OS_WIN
#include <QEvent>
#include <QMenu>
#include <QMenuBar>
#include <QProxyStyle>
#include <QPushButton>
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

    // square push buttons, a little larger than the Windows 11 style makes them
    class ButtonStyle final : public QProxyStyle
    {
    public:
        ButtonStyle()
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
    };
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
    , m_trayIconStyleSetting {u"Appearance/TrayIconStyle"_s}
#if (defined(Q_OS_UNIX) && !defined(Q_OS_MACOS))
    , m_useSystemIcons {Preferences::instance()->useSystemIcons()}
#endif
{
#ifdef Q_OS_WIN
    updateSystemColorMode();
#endif

    if (const QString styleName = Preferences::instance()->getStyle(); styleName.compare(u"system", Qt::CaseInsensitive) != 0)
    {
        if (!QApplication::setStyle(styleName))
            LogMsg(tr("Set app style failed. Unknown style: \"%1\"").arg(styleName), Log::WARNING);
    }

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
        // the Windows 11 style draws menus and their selected item with rounded corners, the rest of the app is square
        if (auto *menu = qobject_cast<QMenu *>(watched))
        {
            static auto *squareStyle = new QProxyStyle(u"fusion"_s);
            if ((qApp->style()->name() == u"windows11"_s) && (menu->style() != squareStyle))
                menu->setStyle(squareStyle);
        }
        // the Windows 11 style also pads every menu bar entry so wide that File, Edit, View... look spread out
        else if (auto *menuBar = qobject_cast<QMenuBar *>(watched))
        {
            static auto *compactStyle = new QProxyStyle(u"fusion"_s);
            if ((qApp->style()->name() == u"windows11"_s) && (menuBar->style() != compactStyle))
                menuBar->setStyle(compactStyle);
        }
        // push buttons are square too
        else if (auto *button = qobject_cast<QPushButton *>(watched))
        {
            static auto *buttonStyle = new ButtonStyle;
            if ((qApp->style()->name() == u"windows11"_s) && (button->style() != buttonStyle))
                button->setStyle(buttonStyle);
        }
    }

    return QObject::eventFilter(watched, event);
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
