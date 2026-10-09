#pragma once

#include <QDialog>
#include <QSettings>
#include <QString>

#include <cstdint>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

// Runtime options, persisted in QSettings (small prefs per AGENTS.md §2,
// never the SQLite store). The dialog loads current values, validates on
// accept (invalid input keeps the dialog open with an inline error), and
// saves only when everything checks out.
class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kDefaultBuckets{"1,7,30,180,365"};
    static constexpr int kDefaultTopN{1000};
    static constexpr int kMinTopN{100};
    static constexpr int kMaxTopN{10000};

    static QString loadBucketsCsv();
    static std::vector<std::uint64_t> loadEdges();
    static int loadTopN();
    static QString loadStartup(); // "last" | "home" | "ask"
    static bool loadThrottle();

    // Strict ascending positive day counts, 1-12 edges, each <= 3650.
    // Never throws; error receives the human reason on failure.
    static bool validateBuckets(const QString& csv, QString* error = nullptr);
    static std::vector<std::uint64_t> parseEdges(const QString& csv);

    explicit OptionsDialog(QWidget* parent = nullptr);

    // Test helper: presets the buckets field like keyboard input would.
    void setBucketsCsv(const QString& csv);

public slots:
    void accept() override;

private:
    void save() const;

    QLineEdit* bucketsEdit_{nullptr};
    QSpinBox* topSpin_{nullptr};
    QComboBox* startupCombo_{nullptr};
    QComboBox* exportCombo_{nullptr};
    QCheckBox* throttleCheck_{nullptr};
    QLabel* errorLabel_{nullptr};
};
