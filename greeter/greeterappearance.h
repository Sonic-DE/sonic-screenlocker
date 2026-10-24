/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <KSharedConfig>
#include <Plasma/Theme>
#include <QObject>
#include <memory>

class QQmlEngine;

namespace ScreenLocker {
// Keep the selected SVG image set alive before the first control is created.
// The standalone locker still uses the user's application color scheme.
class GreeterAppearance : public QObject {
public:
    GreeterAppearance();
    void setupEngine(QQmlEngine *engine);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    KSharedConfig::Ptr m_colors;
    std::unique_ptr<Plasma::Theme> m_desktopTheme;
};
}
