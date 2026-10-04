#include "ThemeManager.h"

#include <QFile>
#include <QStyleHints>

ThemeManager::ThemeManager(QApplication* app, QObject* parent)
    : QObject(parent)
    , app_(app)
    , settings_(QStringLiteral("UniversalStorageAnalyzer"),
                QStringLiteral("UniversalStorageAnalyzer"))
{
    const int saved = settings_.value(QStringLiteral("theme_mode"), 0).toInt();
    mode_ = (saved >= 0 && saved <= 2) ? static_cast<Mode>(saved) : Mode::System;
    if (app_ != nullptr) {
        connect(app_->styleHints(), &QStyleHints::colorSchemeChanged, this,
                &ThemeManager::applySystemTheme);
    }
    apply();
}

void ThemeManager::setMode(Mode mode)
{
    mode_ = mode;
    settings_.setValue(QStringLiteral("theme_mode"), static_cast<int>(mode_));
    apply();
}

void ThemeManager::applySystemTheme()
{
    if (mode_ == Mode::System) {
        apply();
    }
}

void ThemeManager::apply()
{
    if (app_ == nullptr) {
        return;
    }
    bool dark{false};
    if (mode_ == Mode::Dark) {
        dark = true;
    } else if (mode_ == Mode::System) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        dark = (app_->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
#else
        dark = false;
#endif
    }
    app_->setStyleSheet(loadStylesheet(dark));
}

QString ThemeManager::loadStylesheet(bool dark) const
{
    const QString path =
        dark ? QStringLiteral(":/styles/dark.qss") : QStringLiteral(":/styles/light.qss");
    QFile f{path};
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(f.readAll());
    }
    return {};
}
