#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QPushButton>
#include <QMessageBox>
#include <QGroupBox>
#include <QFile>
#include <QTextStream>
#include <QStyle>
#include <QFrame>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QPair>
#include <QSettings>

static const QString START_PATH     = "/sys/class/power_supply/BAT0/charge_start_threshold";
static const QString END_PATH       = "/sys/class/power_supply/BAT0/charge_control_end_threshold";
static const QString CAPACITY_PATH  = "/sys/class/power_supply/BAT0/capacity";
static const QString STATUS_PATH    = "/sys/class/power_supply/BAT0/status";
static const QString BEHAVIOUR_PATH = "/sys/class/power_supply/BAT0/charge_behaviour";

static int readInt(const QString &path, bool &ok) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { ok = false; return -1; }
    QTextStream s(&f);
    int v = s.readLine().trimmed().toInt(&ok);
    return v;
}

static QString readString(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    QTextStream s(&f);
    return s.readLine().trimmed();
}

static bool writeThreshold(const QString &path, int value) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream s(&f);
    s << value;
    return true;
}

static bool writeString(const QString &path, const QString &value) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream s(&f);
    s << value;
    return true;
}

static bool behaviourSupported() {
    QString raw = readString(BEHAVIOUR_PATH);
    return raw.contains("inhibit-charge");
}

static bool behaviourInhibited() {
    QString raw = readString(BEHAVIOUR_PATH);
    return raw.contains("[inhibit-charge]");
}

struct Preset { const char *name; int start; int end; };
static const Preset PRESETS[] = {
    { "Conservative (40 / 80)", 40, 80 },
    { "Balanced (60 / 90)",     60, 90 },
    { "Plugged in (75 / 80)",   75, 80 },
    { "Full (96 / 100)",        96, 100 },
};

static QIcon makeBatteryIcon(int capacity, bool charging) {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QRect body(8, 18, 44, 28);
    QRect tip(52, 26, 4, 12);
    p.setPen(QPen(Qt::white, 3));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(body, 3, 3);
    p.fillRect(tip, Qt::white);

    int fillW = qBound(0, (body.width() - 6) * capacity / 100, body.width() - 6);
    QColor fill = capacity < 20 ? QColor("#e74c3c")
                : capacity < 50 ? QColor("#f39c12")
                                : QColor("#2ecc71");
    p.fillRect(body.x() + 3, body.y() + 3, fillW, body.height() - 6, fill);

    if (charging) {
        p.setPen(QPen(QColor("#f1c40f"), 4));
        QPolygon bolt;
        bolt << QPoint(30, 22) << QPoint(24, 34) << QPoint(30, 34) << QPoint(26, 44)
             << QPoint(36, 30) << QPoint(30, 30);
        p.setBrush(QColor("#f1c40f"));
        p.setPen(Qt::NoPen);
        p.drawPolygon(bolt);
    }
    return QIcon(pm);
}

class ThresholdWidget : public QWidget {
    Q_OBJECT
public:
    ThresholdWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("Battery Charge Threshold");
        setMinimumWidth(360);

        auto *root = new QVBoxLayout(this);
        root->setSpacing(16);
        root->setContentsMargins(20, 20, 20, 20);

        auto *title = new QLabel("Battery Charge Thresholds");
        title->setStyleSheet("font-size: 16px; font-weight: bold;");
        root->addWidget(title);

        statusLabel_ = new QLabel();
        statusLabel_->setStyleSheet("color: #888; font-size: 12px;");
        statusLabel_->setWordWrap(true);
        root->addWidget(statusLabel_);

        auto *sep = new QFrame();
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Sunken);
        root->addWidget(sep);

        auto *startGroup = new QGroupBox("Start Charging (charge_start_threshold)");
        auto *startLayout = new QHBoxLayout(startGroup);
        startSpin_ = new QSpinBox();
        startSpin_->setRange(0, 99);
        startSpin_->setSuffix(" %");
        startLayout->addWidget(new QLabel("Start at:"));
        startLayout->addWidget(startSpin_);
        startLayout->addStretch();
        root->addWidget(startGroup);

        auto *endGroup = new QGroupBox("Stop Charging (charge_control_end_threshold)");
        auto *endLayout = new QHBoxLayout(endGroup);
        endSpin_ = new QSpinBox();
        endSpin_->setRange(1, 100);
        endSpin_->setSuffix(" %");
        endLayout->addWidget(new QLabel("Stop at:"));
        endLayout->addWidget(endSpin_);
        endLayout->addStretch();
        root->addWidget(endGroup);

        hintLabel_ = new QLabel("");
        hintLabel_->setStyleSheet("color: #c0392b; font-size: 12px;");
        hintLabel_->setWordWrap(true);
        root->addWidget(hintLabel_);

        pauseBtn_ = new QPushButton("Pause Charging");
        pauseBtn_->setCheckable(true);
        pauseBtn_->setStyleSheet("QPushButton:checked { background-color: #e67e22; color: white; }");
        root->addWidget(pauseBtn_);

        auto *btnRow = new QHBoxLayout();
        refreshBtn_ = new QPushButton(QChar(0x21BA) + QString("  Refresh"));
        applyBtn_   = new QPushButton(QChar(0x2713) + QString("  Apply"));
        hideBtn_    = new QPushButton("Hide to Tray");
        applyBtn_->setDefault(true);
        btnRow->addWidget(refreshBtn_);
        btnRow->addWidget(hideBtn_);
        btnRow->addStretch();
        btnRow->addWidget(applyBtn_);
        root->addLayout(btnRow);

        connect(refreshBtn_, &QPushButton::clicked, this, &ThresholdWidget::loadValues);
        connect(applyBtn_,   &QPushButton::clicked, this, [this]{ applyThresholds(startSpin_->value(), endSpin_->value(), true); });
        connect(hideBtn_,    &QPushButton::clicked, this, &QWidget::hide);
        connect(pauseBtn_,   &QPushButton::toggled, this, [this](bool on){ setPaused(on, true); });

        connect(startSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &ThresholdWidget::validate);
        connect(endSpin_,   QOverload<int>::of(&QSpinBox::valueChanged), this, &ThresholdWidget::validate);

        loadValues();
    }

    void setTray(QSystemTrayIcon *t) { tray_ = t; }

    QPair<int,int> currentThresholds() const {
        bool a, b;
        return { readInt(START_PATH, a), readInt(END_PATH, b) };
    }

    bool isPaused() const {
        if (behaviourSupported()) return behaviourInhibited();
        QSettings s;
        return s.value("paused", false).toBool();
    }

    void setPaused(bool on, bool fromUser) {
        QString err;
        if (behaviourSupported()) {
            if (!writeString(BEHAVIOUR_PATH, on ? "inhibit-charge" : "auto"))
                err = "Could not write charge_behaviour. Need root/setuid.";
        } else {
            QSettings s;
            if (on) {
                bool ok;
                int curEnd = readInt(END_PATH, ok);
                bool okC;
                int cap = readInt(CAPACITY_PATH, okC);
                if (!ok || !okC) { err = "Could not read battery state."; }
                else {
                    s.setValue("savedEnd", curEnd);
                    s.setValue("paused", true);
                    int target = qMax(cap, startSpin_->value() + 1);
                    if (!writeThreshold(END_PATH, target))
                        err = "Could not write end threshold. Need root/setuid.";
                }
            } else {
                int savedEnd = s.value("savedEnd", endSpin_->value()).toInt();
                s.setValue("paused", false);
                if (!writeThreshold(END_PATH, savedEnd))
                    err = "Could not restore end threshold. Need root/setuid.";
            }
        }

        if (!err.isEmpty()) {
            pauseBtn_->blockSignals(true);
            pauseBtn_->setChecked(!on);
            pauseBtn_->blockSignals(false);
            if (fromUser) QMessageBox::critical(this, "Pause Failed", err);
            else if (tray_) tray_->showMessage("Pause Failed", err, QSystemTrayIcon::Critical, 4000);
            return;
        }

        pauseBtn_->setText(on ? "Resume Charging" : "Pause Charging");
        if (tray_ && !isVisible() && fromUser) {
            tray_->showMessage(on ? "Charging Paused" : "Charging Resumed",
                on ? "Battery will hold at current level." : "Normal thresholds restored.",
                QSystemTrayIcon::Information, 2500);
        }
        loadValues();
        emit pausedChanged(on);
    }

    void applyThresholds(int startVal, int endVal, bool interactive) {
        if (startVal >= endVal) {
            if (interactive) {
                QMessageBox::warning(this, "Invalid Range",
                    "Start threshold must be less than end threshold.");
            }
            return;
        }
        bool ok1 = writeThreshold(START_PATH, startVal);
        bool ok2 = writeThreshold(END_PATH,   endVal);

        if (!ok1 || !ok2) {
            QString msg = "Could not write to sysfs.\n\n"
                          "Run binary as root or set setuid:\n"
                          "  sudo chown root:root thresholdctl-gui\n"
                          "  sudo chmod u+s thresholdctl-gui";
            if (interactive) {
                QMessageBox::critical(this, "Write Failed", msg);
            } else if (tray_) {
                tray_->showMessage("Threshold Write Failed", msg, QSystemTrayIcon::Critical, 5000);
            }
            return;
        }

        hintLabel_->setStyleSheet("color: #27ae60; font-size: 12px;");
        hintLabel_->setText(QString("Applied: start=%1%, stop=%2%").arg(startVal).arg(endVal));
        loadValues();
        if (tray_ && !isVisible()) {
            tray_->showMessage("Thresholds Applied",
                QString("Start %1%, Stop %2%").arg(startVal).arg(endVal),
                QSystemTrayIcon::Information, 2500);
        }
        emit thresholdsChanged();
    }

signals:
    void thresholdsChanged();
    void pausedChanged(bool on);

protected:
    void closeEvent(QCloseEvent *e) override {
        if (tray_ && tray_->isVisible()) {
            hide();
            e->ignore();
        } else {
            e->accept();
        }
    }

private slots:
    void loadValues() {
        bool ok1, ok2;
        int startVal = readInt(START_PATH, ok1);
        int endVal   = readInt(END_PATH,   ok2);

        if (!ok1 || !ok2) {
            statusLabel_->setText("Could not read threshold files. Check that BAT0 exists.");
            hintLabel_->setText("Read error.");
            applyBtn_->setEnabled(false);
            return;
        }

        startSpin_->blockSignals(true);
        endSpin_->blockSignals(true);
        startSpin_->setValue(startVal);
        endSpin_->setValue(endVal);
        startSpin_->blockSignals(false);
        endSpin_->blockSignals(false);

        bool okC;
        int cap = readInt(CAPACITY_PATH, okC);
        QString st = readString(STATUS_PATH);
        bool paused = isPaused();

        QString pauseTxt = paused ? "  [PAUSED]" : "";
        statusLabel_->setText(QString("BAT0: %1%%  —  %2  (thresholds %3 / %4)%5")
                              .arg(okC ? QString::number(cap) : "?")
                              .arg(st.isEmpty() ? "?" : st)
                              .arg(startVal).arg(endVal)
                              .arg(pauseTxt));

        pauseBtn_->blockSignals(true);
        pauseBtn_->setChecked(paused);
        pauseBtn_->setText(paused ? "Resume Charging" : "Pause Charging");
        pauseBtn_->blockSignals(false);

        validate();
    }

    void validate() {
        int s = startSpin_->value();
        int e = endSpin_->value();
        if (s >= e) {
            hintLabel_->setStyleSheet("color: #c0392b; font-size: 12px;");
            hintLabel_->setText("Start must be less than End threshold.");
            applyBtn_->setEnabled(false);
        } else {
            hintLabel_->setStyleSheet("color: #888; font-size: 12px;");
            hintLabel_->setText(QString("Range: start=%1%, stop=%2%  (gap: %3%)").arg(s).arg(e).arg(e-s));
            applyBtn_->setEnabled(true);
        }
    }

private:
    QSpinBox    *startSpin_;
    QSpinBox    *endSpin_;
    QLabel      *hintLabel_;
    QLabel      *statusLabel_;
    QPushButton *refreshBtn_;
    QPushButton *applyBtn_;
    QPushButton *hideBtn_;
    QPushButton *pauseBtn_;
    QSystemTrayIcon *tray_ = nullptr;
};

class TrayController : public QObject {
    Q_OBJECT
public:
    TrayController(ThresholdWidget *w, QObject *parent = nullptr)
        : QObject(parent), w_(w) {
        tray_ = new QSystemTrayIcon(this);
        menu_ = new QMenu();

        showAct_ = menu_->addAction("Show Window");
        connect(showAct_, &QAction::triggered, this, &TrayController::showWindow);

        menu_->addSeparator();
        pauseAct_ = menu_->addAction("Pause Charging");
        pauseAct_->setCheckable(true);
        connect(pauseAct_, &QAction::toggled, this, [this](bool on){ w_->setPaused(on, false); });
        connect(w_, &ThresholdWidget::pausedChanged, this, [this](bool on){
            pauseAct_->blockSignals(true);
            pauseAct_->setChecked(on);
            pauseAct_->setText(on ? "Resume Charging" : "Pause Charging");
            pauseAct_->blockSignals(false);
        });

        menu_->addSeparator();
        auto *presetMenu = menu_->addMenu("Presets");
        presetGroup_ = new QActionGroup(this);
        presetGroup_->setExclusive(true);
        for (const auto &p : PRESETS) {
            QAction *a = presetMenu->addAction(p.name);
            a->setCheckable(true);
            a->setData(QPoint(p.start, p.end));
            presetGroup_->addAction(a);
            connect(a, &QAction::triggered, this, [this, a]{
                QPoint d = a->data().toPoint();
                w_->applyThresholds(d.x(), d.y(), false);
            });
        }

        menu_->addSeparator();
        QAction *refreshAct = menu_->addAction("Refresh");
        connect(refreshAct, &QAction::triggered, this, &TrayController::refresh);

        menu_->addSeparator();
        QAction *quitAct = menu_->addAction("Quit");
        connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

        tray_->setContextMenu(menu_);
        connect(tray_, &QSystemTrayIcon::activated, this, &TrayController::onActivated);
        connect(w_, &ThresholdWidget::thresholdsChanged, this, &TrayController::refresh);

        w_->setTray(tray_);
        tray_->show();
        refresh();

        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &TrayController::refresh);
        timer->start(30000);
    }

private slots:
    void showWindow() {
        w_->show();
        w_->raise();
        w_->activateWindow();
    }

    void onActivated(QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
            if (w_->isVisible()) w_->hide();
            else showWindow();
        }
    }

    void refresh() {
        bool okC;
        int cap = readInt(CAPACITY_PATH, okC);
        QString st = readString(STATUS_PATH);
        bool charging = st.compare("Charging", Qt::CaseInsensitive) == 0;
        bool paused = w_->isPaused();

        tray_->setIcon(makeBatteryIcon(okC ? cap : 0, charging));

        auto thr = w_->currentThresholds();
        QString tip = QString("BAT0: %1%%  (%2)%3\nThresholds: %4 / %5")
                      .arg(okC ? QString::number(cap) : "?")
                      .arg(st.isEmpty() ? "?" : st)
                      .arg(paused ? "  [PAUSED]" : "")
                      .arg(thr.first).arg(thr.second);
        tray_->setToolTip(tip);

        pauseAct_->blockSignals(true);
        pauseAct_->setChecked(paused);
        pauseAct_->setText(paused ? "Resume Charging" : "Pause Charging");
        pauseAct_->blockSignals(false);

        for (QAction *a : presetGroup_->actions()) {
            QPoint d = a->data().toPoint();
            a->setChecked(d.x() == thr.first && d.y() == thr.second);
        }
    }

private:
    ThresholdWidget *w_;
    QSystemTrayIcon *tray_;
    QMenu *menu_;
    QAction *showAct_;
    QAction *pauseAct_;
    QActionGroup *presetGroup_;
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication::setSetuidAllowed(true);
    QApplication app(argc, argv);
    app.setApplicationName("thresholdctl-gui");
    app.setOrganizationName("thresholdctl-gui");
    app.setApplicationDisplayName("Battery Threshold Control");
    QApplication::setQuitOnLastWindowClosed(false);

    ThresholdWidget w;
    TrayController tc(&w);

    bool startHidden = false;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--tray" || a == "--hidden") startHidden = true;
    }
    if (!startHidden) w.show();

    return app.exec();
}
