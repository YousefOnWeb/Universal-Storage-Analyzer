#pragma once

#include <QApplication>
#include <QObject>
#include <QSettings>

// Applies Light/Dark stylesheets and persists the choice via QSettings.
// Auto-follows QStyleHints::colorScheme when mode is System.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum class Mode : int { System = 0, Light = 1, Dark = 2 };
    Q_ENUM(Mode)

    explicit ThemeManager(QApplication* app, QObject* parent = nullptr);

    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    void applySystemTheme();

private:
    void apply();
    QString loadStylesheet(bool dark) const;

    QApplication* app_{nullptr};
    QSettings settings_;
    Mode mode_{Mode::System};
};
