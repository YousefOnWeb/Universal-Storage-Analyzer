#include "OptionsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QSettings makeSettings()
{
    return QSettings(QStringLiteral("UniversalStorageAnalyzer"),
                     QStringLiteral("UniversalStorageAnalyzer"));
}

} // namespace

QString OptionsDialog::loadBucketsCsv()
{
    return makeSettings()
        .value(QStringLiteral("options/ageBuckets"), QString::fromLatin1(kDefaultBuckets))
        .toString();
}

std::vector<std::uint64_t> OptionsDialog::loadEdges()
{
    const auto edges = parseEdges(loadBucketsCsv());
    if (!edges.empty()) {
        return edges;
    }
    return parseEdges(QString::fromLatin1(kDefaultBuckets));
}

int OptionsDialog::loadTopN()
{
    bool ok{false};
    const int n = makeSettings().value(QStringLiteral("options/topN"), kDefaultTopN).toInt(&ok);
    if (!ok || n < kMinTopN || n > kMaxTopN) {
        return kDefaultTopN;
    }
    return n;
}

QString OptionsDialog::loadStartup()
{
    const QString mode =
        makeSettings().value(QStringLiteral("options/startup"), QStringLiteral("last")).toString();
    if (mode == QStringLiteral("home") || mode == QStringLiteral("ask")) {
        return mode;
    }
    return QStringLiteral("last");
}

bool OptionsDialog::loadThrottle()
{
    return makeSettings().value(QStringLiteral("options/throttle"), true).toBool();
}

bool OptionsDialog::validateBuckets(const QString& csv, QString* error)
{
    const auto setError = [&](const QString& msg) {
        if (error != nullptr) {
            *error = msg;
        }
        return false;
    };
    const QStringList parts = csv.split(QStringLiteral(","), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 12) {
        return setError(QStringLiteral("Enter 1 to 12 day counts."));
    }
    std::uint64_t prev{0};
    for (const QString& part : parts) {
        bool ok{false};
        const std::uint64_t days = part.trimmed().toULongLong(&ok);
        if (!ok || days == 0 || days > 3650) {
            return setError(QStringLiteral("Each bucket must be 1–3650 days."));
        }
        if (days <= prev) {
            return setError(QStringLiteral("Buckets must ascend."));
        }
        prev = days;
    }
    return true;
}

std::vector<std::uint64_t> OptionsDialog::parseEdges(const QString& csv)
{
    std::vector<std::uint64_t> edges{};
    QString ignored{};
    if (!validateBuckets(csv, &ignored)) {
        return edges;
    }
    for (const QString& part : csv.split(QStringLiteral(","), Qt::SkipEmptyParts)) {
        edges.push_back(part.trimmed().toULongLong());
    }
    return edges;
}

OptionsDialog::OptionsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Options"));
    setAccessibleName(QStringLiteral("Options dialog"));

    bucketsEdit_ = new QLineEdit(this);
    bucketsEdit_->setText(loadBucketsCsv());
    bucketsEdit_->setPlaceholderText(QString::fromLatin1(kDefaultBuckets));
    bucketsEdit_->setAccessibleName(QStringLiteral("Age bucket edges in days"));

    topSpin_ = new QSpinBox(this);
    topSpin_->setRange(kMinTopN, kMaxTopN);
    topSpin_->setValue(loadTopN());
    topSpin_->setAccessibleName(QStringLiteral("Largest-files list size"));

    startupCombo_ = new QComboBox(this);
    startupCombo_->addItems({QStringLiteral("last"), QStringLiteral("home"),
                             QStringLiteral("ask")});
    startupCombo_->setCurrentText(loadStartup());
    startupCombo_->setAccessibleName(QStringLiteral("Startup target"));

    exportCombo_ = new QComboBox(this);
    exportCombo_->addItems({QStringLiteral("CSV (Milestone 4)")});
    exportCombo_->setEnabled(false);
    exportCombo_->setToolTip(QStringLiteral("Multi-format export arrives in Milestone 4"));
    exportCombo_->setAccessibleName(QStringLiteral("Export format"));

    throttleCheck_ = new QCheckBox(QStringLiteral("Throttle scan on busy disks"), this);
    throttleCheck_->setChecked(loadThrottle());
    throttleCheck_->setAccessibleName(QStringLiteral("Throttle on busy disks"));

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(QStringLiteral("optionsError"));
    QPalette pal{errorLabel_->palette()};
    pal.setColor(QPalette::WindowText, Qt::red);
    errorLabel_->setPalette(pal);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &OptionsDialog::reject);

    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Age buckets (days):"), bucketsEdit_);
    form->addRow(QStringLiteral("Largest files:"), topSpin_);
    form->addRow(QStringLiteral("Startup target:"), startupCombo_);
    form->addRow(QStringLiteral("Export format:"), exportCombo_);
    form->addRow(QStringLiteral("Disk throttle:"), throttleCheck_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(errorLabel_);
    layout->addWidget(buttons);
}

void OptionsDialog::setBucketsCsv(const QString& csv)
{
    bucketsEdit_->setText(csv);
}

void OptionsDialog::save() const
{
    QSettings s = makeSettings();
    s.setValue(QStringLiteral("options/ageBuckets"), bucketsEdit_->text());
    s.setValue(QStringLiteral("options/topN"), topSpin_->value());
    s.setValue(QStringLiteral("options/startup"), startupCombo_->currentText());
    s.setValue(QStringLiteral("options/throttle"), throttleCheck_->isChecked());
}

void OptionsDialog::accept()
{
    QString error{};
    if (!validateBuckets(bucketsEdit_->text(), &error)) {
        errorLabel_->setText(error);
        return; // stay open: fixable input gets inline errors, not popups
    }
    errorLabel_->clear();
    save();
    QDialog::accept();
}
