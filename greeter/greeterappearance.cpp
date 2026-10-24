/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "greeterappearance.h"

#include <KColorScheme>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QDynamicPropertyChangeEvent>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStandardPaths>

namespace ScreenLocker {
GreeterAppearance::GreeterAppearance()
{
    if (QQuickStyle::name() != QLatin1String("org.kde.breeze")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.breeze"));
    }
    QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    if (path.isEmpty()) {
        path = QStringLiteral("kdeglobals");
    }
    m_colors = KSharedConfig::openConfig(path);
    const auto &globals = m_colors;
    const auto scheme = KConfigGroup(globals, QStringLiteral("General")).readEntry("ColorScheme", QString());
    if (!scheme.isEmpty()) {
        const auto defaults =
            QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("color-schemes/") + scheme + QStringLiteral(".colors"));
        if (!defaults.isEmpty()) {
            // Explicit user entries remain above the scheme's defaults.
            globals->addConfigSources({defaults});
        }
    }
    // Kirigami's palette-change handler reads this property while refreshing
    // its scheme cache. Setting it after setPalette leaves early controls
    // using the previous/default colors. Keep explicit user overrides too.
    qGuiApp->setProperty("KDE_COLOR_SCHEME_PATH", path);
    qGuiApp->setPalette(KColorScheme::createApplicationPalette(globals));
    m_desktopTheme = std::make_unique<Plasma::Theme>();
}

void GreeterAppearance::setupEngine(QQmlEngine *engine)
{
    // Plasma shell applets use the SVG theme's optional colors file. A
    // standalone locker, like the login greeter, uses the user's application
    // scheme instead. Keep the selected SVG artwork, not its separate palette.
    engine->setProperty("_kirigamiTheme", QStringLiteral("org.kde.breeze"));
    engine->installEventFilter(this);
}

bool GreeterAppearance::eventFilter(QObject *object, QEvent *event)
{
    // The shared look-and-feel component plugin assigns KirigamiPlasmaStyle
    // during import, after setupEngine(). Keep this standalone greeter's
    // policy authoritative before any of that module's controls are created.
    if (event->type() == QEvent::DynamicPropertyChange && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "_kirigamiTheme"
        && object->property("_kirigamiTheme").toString() != QLatin1String("org.kde.breeze")) {
        object->setProperty("_kirigamiTheme", QStringLiteral("org.kde.breeze"));
    }
    return QObject::eventFilter(object, event);
}
}
