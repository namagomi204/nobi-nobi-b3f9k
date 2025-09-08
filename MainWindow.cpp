#include "MainWindow.h"

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include "ui_MainWindow.h"
#include "iv_greeks.h"

#include "WebSocketClient.h"
#include "ux_support.h"
#include "engine_helpers.h"

#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QHeaderView>
#include <QLocale>
#include <QList>
#include <cmath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QSettings>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <vector>

#ifdef HAS_IV_SOLVER
#include "iv_greeks.h"
#endif
// ← plotLine() から使うので前方宣言
static int clamp_i(int x, int lo, int hi);

// ===== Charts 汎用描画関数 =====
static void plotLine(QChartView* view,
    const QList<QPointF>& pts,
    const char* xfmt = "%.1f",
    const char* /*yfmt*/ = "%.2e")
{
    if (!view) return;

    // 有限点だけ抽出
    QList<QPointF> clean;
    clean.reserve(pts.size());
    double maxAbs = 0.0;
    for (const QPointF& p : pts) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) continue;
        clean.append(p);
        maxAbs = std::max(maxAbs, std::fabs(p.y()));
    }

    QChart* ch = view->chart();
    if (!ch) { ch = new QChart(); view->setChart(ch); }
    ch->removeAllSeries();
    // 既存の軸もクリア（非推奨API使用回避）
    if (auto* ax = ch->axisX()) ch->removeAxis(ax);
    if (auto* ay = ch->axisY()) ch->removeAxis(ay);
    if (clean.isEmpty()) return;

    // Yスケール（工学表記の指数を見て割る）
    int e3 = 0;
    if (maxAbs > 0.0) {
        const int e = int(std::floor(std::log10(maxAbs)));
        e3 = clamp_i((e / 3) * 3, -12, 12);
    }
    const double scale = (e3 == 0 ? 1.0 : std::pow(10.0, e3));

    auto* series = new QLineSeries();
    for (const QPointF& p : clean)
        series->append(p.x(), (scale != 0.0 ? p.y() / scale : p.y()));
    ch->addSeries(series);

    // 新しい軸を作って追加 → series にアタッチ
    auto* axX = new QValueAxis();
    auto* axY = new QValueAxis();
    ch->addAxis(axX, Qt::AlignBottom);
    ch->addAxis(axY, Qt::AlignLeft);
    series->attachAxis(axX);
    series->attachAxis(axY);

    // X軸フォーマットとタイトル
    axX->setLabelFormat(QString::fromLatin1(xfmt));
    const QString t = ch->title();
    const int l = t.indexOf('('), r = t.indexOf(')');
    if (l >= 0 && r > l) axX->setTitleText(t.mid(l + 1, r - l - 1).trimmed());

    // Y軸フォーマットと「×10^n」
    const int decimals = (e3 <= -9 ? 6 : (e3 <= -6 ? 5 : (e3 <= -3 ? 4 : 3)));
    axY->setLabelFormat(QString("%.%1f").arg(decimals));
    const QString base = ch->title().split('(').first().trimmed();
    axY->setTitleText(e3 == 0 ? base : QString("%1 (×10^%2)").arg(base).arg(e3));

    view->setRenderHint(QPainter::Antialiasing, true);
}

static double trySolveIV(bool isCall, double price, double S, double K, double minutes)
{
#ifdef HAS_IV_SOLVER
    // ※ iv_greeks.h 側のシグネチャ・名前空間が異なる場合はここだけ合わせればOK
    auto gk = IVGreeks::solveAndGreeks(
        isCall ? OptionCP::Call : OptionCP::Put,
        price, S, K, minutes, 0.0, 0.0);
    return gk.iv;
#else
    (void)isCall; (void)price; (void)S; (void)K; (void)minutes;
    return 0.0;
#endif
}

// --- SI表記とツールチップ付きセル ---
static QString fmtSI(double v, int digits = 3) {
    if (v == 0.0 || !std::isfinite(v)) return QString::number(v, 'g', digits);
    double av = std::fabs(v);
    int e = int(std::floor(std::log10(av)));
    int k3 = std::clamp((e / 3), -4, 4); // -12..+12 → -4..+4
    static const QStringList unit = { "p","n","µ","m","","k","M","G","T" };
    double scaled = v / std::pow(10.0, k3 * 3);
    return QString("%1 %2").arg(QString::number(scaled, 'f', digits),
        unit[k3 + 4]);
}

static QTableWidgetItem* mkNumItemSI(double v, int digits = 3) {
    auto* it = new QTableWidgetItem;
    it->setData(Qt::EditRole, v); // ソート用：生値
    it->setText(fmtSI(v, digits)); // 表示：SI
    it->setToolTip(
        QString("raw: %1\nsci: %2")
        .arg(QString::number(v, 'g', 12))
        .arg(QString::number(v, 'e', 6))
    );
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}

// ---- NaN/Inf ガード用 ----
static inline double fin(double x, double fallback = 0.0) {
    return std::isfinite(x) ? x : fallback;
}

// --- 工学スケール（×10^n, nは3の倍数）で表を読みやすく ---
static int clamp_i(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

struct EngScale { int e3 = 0; double scale = 1.0; }; // e3: -12..12 の3の倍数

static EngScale calcEngScale(const QVector<double>& vals) {
    double maxAbs = 0.0;
    for (double v : vals) { double a = std::fabs(v); if (a > maxAbs) maxAbs = a; }
    EngScale s;
    if (maxAbs > 0.0) {
        int e = int(std::floor(std::log10(maxAbs)));
        s.e3 = clamp_i((e / 3) * 3, -12, 12);
        s.scale = std::pow(10.0, s.e3);
    }
    return s;
}

static QTableWidgetItem* mkNumItemScaled(double raw, const EngScale& sc, int decimals = 3) {
    auto* it = new QTableWidgetItem;
    it->setData(Qt::EditRole, raw); // ソートは生値
    const double shown = (sc.scale != 0.0 ? raw / sc.scale : raw);
    it->setText(QString::number(shown, 'f', decimals));
    it->setToolTip(QString("raw: %1\nscaled: %2 ×10^%3")
        .arg(QString::number(raw, 'g', 12))
        .arg(QString::number(shown, 'f', decimals))
        .arg(sc.e3));
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}
// ============ アプリ設定の保存/復元（QSettings） ============
// MainWindow.h は先頭でインクルード済みの想定
static void loadPrefs(MainWindow* self) {
    if (!self) return;
    QSettings s("BTC_OP_V2", "BTC_OP_V2");  // Org, App

    // ウインドウ配置（あれば復元）
    if (s.contains("win/geometry")) self->restoreGeometry(s.value("win/geometry").toByteArray());
    if (s.contains("win/state"))    self->restoreState(s.value("win/state").toByteArray());

    // UI 値（ui を直接触らず findChild で取得）
    if (auto* d = self->findChild<QDoubleSpinBox*>("spinMinSize"))
        d->setValue(s.value("ui/minSize", 0).toInt()); // 0=Auto
    if (auto* sp = self->findChild<QSpinBox*>("spinBackHours"))
        sp->setValue(s.value("ui/backHours", 24).toInt());
    if (auto* chk = self->findChild<QCheckBox*>("chkPauseTape"))
        chk->setChecked(s.value("ui/pauseTape", false).toBool());
    if (auto* cmb = self->findChild<QComboBox*>("comboMoneyness")) {
        const QString want = s.value("ui/moneyness", "All").toString();
        const int idx = cmb->findText(want, Qt::MatchFixedString);
        if (idx >= 0) cmb->setCurrentIndex(idx);
    }
    // 満期は銘柄一覧取得後（onRpc 内）で復元する
}

static void savePrefs(const MainWindow* self) {
    if (!self) return;
    QSettings s("BTC_OP_V2", "BTC_OP_V2");

    // ウインドウ配置
    s.setValue("win/geometry", self->saveGeometry());
    s.setValue("win/state", self->saveState());

    // UI 値（ui を直接触らず findChild で取得）
    if (auto* d = self->findChild<QDoubleSpinBox*>("spinMinSize"))
        s.setValue("ui/minSize", int(std::llround(d->value())));
    if (auto* sp = self->findChild<QSpinBox*>("spinBackHours"))
        s.setValue("ui/backHours", sp->value());
    if (auto* chk = self->findChild<QCheckBox*>("chkPauseTape"))
        s.setValue("ui/pauseTape", chk->isChecked());
    if (auto* cmb = self->findChild<QComboBox*>("comboMoneyness"))
        s.setValue("ui/moneyness", cmb->currentText());

    // 満期（comboExpiry の UserData は qint64）
    if (auto* cmbExp = self->findChild<QComboBox*>("comboExpiry")) {
        const QVariant v = cmbExp->currentData();
        s.setValue("ui/expiryMs", v.isValid() ? v.toLongLong() : 0ll);
    }

    s.sync();
}

// ---- Delta backfill watermark helpers ----
static qint64 loadBackfillWatermarkMs() {
    QSettings s("BTC_OP_V2", "BTC_OP_V2");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 def = now - 24ll * 60 * 60 * 1000; // フォールバック=過去24h
    return s.value("cache/lastBackfillToMs", def).toLongLong();
}
static void storeBackfillWatermarkMs(qint64 toMs) {
    QSettings s("BTC_OP_V2", "BTC_OP_V2");
    s.setValue("cache/lastBackfillToMs", toMs);
    s.sync();
}

// ============ 状態スナップショット（残存・アンカー・Auto閾値用サンプルなど） ============
bool MainWindow::loadSnapshot() {
    QSettings s("BTC_OP_V2", "BTC_OP_V2");
    const QByteArray blob = s.value("state/snapshot").toByteArray();
    if (blob.isEmpty()) return false;

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(blob, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();

    // 1) 前回保存時刻
    m_lastSnapshotTs = qint64(o.value("ts").toDouble());

    // 2) マップ類
    auto loadMapD = [&](const char* key, QHash<QString, double>& dst) {
        dst.clear();
        const QJsonObject m = o.value(key).toObject();
        for (auto it = m.begin(); it != m.end(); ++it) dst.insert(it.key(), it.value().toDouble());
        };
    auto loadMapI64 = [&](const char* key, QHash<QString, qint64>& dst) {
        dst.clear();
        const QJsonObject m = o.value(key).toObject();
        for (auto it = m.begin(); it != m.end(); ++it) dst.insert(it.key(), qint64(it.value().toDouble()));
        };
    auto loadMapI = [&](const char* key, QHash<QString, int>& dst) {
        dst.clear();
        const QJsonObject m = o.value(key).toObject();
        for (auto it = m.begin(); it != m.end(); ++it) dst.insert(it.key(), it.value().toInt());
        };
    auto loadMapSet = [&](const char* key, QHash<QString, QSet<QString>>& dst) {
        dst.clear();
        const QJsonObject m = o.value(key).toObject();
        for (auto it = m.begin(); it != m.end(); ++it) {
            QSet<QString> set;
            for (const auto& v : it.value().toArray()) set.insert(v.toString());
            dst.insert(it.key(), set);
        }
        };

    loadMapD("residualQty", m_residualQtyByKey);
    loadMapD("residualDVol", m_residualDVolByKey);
    loadMapD("residualSignedQty", m_residualSignedQtyByKey);
    loadMapI64("residualLastTs", m_residualLastTsByKey);
    loadMapI("residualTrades", m_residualTradesByKey);
    loadMapSet("residualInsts", m_residualInstsByKey);
    loadMapI64("signalAnchorTs", m_signalAnchorTsByKey);

    // Auto 閾値用サンプル（直近だけでOK）
    m_amtSamples.clear();
    for (const auto& v : o.value("amtSamples").toArray()) {
        const auto a = v.toArray();
        if (a.size() == 2) m_amtSamples.append(AmtSample{ qint64(a[0].toDouble()), a[1].toDouble() });
    }

    // 代表I// 代表IV/Δ（任意・あれば復元）
    m_lastIV.clear();
    {
        const QJsonObject lastIVObj = o.value("lastIV").toObject();
        for (auto it = lastIVObj.begin(); it != lastIVObj.end(); ++it) {
            m_lastIV.insert(it.key(), it.value().toDouble());
        }
    }

    m_lastDelta.clear();
    {
        const QJsonObject lastDeltaObj = o.value("lastDelta").toObject();
        for (auto it = lastDeltaObj.begin(); it != lastDeltaObj.end(); ++it) {
            m_lastDelta.insert(it.key(), it.value().toDouble());
        }
    }

    // 即時描画
    rebuildSignalTableFromResidual();
    updateExpiryActivityTable();
    updatePinMapTable();
    updateCurvesTables();
    updateCurvesCharts();

    ui->plainTextEdit->appendPlainText(QString("[情報] 前回スナップショットを復元しました（%1キー）。")
        .arg(m_residualQtyByKey.size()));
    return true;
}

void MainWindow::saveSnapshot() const {
    QSettings s("BTC_OP_V2", "BTC_OP_V2");

    auto dumpMapD = [&](const QHash<QString, double>& src) {
        QJsonObject m; for (auto it = src.begin(); it != src.end(); ++it) m.insert(it.key(), it.value());
        return m;
        };
    auto dumpMapI64 = [&](const QHash<QString, qint64>& src) {
        QJsonObject m; for (auto it = src.begin(); it != src.end(); ++it) m.insert(it.key(), double(it.value()));
        return m;
        };
    auto dumpMapI = [&](const QHash<QString, int>& src) {
        QJsonObject m; for (auto it = src.begin(); it != src.end(); ++it) m.insert(it.key(), it.value());
        return m;
        };
    auto dumpMapSet = [&](const QHash<QString, QSet<QString>>& src) {
        QJsonObject m;
        for (auto it = src.begin(); it != src.end(); ++it) {
            QJsonArray a; for (const auto& s : it.value()) a.append(s);
            m.insert(it.key(), a);
        }
        return m;
        };

    QJsonObject o;
    o.insert("ts", double(QDateTime::currentMSecsSinceEpoch()));
    o.insert("residualQty", dumpMapD(m_residualQtyByKey));
    o.insert("residualDVol", dumpMapD(m_residualDVolByKey));
    o.insert("residualSignedQty", dumpMapD(m_residualSignedQtyByKey));
    o.insert("residualLastTs", dumpMapI64(m_residualLastTsByKey));
    o.insert("residualTrades", dumpMapI(m_residualTradesByKey));
    o.insert("residualInsts", dumpMapSet(m_residualInstsByKey));
    o.insert("signalAnchorTs", dumpMapI64(m_signalAnchorTsByKey));

    // Auto 閾値用サンプルは直近1000件だけ
    QJsonArray samples;
    const int sz = static_cast<int>(m_amtSamples.size());
    const int keep = std::min(1000, sz);
    for (int i = std::max(0, sz - keep); i < sz; ++i) {
        const auto& a = m_amtSamples[i];
        QJsonArray row; row.append(double(a.ts)); row.append(a.absAmt); samples.append(row);
    }
    o.insert("amtSamples", samples);

    // 任意
    o.insert("lastIV", dumpMapD(m_lastIV));
    o.insert("lastDelta", dumpMapD(m_lastDelta));

    const QByteArray blob = QJsonDocument(o).toJson(QJsonDocument::Compact);
    s.setValue("state/snapshot", blob);
    s.sync();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveSnapshot();                 // 状態保存
    // もし load/savePrefs を使っているならここで savePrefs(this); を呼ぶ
    QMainWindow::closeEvent(e);
}


// ---- Auto 閾値のポリシー（調整しやすいよう定数化）----
static constexpr int   AUTO_FLOOR = 50;    // 最低でも 50 枚
static constexpr int   AUTO_MIN_SAMPLES = 200;   // サンプル不足なら強制 FLOOR
static constexpr double AUTO_Q = 0.98;  // 98パーセンタイル
static constexpr int   AUTO_ROUND_STEP = 10;    // 10 枚刻みで切り上げ
static constexpr qint64 DAY_MS64 = 24ll * 60 * 60 * 1000;
static constexpr int   ALL_BACK_DAYS = 7; // 「全満期バックフィル」の既定期間（日）。24h制限撤廃。
// バックフィル時の最小枚数：手動>0なら手動値、Auto(=0)なら1枚で全件保持
static inline int backfillMinUnit(const Ui::MainWindow* ui) {
    if (ui && ui->spinMinSize) {
        int manual = int(std::llround(ui->spinMinSize->value()));
        if (manual > 0) return manual;
    }
    return 1;
}
// 24h バッファ管理
void MainWindow::pushAmtSample(qint64 ts, double absAmt) {
    if (!(absAmt > 0.0) || !(ts > 0)) return;
    m_amtSamples.append(AmtSample{ ts, absAmt });
    pruneAmtSamples(ts);
}
void MainWindow::pruneAmtSamples(qint64 now) {
    const qint64 cutoff = now - DAY_MS64;
    while (!m_amtSamples.isEmpty() && m_amtSamples.front().ts < cutoff)
        m_amtSamples.removeFirst();
}

// Auto 閾値（枚）の決定
int MainWindow::currentBigUnit() const {
    // 手動指定があればそれを使う
    if (ui->spinMinSize) {
        int manual = int(std::llround(ui->spinMinSize->value()));
        if (manual > 0) return manual;
    }

    // 24h の全サンプルから分位点
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // prune（const関数だが軽微なので const_cast で許容、嫌なら呼び出し側でprune）
    const_cast<MainWindow*>(this)->pruneAmtSamples(now);

    std::vector<double> vals;
    vals.reserve(size_t(m_amtSamples.size()));
    for (const auto& s : m_amtSamples) vals.push_back(s.absAmt);

    int unit = AUTO_FLOOR;
    if ((int)vals.size() >= AUTO_MIN_SAMPLES) {
        // nth_element で p 分位
        const double p = std::clamp(AUTO_Q, 0.0, 1.0);
        size_t k = (size_t)std::floor((vals.size() - 1) * p);
        std::nth_element(vals.begin(), vals.begin() + k, vals.end());
        unit = (int)std::llround(vals[k]);
        if (unit < AUTO_FLOOR) unit = AUTO_FLOOR;
    }
    // 10枚刻みに切り上げ
    if (unit % AUTO_ROUND_STEP) {
        unit = ((unit + AUTO_ROUND_STEP - 1) / AUTO_ROUND_STEP) * AUTO_ROUND_STEP;
    }
    return unit;
}
/* ================= ctor / dtor ================= */

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // --- Charts tab: QChart を差し込んでおく（空でも軸が出る） ---
    auto initChart = [](QChartView* v, const QString& title) {
        if (!v) return;
        auto* ch = new QChart();
        ch->legend()->hide();
        ch->setTitle(title);
        ch->setMargins(QMargins(6, 6, 6, 6));
        v->setRenderHint(QPainter::Antialiasing, true);
        v->setChart(ch);
        };


    initChart(ui->viewGamma, QStringLiteral("Gamma (残存日)"));
    initChart(ui->viewVega, QStringLiteral("Vega (残存日)"));
    initChart(ui->viewCumPnl, QStringLiteral("Cumulative PnL (分)"));


    {
        if (auto vbox = findChild<QVBoxLayout*>("vboxCurves")) {
            m_curvesPane = new CurvesChartPane(this);
            vbox->insertWidget(0, m_curvesPane, /*stretch*/1);
        }
        m_pnlStartMs = QDateTime::currentMSecsSinceEpoch();
        m_cumPnlValue = 0.0;
    }

    // --- レッグ明細テーブル（存在すれば使う：いずれかの名前を探索） ---
    m_tableLegs = findChild<QTableWidget*>("tableLegs");
    if (!m_tableLegs) m_tableLegs = findChild<QTableWidget*>("tableLegDetails");
    if (!m_tableLegs) m_tableLegs = findChild<QTableWidget*>("tableLegDetail");
    if (m_tableLegs) {
        m_tableLegs->setColumnCount(19);
        QStringList hdr;
        hdr << "時刻" << "LinkID" << "アグレッサ" << "Venue" << "銘柄" << "Call/Put"
            << "満期" << "行使" << "数量" << "プレミアム" << "通貨" << "乗数M" << "手数料"
            << "Trade IV" << "NBBO Bid" << "NBBO Ask" << "Mid" << "乖離(bp)" << "OrderID";
        m_tableLegs->setHorizontalHeaderLabels(hdr);
        m_tableLegs->horizontalHeader()->setStretchLastSection(true);
        m_tableLegs->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_tableLegs->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tableLegs->setSortingEnabled(true);
        m_tableLegs->setColumnWidth(0, 140);
        m_tableLegs->setColumnWidth(2, 60);
        m_tableLegs->setColumnWidth(4, 140);
        m_tableLegs->setColumnWidth(6, 130);
        m_tableLegs->setColumnWidth(7, 70);
        m_tableLegs->setColumnWidth(8, 80);
        m_tableLegs->setColumnWidth(17, 80);
    }


    // --- シグナル行選択 → 0列(UserRoleに入れたkey)を読んで明細表示 ---
    if (ui->tableSignals && ui->tableSignals->selectionModel()) {
        connect(ui->tableSignals->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& cur, const QModelIndex&) {
                if (!cur.isValid()) return;
                auto* item = ui->tableSignals->item(cur.row(), 0);
                if (!item) return;
                const QString key = item->data(Qt::UserRole).toString();
                if (!key.isEmpty()) populateLegDetailsForKey(key);
            });
    }


    // ---- UI 既定値 ----
    if (ui->comboMoneyness) {
        int idxAll = ui->comboMoneyness->findText("All", Qt::MatchExactly);
        if (idxAll >= 0) ui->comboMoneyness->setCurrentIndex(idxAll);
    }
    if (ui->spinMinSize) {
        ui->spinMinSize->setDecimals(0);          // 小数なし = 整数
        ui->spinMinSize->setMinimum(0);           // 0 を許可
        ui->spinMinSize->setSingleStep(1);
        ui->spinMinSize->setSpecialValueText(QStringLiteral("Auto")); // 0表示時に「Auto」
        ui->spinMinSize->setValue(0);             // 既定をAutoにしたい場合（25に戻すならここを25）
    }
    if (auto* sp = findChild<QSpinBox*>("spinBackHours")) sp->setValue(24);

    // シグナル表
    if (ui->tableSignals) {
        ui->tableSignals->setColumnCount(10);
        QStringList hdr;
        hdr << "時刻" << "満期" << "方向" << "パターン" << "行使" << "枚数"
            << "推定Δ" << "強度" << "名目(USD)" << "詳細";
        ui->tableSignals->setHorizontalHeaderLabels(hdr);
        ui->tableSignals->horizontalHeader()->setStretchLastSection(true);
        ui->tableSignals->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->tableSignals->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->tableSignals->setSortingEnabled(true);
        ui->tableSignals->setColumnWidth(0, 140);
        ui->tableSignals->setColumnWidth(1, 130);
        ui->tableSignals->setColumnWidth(2, 36);
        ui->tableSignals->setColumnWidth(3, 110);
        ui->tableSignals->setColumnWidth(4, 70);
        ui->tableSignals->setColumnWidth(5, 70);
        ui->tableSignals->setColumnWidth(6, 60);
        ui->tableSignals->setColumnWidth(7, 70);
        ui->tableSignals->setColumnWidth(8, 110);
    }

    // 満期アクティビティ表
    if (ui->tableExpiryActivity) {
        ui->tableExpiryActivity->setColumnCount(4);
        QStringList hdr; hdr << "満期" << "全期間枚数" << "24h枚数" << "1h枚数";
        ui->tableExpiryActivity->setHorizontalHeaderLabels(hdr);
        ui->tableExpiryActivity->horizontalHeader()->setStretchLastSection(true);
        ui->tableExpiryActivity->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->tableExpiryActivity->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->tableExpiryActivity->setSortingEnabled(true);
        ui->tableExpiryActivity->setColumnWidth(0, 130);
        ui->tableExpiryActivity->setColumnWidth(1, 80);
        ui->tableExpiryActivity->setColumnWidth(2, 70);
        ui->tableExpiryActivity->setColumnWidth(3, 70);

        // ピンマップ（列幅だけ整える）
        if (auto* tbl = findChild<QTableWidget*>("tablePinMap")) {
            tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
            tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
            tbl->setSortingEnabled(true);
            tbl->setAlternatingRowColors(true);
            tbl->setColumnWidth(0, 130); // 満期
            tbl->setColumnWidth(1, 60);  // CP
            tbl->setColumnWidth(2, 70);  // 行使
            tbl->setColumnWidth(3, 110); // 距離%
            tbl->setColumnWidth(4, 90);  // 残存枚数
            tbl->setColumnWidth(5, 110); // 残存dVol
            tbl->setColumnWidth(6, 90);  // OI
            tbl->setColumnWidth(7, 90);  // Pin指数
        }


        // クリックした列を覚えて維持（1=24h, 2=1h）
        connect(ui->tableExpiryActivity->horizontalHeader(),
            &QHeaderView::sortIndicatorChanged,
            this,
            [this](int col, Qt::SortOrder order) {
                m_expActSortCol = col;
                m_expActSortOrder = order;
            });
        ui->tableExpiryActivity->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder);
        m_expActSortCol = 1;
        m_expActSortOrder = Qt::DescendingOrder;
    }

    hookUiActions();

    // ---- WS 初期化 ----
    m_ws = new WebSocketClient(this);
    connect(m_ws, &WebSocketClient::msgReceived, this, [this](const QJsonObject& o) { handleDeribitMsg(o); });
    connect(m_ws, &WebSocketClient::rpcReceived, this, [this](int id, const QJsonObject& rep) { onRpc(id, rep); });
    connect(m_ws, &WebSocketClient::connected, this, [this] {
        bootstrapAuto();
        // 全BTCオプションの約定を購読（ログ／満期アクティビティ／残存用）
        m_ws->subscribe(QStringList() << "trades.option.BTC.raw");
        ui->plainTextEdit->appendPlainText("[情報] BTC全体トレード購読: trades.option.BTC.raw");
        });
    m_ws->connectPublic();

    // ---- IV オンデマンド取得ポンプ（200msに1件・同時1）----
    connect(&m_ivTimer, &QTimer::timeout, this, [this] { pumpIV(); });
    m_ivTimer.setInterval(200);
    m_ivTimer.start();

    // ---- OI 定期取得（60s毎）----
    connect(&m_oiTimer, &QTimer::timeout, this, [this] { requestOIAll(); });
    m_oiTimer.setInterval(60 * 1000);
    m_oiTimer.start();

    // ---- UI 1秒更新 ----
    connect(&m_uiTick, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        pruneOld(now);

        const double d1m = sumDeltaVolume(now, ONE_MIN_MS);
        const double d5m = sumDeltaVolume(now, FIVE_MIN_MS);
        ui->valueDVol1m->setText(fmt2(d1m));
        ui->valueDVol5m->setText(fmt2(d5m));

        QString ivText = "-";
        if (!m_targetInstruments.isEmpty()) {
            const auto& inst0 = m_targetInstruments.front();
            if (m_lastIV.contains(inst0)) ivText = fmt2(m_lastIV.value(inst0));
        }
        ui->valueIV->setText(ivText);

        updateExpiryActivityTable();

        // ピンマップ（0.5〜1秒に1回程度にスロットル）
        if (++m_pinMapTick >= 2) {
            updatePinMapTable();
            m_pinMapTick = 0;
        }

        // 満期カーブ（GEX/Vanna/Charm）も1〜2秒に1回
        if (++m_curvesTick >= 2) {
            updateCurvesTables();
            updateCurvesCharts();
            m_curvesTick = 0;
        }

        statusBar()->showMessage(QString("Δ-Vol 1分 %1 | 5分 %2 | 代表IV %3 | 大口閾値 %4枚")
            .arg(fmt2(d1m)).arg(fmt2(d5m)).arg(ivText).arg(currentBigUnit()));
        });
    m_uiTick.start(1000);
}

MainWindow::~MainWindow() { delete ui; }

/* ================= UI配線 ================= */

void MainWindow::hookUiActions() {
    // 満期選択（表示フィルタ）：変えたらテーブル再構築
    connect(ui->comboExpiry, &QComboBox::currentIndexChanged, this, [this](int) {
        rebuildSignalTableFromResidual();
        });
    // マネネス帯変更 → 購読の対象を取り直すだけ（表示はAllのまま）
    connect(ui->comboMoneyness, &QComboBox::currentTextChanged, this, [this] {
        if (!m_instruments.isEmpty() && m_underlyingPx > 0.0) { m_subscribedOnce = false; chooseAndSubscribe(); }
        });
    connect(ui->btnResubscribe, &QPushButton::clicked, this, [this] { m_subscribedOnce = false; chooseAndSubscribe(); });
    connect(ui->btnRefresh, &QPushButton::clicked, this, [this] {
        QJsonObject p; p["currency"] = "BTC"; p["kind"] = "option"; p["expired"] = false;
        m_idGetInstruments = m_ws->call("public/get_instruments", p);
        });
    connect(ui->btnClearTape, &QPushButton::clicked, this, [this] { ui->plainTextEdit->clear(); });
    connect(ui->btnSaveTape, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, "テープを保存", "", "Text (*.txt)");
        if (path.isEmpty()) return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { QTextStream ts(&f); ts << ui->plainTextEdit->toPlainText(); }
        });

    if (auto* btn = findChild<QPushButton*>("btnBackfill")) {
        connect(btn, &QPushButton::clicked, this, &MainWindow::onBackfillClicked);
    }

    // 大口閾値を変えたら再構築（小さすぎる行を隠す/出す）
    connect(ui->spinMinSize, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        rebuildSignalTableFromResidual();
        });
}

/* ================= WS / RPC ================= */

void MainWindow::bootstrapAuto() {
    ui->plainTextEdit->appendPlainText("[情報] WS接続完了。銘柄一覧とPERP価格を取得します。");

    QJsonObject p1; p1["currency"] = "BTC"; p1["kind"] = "option"; p1["expired"] = false;
    m_idGetInstruments = m_ws->call("public/get_instruments", p1);

    QJsonObject p2; p2["instrument_name"] = "BTC-PERPETUAL";
    m_idPerpTicker = m_ws->call("public/ticker", p2);
}

void MainWindow::onRpc(int id, const QJsonObject& reply) {
    if (!reply.contains("result")) return;
    const auto res = reply.value("result");

    if (id == m_idPerpTicker) {
        const auto o = res.toObject();
        const double idx = o.value("index_price").toDouble();
        const double last = o.value("last_price").toDouble();
        m_underlyingPx = (idx > 0.0 ? idx : last);
        ui->plainTextEdit->appendPlainText(QString("[情報] 参照価格: %1").arg(fmt2(m_underlyingPx)));
    }
    else if (id == m_idGetInstruments) {
        if (!res.isArray()) return;
        m_instruments = res.toArray();
        ui->plainTextEdit->appendPlainText(QString("[情報] 銘柄を取得: %1件").arg(m_instruments.size()));

        // inst→expiry
        m_instToExpiryMs.clear();
        QVector<qint64> exps; exps.reserve(m_instruments.size());
        for (const auto& v : m_instruments) {
            if (!v.isObject()) continue;
            const auto o = v.toObject();
            if (!o.value("is_active").toBool(true)) continue;
            const QString name = o.value("instrument_name").toString();
            const qint64  exp = (qint64)o.value("expiration_timestamp").toDouble();
            if (!name.isEmpty() && exp > 0) {
                m_instToExpiryMs.insert(name, exp);
                exps.push_back(exp);
            }
        }
        std::sort(exps.begin(), exps.end());
        exps.erase(std::unique(exps.begin(), exps.end()), exps.end());
        m_nearestExpiryMs = exps.isEmpty() ? 0 : exps.front();

        populateExpiryChoices();                 // ここで先頭に All を入れる
        ui->comboExpiry->setCurrentIndex(0);     // 表示フィルタは All

        const bool restored = loadSnapshot();
        // 先に直近7日(または前回停止点→今)の差分を回す → 右画面がすぐ埋まる
        ui->plainTextEdit->appendPlainText("[情報] 直近差分の取り込みを開始します（初回は過去7日）。");
        autoBackfillDeltaInit();

        // その裏でフルバックフィルも走らせて、徐々に深掘り
        fullBackfillLiveExpiriesInit();


        updateExpiryActivityTable();
        requestOIAll();                           // OI は従来通り
        // ★ 初回のOI取得
    }

    if (m_underlyingPx > 0.0 && !m_instruments.isEmpty() && !m_subscribedOnce) {
        chooseAndSubscribe(); // 期近を購読
        m_subscribedOnce = true;
    }
}

/* ================= 受信（購読） ================= */

void MainWindow::handleDeribitMsg(const QJsonObject& obj) {
    const QString method = obj.value("method").toString();
    if (method != QStringLiteral("subscription")) return;

    const QJsonObject params = obj.value("params").toObject();
    const QString channel = params.value("channel").toString();
    const QJsonValue dataVal = params.value("data");

    // ---- trades.* ----
    if (channel.startsWith(QStringLiteral("trades."))) {
        const bool isGlobal = channel.startsWith(QStringLiteral("trades.option."));
        QJsonArray tradesArr;
        if (dataVal.isArray())       tradesArr = dataVal.toArray();
        else if (dataVal.isObject()) tradesArr = dataVal.toObject().value("trades").toArray();
        if (tradesArr.isEmpty()) return;

        for (const auto& v : tradesArr) {
            if (!v.isObject()) continue;
            const QJsonObject t = v.toObject();

            const QString tradeId = t.value("trade_id").toVariant().toString();
            const QString inst = t.value("instrument_name").toString();
            const double  amount = t.value("amount").toDouble();
            const double  price = t.value("price").toDouble();
            const qint64  ts = (qint64)t.value("timestamp").toDouble();
            const QString dir = t.value("direction").toString();
            const int     sign = (dir.compare("buy", Qt::CaseInsensitive) == 0) ? +1 : -1;

            if (!tradeId.isEmpty() && alreadySeenTrade(tradeId, ts)) continue;

            const double delta = m_lastDelta.value(inst, 0.0);

            // ★ Auto用サンプルは必ず記録（小口でも）
            pushAmtSample(ts, std::fabs(amount));

            // 全約定ログ（表示は出すが小口は以降スキップ）
            {
                const auto dtStr = QDateTime::fromMSecsSinceEpoch(ts).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
                ui->plainTextEdit->appendPlainText(
                    QString("[約定] %1  %2  %3  amt=%4  @%5")
                    .arg(dtStr).arg(inst).arg(dir)
                    .arg(QString::number(amount, 'f', 3))
                    .arg(QString::number(price, 'f', 3)));
                if (!isBigTrade(amount)) continue;  // ★小口はここで切る
            }

            // 満期アクティビティ
            recordExpiryEvent(inst, ts, amount, sign, delta);

            // 残存へ反映（tradePx=price を渡す）
            applyTradeToResidual(inst, ts, amount, sign, delta, price);

            // ★ 逆算IV（価格・残存分から求める）→ tradeIV と m_lastIV 埋め
            {
                const qint64 expMs = expiryFromInst(inst);
                const qint64 minLeft = std::max<qint64>(expMs - ts, 0) / 60000ll;
                if (price > 0.0 && minLeft > 0 && m_underlyingPx > 0.0) {
                    const double K = strikeFromInst(inst);
                    const bool   isCall = isCallFromInst(inst);
                    const auto   gk = IVGreeks::solveAndGreeks(
                        isCall ? OptionCP::Call : OptionCP::Put,
                        price, m_underlyingPx, K, double(minLeft), 0.0, 0.0);
                    if (gk.iv > 0.0) {
                        // 代表IVが未設定なら埋める
                        if (m_lastIV.value(inst, 0.0) <= 0.0) m_lastIV[inst] = gk.iv;
                    }
                }
                // 代表IVが未だ無ければ、オンデマンドで取りに行く
                if (m_lastIV.value(inst, 0.0) <= 0.0) queueIV(inst);
            }


            // ★ レッグ明細の保存（NBBO/Aggressor 付き、大口のみ）
            if (isBigTrade(amount)) {
                const bool   isCall = isCallFromInst(inst);
                const double k = strikeFromInst(inst);
                const qint64 expMs = expiryFromInst(inst);
                const QString key = makeClusterKey(expMs, isCall, k);

                double bpDiff = 0.0;
                Aggressor ag = m_nbbo.inferAggressor(inst, price, &bpDiff);
                const auto nb = m_nbbo.get(inst);
                const double mid = nb.mid();

                // 推定Δ：無い/0なら距離から補完
                double dAbs = std::abs(delta);
                if (dAbs <= 1e-9) dAbs = absDeltaGuess(k, m_underlyingPx);

                LegDetail lg;
                lg.ts = ts;
                lg.linkKey = key;
                lg.inst = inst;
                lg.sign = sign;
                lg.amount = std::abs(amount);
                lg.estDelta = dAbs;
                lg.price = price;

                lg.aggressor = ag;
                lg.venue = "Deribit";
                lg.expiryMs = expMs;
                lg.strike = k;
                lg.isCall = isCall;

                lg.nbboBid = nb.bid;
                lg.nbboAsk = nb.ask;
                lg.mid = mid;
                lg.bpDiffBp = bpDiff;

                // Trade IV 優先順位: 逆算IV > payload(iv) > 代表IV
                {
                    double ivSolve = 0.0;
                    const qint64 expMs2 = lg.expiryMs;
                    const qint64 minLeft = std::max<qint64>(expMs2 - lg.ts, 0) / 60000ll;
                    if (lg.price > 0.0 && minLeft > 0 && m_underlyingPx > 0.0 && lg.strike > 0.0) {
                        const auto gk = IVGreeks::solveAndGreeks(
                            lg.isCall ? OptionCP::Call : OptionCP::Put,
                            lg.price, m_underlyingPx, lg.strike, double(minLeft), 0.0, 0.0);
                        ivSolve = gk.iv;
                    }
                    double ivPayload = t.value("iv").toDouble();
                    double ivRep = m_lastIV.value(inst, 0.0);
                    if (ivSolve > 0.0)       lg.tradeIV = ivSolve;
                    else if (ivPayload > 0.) lg.tradeIV = ivPayload;
                    else                     lg.tradeIV = ivRep;
                }


                lg.orderId = tradeId;

                auto& vec = m_legsByKey[key];
                vec.push_back(lg);
                // メモリ上限（直近200件だけ保持）
                if (vec.size() > 200) vec.remove(0, vec.size() - 200);
            }


            // 個別購読のみ短期Δ集計とバースト
            if (!isGlobal) {
                if (!ui->chkPauseTape->isChecked()) {
                    ui->plainTextEdit->appendPlainText(
                        QString("[TAPE] %1  %2  amt=%3  @%4  d~%5")
                        .arg(inst, 12).arg(dir, 4)
                        .arg(QString::number(amount, 'f', 3))
                        .arg(QString::number(price, 'f', 3))
                        .arg(QString::number(delta, 'f', 3)));
                }
                addEvent(TradeEvent{ ts, amount, delta, sign, inst });
            }
        }
        return;
    }

    // ---- ticker.* ----
    if (channel.startsWith(QStringLiteral("ticker."))) {
        if (!dataVal.isObject()) return;
        const QJsonObject d = dataVal.toObject();
        const QString inst = d.value("instrument_name").toString();
        if (!inst.isEmpty()) {
            const QJsonObject greeks = d.value("greeks").toObject();
            if (!greeks.isEmpty()) m_lastDelta[inst] = greeks.value("delta").toDouble();
            if (d.contains("mark_iv")) m_lastIV[inst] = d.value("mark_iv").toDouble();

            // ★ NBBO配線（best bid/ask が来る）
            const double bid = d.value("best_bid_price").toDouble();
            const double ask = d.value("best_ask_price").toDouble();
            if (bid > 0.0 && ask > 0.0 && ask >= bid) {
                m_nbbo.update(inst, bid, ask);
            }
        }

        return;
    }
}

/* ================= 満期一覧 / 購読 ================= */

void MainWindow::populateExpiryChoices() {
    ui->comboExpiry->clear();
    ui->comboExpiry->addItem("All", QVariant::fromValue<qint64>(0)); // 先頭に All

    QVector<qint64> exps; exps.reserve(m_instruments.size());
    for (const auto& v : m_instruments) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        if (!o.value("is_active").toBool(true)) continue;
        const qint64 exp = (qint64)o.value("expiration_timestamp").toDouble();
        if (exp > 0) exps.push_back(exp);
    }
    std::sort(exps.begin(), exps.end());
    exps.erase(std::unique(exps.begin(), exps.end()), exps.end());

    for (qint64 ms : exps) {
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms).toLocalTime();
        ui->comboExpiry->addItem(dt.toString("yyyy-MM-dd HH:mm"), QVariant::fromValue(ms));
    }
}

qint64 MainWindow::selectedExpiryMs() const {
    const QVariant v = ui->comboExpiry->currentData();
    return v.isValid() ? v.toLongLong() : 0;
}
qint64 MainWindow::displayExpiryFilterMs() const { return selectedExpiryMs(); }

double MainWindow::currentMoneynessBand() const {
    const QString sel = ui->comboMoneyness->currentText();
    if (sel.contains("10")) return 0.10;
    if (sel.contains("20")) return 0.20;
    return std::numeric_limits<double>::infinity();
}

void MainWindow::chooseAndSubscribe() {
    if (m_underlyingPx <= 0.0 || m_instruments.isEmpty()) {
        ui->plainTextEdit->appendPlainText("[警告] chooseAndSubscribe: price or instruments missing");
        return;
    }

    const double band = currentMoneynessBand();
    const double lo = std::isinf(band) ? 0.0 : m_underlyingPx * (1.0 - band);
    const double hi = std::isinf(band) ? std::numeric_limits<double>::infinity()
        : m_underlyingPx * (1.0 + band);

    // ★ 表示は All でも、購読は「期近」だけにする（負荷削減）
    const qint64 targetExp = (m_nearestExpiryMs > 0 ? m_nearestExpiryMs : std::numeric_limits<qint64>::max());

    struct Row { QString name; double strike; bool isCall; double dist; qint64 exp; };
    QVector<Row> calls, puts;

    for (const auto& v : m_instruments) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        if (!o.value("is_active").toBool(true)) continue;
        const qint64 exp = (qint64)o.value("expiration_timestamp").toDouble();
        if (exp != targetExp) continue;
        const QString name = o.value("instrument_name").toString();
        const double  k = o.value("strike").toDouble();
        const QString cp = o.value("option_type").toString();
        if (name.isEmpty() || k <= 0.0) continue;
        if (!(k >= lo && k <= hi)) continue;

        Row r{ name, k, (cp == "call"), std::abs(k - m_underlyingPx), exp };
        (r.isCall ? calls : puts).push_back(r);
    }
    auto byDist = [](const Row& a, const Row& b) { return a.dist < b.dist; };
    std::sort(calls.begin(), calls.end(), byDist);
    std::sort(puts.begin(), puts.end(), byDist);

    const int pickC = std::min(8, (int)calls.size());
    const int pickP = std::min(8, (int)puts.size());

    m_targetInstruments.clear();
    for (int i = 0; i < pickC; ++i) m_targetInstruments << calls[i].name;
    for (int i = 0; i < pickP; ++i) m_targetInstruments << puts[i].name;

    if (m_targetInstruments.isEmpty()) {
        ui->plainTextEdit->appendPlainText("[警告] フィルタ後に銘柄なし");
        return;
    }

    m_channels.clear();
    for (const auto& inst : m_targetInstruments) {
        m_channels << QString("ticker.%1.raw").arg(inst);
        m_channels << QString("trades.%1.raw").arg(inst);
    }
    m_ws->subscribe(m_channels);
    refreshWatchList();

    ui->plainTextEdit->appendPlainText(
        QString("[情報] %1 銘柄に対して %2 チャンネルを購読しました。")
        .arg(m_targetInstruments.size()).arg(m_channels.size()));
}

/* ================= 監視リスト ================= */

void MainWindow::refreshWatchList() {
    ui->listInstruments->clear();
    ui->listInstruments->addItems(m_targetInstruments);
}

void MainWindow::autoBackfillPump() {
    while (m_autoInflight < AUTO_MAX_INFLIGHT && !m_autoBackfillQueue.isEmpty()) {
        const QString inst = m_autoBackfillQueue.front();
        m_autoBackfillQueue.pop_front();
        ++m_autoInflight;
        requestBackfillAuto(inst, m_autoBackFromMs, m_autoBackToMs);
    }

    if (m_autoInflight == 0 && m_autoBackfillQueue.isEmpty() && !m_autoBackfillDone) {
        m_autoBackfillDone = true;
        ui->plainTextEdit->appendPlainText("[情報] 差分取り込みが完了しました。");
        storeBackfillWatermarkMs(m_autoBackToMs);      // ★ 追加：完了時点を保存
        rebuildSignalTableFromResidual();
        updateExpiryActivityTable();
    }

}
void MainWindow::requestBackfillAuto(const QString& inst, qint64 fromMs, qint64 toMs) {
    if (fromMs >= toMs) {
        ui->plainTextEdit->appendPlainText(
            QString("[DIFF][WARN] %1 範囲が不正: from=%2 to=%3").arg(inst).arg(fromMs).arg(toMs));
        m_autoInflight = std::max(0, m_autoInflight - 1);
        autoBackfillPump();
        return;
    }

    QUrl url("https://www.deribit.com/api/v2/public/get_last_trades_by_instrument_and_time");
    QUrlQuery q;
    q.addQueryItem("instrument_name", inst);
    q.addQueryItem("start_timestamp", QString::number(fromMs));
    q.addQueryItem("end_timestamp", QString::number(toMs));
    q.addQueryItem("include_old", "true");
    q.addQueryItem("count", "1000");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "BTC-Option-Viewer/1.0 (+Qt)");
    QNetworkReply* rep = m_net.get(req);
    rep->setProperty("inst", inst);
    rep->setProperty("fromMs", QVariant::fromValue(fromMs));
    rep->setProperty("toMs", QVariant::fromValue(toMs));

    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString inst = rep->property("inst").toString();
        const qint64 fromMs = rep->property("fromMs").toLongLong();
        const qint64 toMs = rep->property("toMs").toLongLong();

        if (rep->error() != QNetworkReply::NoError) {
            const auto fstr = QDateTime::fromMSecsSinceEpoch(fromMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            const auto tstr = QDateTime::fromMSecsSinceEpoch(toMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            ui->plainTextEdit->appendPlainText(
                QString("[DIFF][ERR] %1  %2 ～ %3 : %4")
                .arg(inst, fstr, tstr, rep->errorString()));
            rep->deleteLater();
            m_autoInflight = std::max(0, m_autoInflight - 1);
            autoBackfillPump();
            return;
        }

        QByteArray bytes = rep->readAll();
        rep->deleteLater();

        int n = 0;

        QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (doc.isObject()) {
            const QJsonObject res = doc.object().value("result").toObject();
            const QJsonArray  trades = res.value("trades").toArray();
            n = trades.size();

            for (const auto& v : trades) {
                if (!v.isObject()) continue;
                const QJsonObject t = v.toObject();
                const qint64 ts = qint64(t.value("timestamp").toDouble());
                const double amt = t.value("amount").toDouble();
                const QString dir = t.value("direction").toString();
                const int sign = (dir.compare("buy", Qt::CaseInsensitive) == 0) ? +1 : -1;
                const double px = t.value("price").toDouble();
                const double delta = m_lastDelta.value(inst, 0.0);

                pushAmtSample(ts, std::fabs(amt)); // Auto閾値サンプルは常に保持

                if (std::fabs(amt) >= backfillMinUnit(ui)) {
                    if (m_lastIV.value(inst, 0.0) <= 0.0) queueIV(inst);
                    recordExpiryEvent(inst, ts, amt, sign, delta);
                    applyTradeToResidual(inst, ts, amt, sign, delta, px);
                }
            }
        }
        else {
            const auto head = QString::fromUtf8(bytes.left(200)).replace('\n', ' ');
            ui->plainTextEdit->appendPlainText(
                QString("[DIFF][WARN] %1 JSON解釈失敗。head=%2").arg(inst, head));
        }

        ui->plainTextEdit->appendPlainText(QString("[DIFF] %1 : %2件").arg(inst).arg(n));
        m_autoInflight = std::max(0, m_autoInflight - 1);
        autoBackfillPump();
        });
}

void MainWindow::autoBackfillDeltaInit() {
    m_deltaQueue.clear();
    m_deltaInflight = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 初回(=スナップショット無し) は直近7日分だけ先に埋める
    m_deltaFromMs = (m_lastSnapshotTs > 0 ? m_lastSnapshotTs : now - 7ll * DAY_MS);
    m_deltaToMs = now;
    if (m_instruments.isEmpty() || m_deltaFromMs >= m_deltaToMs) {
        m_deltaDone = true;
        ui->plainTextEdit->appendPlainText("[情報] 差分取り込み: 取り込み対象なし。");
        return;
    }
    for (const auto& v : m_instruments) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        if (!o.value("is_active").toBool(true)) continue;
        const QString name = o.value("instrument_name").toString();
        if (!name.isEmpty()) m_deltaQueue.push_back(name);
    }
    ui->plainTextEdit->appendPlainText(QString("[情報] 差分取り込み: 銘柄=%1").arg(m_deltaQueue.size()));
    autoBackfillDeltaPump();
}

void MainWindow::autoBackfillDeltaPump() {
    while (m_deltaInflight < AUTO_MAX_INFLIGHT && !m_deltaQueue.isEmpty()) {
        const QString inst = m_deltaQueue.front(); m_deltaQueue.pop_front();
        ++m_deltaInflight;
        requestBackfillDelta(inst, m_deltaFromMs, m_deltaToMs);
    }
    if (m_deltaInflight == 0 && m_deltaQueue.isEmpty() && !m_deltaDone) {
        m_deltaDone = true;
        ui->plainTextEdit->appendPlainText("[情報] 差分取り込みが完了しました。");
        // 差分バックフィルの完了ウォーターマークを保存（次回の起動で“前回停止時＋今回分”を連結）
        storeBackfillWatermarkMs(m_deltaToMs);

        rebuildSignalTableFromResidual();
        updateExpiryActivityTable();
        updatePinMapTable();
        updateCurvesTables();
        updateCurvesCharts();
    }

}

void MainWindow::requestBackfillDelta(const QString& inst, qint64 fromMs, qint64 toMs) {
    if (fromMs >= toMs) {
        ui->plainTextEdit->appendPlainText(
            QString("[DIFF][WARN] %1 範囲が不正: from=%2 to=%3").arg(inst).arg(fromMs).arg(toMs));
        m_deltaInflight = std::max(0, m_deltaInflight - 1);
        autoBackfillDeltaPump();
        return;
    }

    QUrl url("https://www.deribit.com/api/v2/public/get_last_trades_by_instrument_and_time");
    QUrlQuery q;
    q.addQueryItem("instrument_name", inst);
    q.addQueryItem("start_timestamp", QString::number(fromMs));
    q.addQueryItem("end_timestamp", QString::number(toMs));
    q.addQueryItem("include_old", "true");
    q.addQueryItem("count", "1000");
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "BTC-Option-Viewer/1.0 (+Qt)");
    QNetworkReply* rep = m_net.get(req);
    rep->setProperty("inst", inst);
    rep->setProperty("fromMs", QVariant::fromValue(fromMs));
    rep->setProperty("toMs", QVariant::fromValue(toMs));

    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString inst = rep->property("inst").toString();
        const qint64 fromMs = rep->property("fromMs").toLongLong();
        const qint64 toMs = rep->property("toMs").toLongLong();

        if (rep->error() != QNetworkReply::NoError) {
            const auto fstr = QDateTime::fromMSecsSinceEpoch(fromMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            const auto tstr = QDateTime::fromMSecsSinceEpoch(toMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            ui->plainTextEdit->appendPlainText(
                QString("[DIFF][ERR] %1  %2 ～ %3 : %4")
                .arg(inst, fstr, tstr, rep->errorString()));
            rep->deleteLater();
            m_deltaInflight = std::max(0, m_deltaInflight - 1);
            autoBackfillDeltaPump();
            return;
        }

        QByteArray bytes = rep->readAll();
        rep->deleteLater();

        int n = 0;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (doc.isObject()) {
            const QJsonArray trades = doc.object().value("result").toObject().value("trades").toArray();
            n = trades.size();
            for (const auto& v : trades) {
                if (!v.isObject()) continue;
                const QJsonObject t = v.toObject();
                const qint64 ts = qint64(t.value("timestamp").toDouble());
                const double amt = t.value("amount").toDouble();
                pushAmtSample(ts, std::fabs(amt));                     // Auto閾値用
                if (std::fabs(amt) < backfillMinUnit(ui)) continue;    // 残存へは手動>0なら手動、Auto=0なら全件
                const QString dir = t.value("direction").toString();
                const int sign = (dir.compare("buy", Qt::CaseInsensitive) == 0) ? +1 : -1;
                const double px = t.value("price").toDouble();
                const double delta = m_lastDelta.value(inst, 0.0);

                if (m_lastIV.value(inst, 0.0) <= 0.0) queueIV(inst);
                recordExpiryEvent(inst, ts, amt, sign, delta);
                applyTradeToResidual(inst, ts, amt, sign, delta, px);
            }
        }
        else {
            const auto head = QString::fromUtf8(bytes.left(200)).replace('\n', ' ');
            ui->plainTextEdit->appendPlainText(
                QString("[DIFF][WARN] %1 JSON解釈失敗。head=%2").arg(inst, head));
        }

        ui->plainTextEdit->appendPlainText(QString("[DIFF] %1 : %2件").arg(inst).arg(n));
        m_deltaInflight = std::max(0, m_deltaInflight - 1);
        autoBackfillDeltaPump();
        });
}
// ===== ここから挿入：生存満期のフルバックフィル =====
struct FullTask { QString inst; qint64 fromMs; qint64 toMs; qint64 stepMs; };

void MainWindow::fullBackfillLiveExpiriesInit() {
    m_fullQueue.clear();
    m_fullInflight = 0;
    m_fullDone = false;

    if (m_instruments.isEmpty()) {
        ui->plainTextEdit->appendPlainText("[情報] フルバックフィル: 銘柄なし。");
        m_fullDone = true;
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 生存満期のみに限定
    QSet<QString> liveInst;
    for (const auto& v : m_instruments) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();
        if (!o.value("is_active").toBool(true)) continue;
        const qint64 exp = qint64(o.value("expiration_timestamp").toDouble());
        if (exp > now) {
            const QString name = o.value("instrument_name").toString();
            if (!name.isEmpty()) liveInst.insert(name);
        }
    }

    // 初期窓：6時間 → 自動調整
    const qint64 initialStep = 6ll * HOUR_MS;
    const qint64 endMs = now;

    for (const QString& inst : liveInst) {
        const qint64 exp = m_instToExpiryMs.value(inst, endMs);
        // 満期の120日前（早過ぎる空振り期間をスキップ）
        const qint64 beginMs = std::max<qint64>(0, std::min(endMs, exp) - 120ll * DAY_MS);
        m_fullQueue.push_back(FullTask{ inst, beginMs, endMs, initialStep });
    }

    ui->plainTextEdit->appendPlainText(
        QString("[情報] フルバックフィル開始（生存満期のみ）: 銘柄=%1").arg(m_fullQueue.size())
    );

    fullBackfillPump();
}
void MainWindow::fullBackfillPump() {
    while (m_fullInflight < AUTO_MAX_INFLIGHT && !m_fullQueue.empty()) { // std::deque -> empty()
        FullTask t = m_fullQueue.front();
        m_fullQueue.pop_front();

        const qint64 winStart = t.fromMs;
        const qint64 winEnd = std::min(t.fromMs + t.stepMs, t.toMs);

        ++m_fullInflight;
        requestBackfillWindow(t.inst, winStart, winEnd, t.stepMs);
    }

    if (m_fullInflight == 0 && m_fullQueue.empty() && !m_fullDone) { // std::deque -> empty()
        m_fullDone = true;
        ui->plainTextEdit->appendPlainText("[情報] フルバックフィルが完了しました。");
        rebuildSignalTableFromResidual();
        updateExpiryActivityTable();
        updatePinMapTable();
        updateCurvesTables();
        updateCurvesCharts();
    }
}
void MainWindow::requestBackfillWindow(const QString& inst, qint64 fromMs, qint64 toMs, qint64 stepMs) {
    QUrl url("https://www.deribit.com/api/v2/public/get_last_trades_by_instrument_and_time");
    QUrlQuery q;
    q.addQueryItem("instrument_name", inst);
    q.addQueryItem("start_timestamp", QString::number(fromMs));
    q.addQueryItem("end_timestamp", QString::number(toMs));
    q.addQueryItem("include_old", "true");
    q.addQueryItem("count", "1000");
    url.setQuery(q);

    QNetworkRequest req(url);
    QNetworkReply* rep = m_net.get(req);
    rep->setProperty("inst", inst);
    rep->setProperty("fromMs", QVariant::fromValue(fromMs));
    rep->setProperty("toMs", QVariant::fromValue(toMs));
    rep->setProperty("stepMs", QVariant::fromValue(stepMs));

    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString inst = rep->property("inst").toString();
        const qint64 fromMs = rep->property("fromMs").toLongLong();
        const qint64 toMs = rep->property("toMs").toLongLong();
        qint64 stepMs = rep->property("stepMs").toLongLong();

        // 1) ネットワークエラー検知（詰まり防止）
        if (rep->error() != QNetworkReply::NoError) {
            const auto fstr = QDateTime::fromMSecsSinceEpoch(fromMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            const auto tstr = QDateTime::fromMSecsSinceEpoch(toMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            ui->plainTextEdit->appendPlainText(
                QString("[FULL][ERR] %1  %2 ～ %3 : %4")
                .arg(inst, fstr, tstr, rep->errorString())
            );
            rep->deleteLater();
            // 同じ窓を後ろに回して再試行（混雑回避）。step はそのまま。
            m_fullQueue.push_back(FullTask{ inst, fromMs, toMs, stepMs });
            m_fullInflight = std::max(0, m_fullInflight - 1);
            fullBackfillPump();
            return;
        }

        QByteArray bytes = rep->readAll();
        rep->deleteLater();

        int n = 0;
        qint64 lastTsSeen = -1;

        // 2) JSON パース
        QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            const QJsonObject res = root.value("result").toObject();
            const QJsonArray  trades = res.value("trades").toArray();

            n = trades.size();
            for (const auto& v : trades) {
                if (!v.isObject()) continue;
                const QJsonObject t = v.toObject();
                const qint64 ts = qint64(t.value("timestamp").toDouble());
                const double amt = t.value("amount").toDouble();
                const QString dir = t.value("direction").toString();
                const int sign = (dir.compare("buy", Qt::CaseInsensitive) == 0) ? +1 : -1;
                const double px = t.value("price").toDouble();
                const double delta = m_lastDelta.value(inst, 0.0);

                // Auto 閾値サンプルは常に保持
                pushAmtSample(ts, std::fabs(amt));
                // 残存への格納（手動>0なら手動、Auto=0なら1枚保持）
                if (std::fabs(amt) >= backfillMinUnit(ui)) {
                    if (m_lastIV.value(inst, 0.0) <= 0.0) queueIV(inst);
                    recordExpiryEvent(inst, ts, amt, sign, delta);
                    applyTradeToResidual(inst, ts, amt, sign, delta, px);
                }

                if (ts > lastTsSeen) lastTsSeen = ts;
            }
        }
        else {
            // パース失敗（レスポンス先頭 200 文字だけダンプ）
            const auto head = QString::fromUtf8(bytes.left(200)).replace('\n', ' ');
            const auto fstr = QDateTime::fromMSecsSinceEpoch(fromMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            const auto tstr = QDateTime::fromMSecsSinceEpoch(toMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            ui->plainTextEdit->appendPlainText(
                QString("[FULL][WARN] %1  %2 ～ %3 : JSON解釈失敗。head=%4")
                .arg(inst, fstr, tstr, head)
            );
        }

        // 3) 進捗ログ（Qt の %n 置換に統一）
        {
            const auto fstr = QDateTime::fromMSecsSinceEpoch(fromMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            const auto tstr = QDateTime::fromMSecsSinceEpoch(toMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            const double stepH = double(stepMs) / 3600000.0;
            ui->plainTextEdit->appendPlainText(
                QString("[FULL] %1  %2 ～ %3 : %4件 (step=%5h)")
                .arg(inst, fstr, tstr)
                .arg(n)
                .arg(QString::number(stepH, 'f', 1))
            );
        }

        // 4) 窓サイズの自動調整（疑似ページング）
        if (n >= 1000) {
            stepMs = std::max<qint64>(5ll * 60 * 1000, stepMs / 2); // 下限5分
        }
        else if (n < 800) {
            const qint64 maxStep = 24ll * HOUR_MS;
            stepMs = std::min(maxStep, (qint64)(stepMs * 3 / 2));
        }

        // 5) 次窓の enqueue（0件でも必ず前進）
        //    - 通常: lastTsSeen+1 から toMs
        //    - lastTs が取れない（n=0 等）: fromMs+step から toMs
        qint64 resumeFrom = (lastTsSeen >= 0) ? (lastTsSeen + 1)
            : std::min(toMs, fromMs + stepMs + 1);
        if (resumeFrom < toMs) {
            m_fullQueue.push_front(FullTask{ inst, resumeFrom, toMs, stepMs });
        }

        // 6) カウンタ調整＆次の実行
        m_fullInflight = std::max(0, m_fullInflight - 1);
        updateExpiryActivityTable();  // 右下の集計列が止まらないよう随時更新
        fullBackfillPump();
        }); // ← connect の閉じカッコ

} 

/* ================= 手動バックフィル（監視銘柄のみ） ================= */

void MainWindow::onBackfillClicked() {
    if (m_targetInstruments.isEmpty()) { QMessageBox::information(this, "履歴取り込み", "先に購読銘柄を選んでください。"); return; }

    m_events.clear();
    m_bursts.clear();

    int hours = 24;  // UI部品がなければ既定24h
    if (auto* sp = findChild<QSpinBox*>("spinBackHours")) {
        hours = sp->value();
    }
    m_backToMs = QDateTime::currentMSecsSinceEpoch();
    m_backFromMs = m_backToMs - qint64(hours) * HOUR_MS;

    ui->plainTextEdit->appendPlainText(
        QString("[情報] 履歴取り込み準備（%1時間, %2銘柄）: 先にΔ/IVを取得します。")
        .arg(hours).arg(m_targetInstruments.size()));

    prefetchTickersForTargets();
}

void MainWindow::prefetchTickersForTargets() {
    m_pendingTickers = 0;
    for (const QString& inst : m_targetInstruments) requestTickerFor(inst);
    if (m_pendingTickers == 0) {
        for (const QString& inst : m_targetInstruments)
            requestBackfillFor(inst, m_backFromMs, m_backToMs);
    }
}

void MainWindow::requestTickerFor(const QString& inst) {
    QUrl url("https://www.deribit.com/api/v2/public/ticker");
    QUrlQuery q; q.addQueryItem("instrument_name", inst); url.setQuery(q);
    QNetworkRequest req(url);

    QNetworkReply* rep = m_net.get(req);
    m_pendingTickers++;
    rep->setProperty("inst", inst);

    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString inst = rep->property("inst").toString();
        QByteArray bytes = rep->readAll(); rep->deleteLater();

        QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (doc.isObject()) {
            const QJsonObject res = doc.object().value("result").toObject();
            const QJsonObject greeks = res.value("greeks").toObject();
            if (!greeks.isEmpty()) m_lastDelta[inst] = greeks.value("delta").toDouble();
            if (res.contains("mark_iv")) m_lastIV[inst] = res.value("mark_iv").toDouble();
        }
        if (--m_pendingTickers == 0) {
            ui->plainTextEdit->appendPlainText("[情報] Δ/IVの取得完了。約定履歴を取り込みます。");
            m_backfillPending = 0;
            for (const QString& s : m_targetInstruments)
                requestBackfillFor(s, m_backFromMs, m_backToMs);
        }
        });
}

void MainWindow::requestBackfillFor(const QString& inst, qint64 fromMs, qint64 toMs) {
    QUrl url("https://www.deribit.com/api/v2/public/get_last_trades_by_instrument_and_time");
    QUrlQuery q;
    q.addQueryItem("instrument_name", inst);
    q.addQueryItem("start_timestamp", QString::number(fromMs));
    q.addQueryItem("end_timestamp", QString::number(toMs));
    q.addQueryItem("include_old", "true");
    q.addQueryItem("count", "1000");
    url.setQuery(q);

    QNetworkRequest req(url);
    QNetworkReply* rep = m_net.get(req);
    m_backfillPending++;
    rep->setProperty("inst", inst);

    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString inst = rep->property("inst").toString();
        QByteArray bytes = rep->readAll(); rep->deleteLater();

        const QJsonDocument doc = QJsonDocument::fromJson(bytes);
        int added = 0;
        if (doc.isObject()) {
            const QJsonArray trs = doc.object().value("result").toObject().value("trades").toArray();
            for (const auto& v : trs) {
                if (!v.isObject()) continue;
                const QJsonObject t = v.toObject();
                const qint64 ts = (qint64)t.value("timestamp").toDouble();
                const double amt = t.value("amount").toDouble();
                // ★ Auto用サンプルは必ず記録
                pushAmtSample(ts, std::fabs(amt));
                if (std::fabs(amt) < backfillMinUnit(ui)) continue;  // 手動>0なら手動、Auto時は全件
                const QString dir = t.value("direction").toString();
                const int sign = (dir.compare("buy", Qt::CaseInsensitive) == 0) ? +1 : -1;
                const double delta = m_lastDelta.value(inst, 0.0);

                const double px = t.value("price").toDouble();

                // 逆算IV → m_lastIV を温める（ない時のみ）
                {
                    const qint64 expMs = expiryFromInst(inst);
                    const qint64 minLeft = std::max<qint64>(expMs - ts, 0) / 60000ll;
                    if (px > 0.0 && minLeft > 0 && m_underlyingPx > 0.0) {
                        const double K = strikeFromInst(inst);
                        const bool   isCall = isCallFromInst(inst);
                        const auto   gk = IVGreeks::solveAndGreeks(
                            isCall ? OptionCP::Call : OptionCP::Put,
                            px, m_underlyingPx, K, double(minLeft), 0.0, 0.0);
                        if (gk.iv > 0.0 && m_lastIV.value(inst, 0.0) <= 0.0) {
                            m_lastIV[inst] = gk.iv;
                        }
                    }
                    if (m_lastIV.value(inst, 0.0) <= 0.0) queueIV(inst);
                }


                addEvent(TradeEvent{ ts, amt, delta, sign, inst });
                applyTradeToResidual(inst, ts, amt, sign, delta, px);
                ++added;

                // （任意）バックフィルでもレッグ明細を復元したい場合は以下を有効化
                {
                    const bool   isCall = isCallFromInst(inst);
                    const double k = strikeFromInst(inst);
                    const qint64 expMs2 = expiryFromInst(inst);
                    const QString key = makeClusterKey(expMs2, isCall, k);

                    double bpDiff = 0.0;
                    Aggressor ag = m_nbbo.inferAggressor(inst, px, &bpDiff);
                    const auto nb = m_nbbo.get(inst);
                    const double mid = nb.mid();

                    double dAbs = std::abs(delta);
                    if (dAbs <= 1e-9) dAbs = absDeltaGuess(k, m_underlyingPx);

                    LegDetail lg;
                    lg.ts = ts;
                    lg.linkKey = key;
                    lg.inst = inst;
                    lg.sign = sign;
                    lg.amount = std::abs(amt);
                    lg.estDelta = dAbs;
                    lg.price = px;

                    lg.aggressor = ag;
                    lg.venue = "Deribit";
                    lg.expiryMs = expMs2;
                    lg.strike = k;
                    lg.isCall = isCall;

                    lg.nbboBid = nb.bid;
                    lg.nbboAsk = nb.ask;
                    lg.mid = mid;
                    lg.bpDiffBp = bpDiff;

                    lg.tradeIV = t.value("iv").toDouble();
                    if (lg.tradeIV <= 0.0) lg.tradeIV = m_lastIV.value(inst, 0.0);

                    lg.orderId = t.value("trade_id").toVariant().toString();

                    auto& vec = m_legsByKey[key];
                    vec.push_back(lg);
                    if (vec.size() > 200) vec.remove(0, vec.size() - 200);
                }

            }
        }
        ui->plainTextEdit->appendPlainText(QString("[情報] 履歴取り込み %1: %2件").arg(inst).arg(added));

        if (--m_backfillPending == 0) {
            ui->plainTextEdit->appendPlainText("[情報] 履歴取り込み完了。サマリ更新。");
            rebuildSignalTableFromResidual();
        }
        });
}

/* ================= 短期集計 ================= */

void MainWindow::addEvent(const TradeEvent& ev) {
    m_events.append(ev);
    pruneOld(ev.tsMs);
    onNewTradeForBurst(ev);
}

void MainWindow::pruneOld(qint64 nowMs) {
    while (!m_events.isEmpty()) {
        const auto& head = m_events.first();
        if (nowMs - head.tsMs > FIVE_MIN_MS) m_events.removeFirst();
        else break;
    }
}

bool MainWindow::isBigTrade(double amount) const {
    const int unit = currentBigUnit();  // ← メンバー版Auto閾値を使用
    return std::fabs(amount) >= double(unit);
}

double MainWindow::sumDeltaVolume(qint64 nowMs, int windowMs) const {
    double s = 0.0;
    for (const auto& e : m_events)
        if (nowMs - e.tsMs <= windowMs) s += double(e.sign) * e.amount * e.delta;
    return s;
}

/* ================= 満期アクティビティ ================= */

qint64 MainWindow::expiryFromInst(const QString& inst) const {
    auto it = m_instToExpiryMs.find(inst);
    if (it != m_instToExpiryMs.end()) return it.value();
    return 0;
}

bool MainWindow::alreadySeenTrade(const QString& tradeId, qint64 ts) {
    const qint64 cutoff = ts - DAY_MS;
    while (!m_seenTradeQueue.isEmpty() && m_seenTradeQueue.front().first < cutoff) {
        auto old = m_seenTradeQueue.front(); m_seenTradeQueue.pop_front();
        m_seenTradeIds.remove(old.second);
    }
    if (m_seenTradeIds.contains(tradeId)) return true;
    m_seenTradeIds.insert(tradeId);
    m_seenTradeQueue.push_back(qMakePair(ts, tradeId));
    return false;
}

void MainWindow::recordExpiryEvent(const QString& inst, qint64 ts, double amount, int /*sign*/, double /*delta*/) {
    const qint64 expMs = expiryFromInst(inst);
    if (expMs <= 0) return;
    auto& vec = m_expiryEvents[expMs];
    vec.push_back(MiniEv{ ts, std::abs(amount), 0.0 });

    // 全期間集計できるように長期保持（上限は安全のため最大365日）
    static constexpr qint64 ACTIVITY_KEEP_MS = 365ll * DAY_MS;
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - ACTIVITY_KEEP_MS;
    while (!vec.isEmpty() && vec.front().ts < cutoff) vec.removeFirst();

}

void MainWindow::updateExpiryActivityTable() {
    if (!ui->tableExpiryActivity) return;

    QVector<qint64> exps; exps.reserve(m_instToExpiryMs.size());
    for (auto it = m_instToExpiryMs.begin(); it != m_instToExpiryMs.end(); ++it) exps.push_back(it.value());
    std::sort(exps.begin(), exps.end());
    exps.erase(std::unique(exps.begin(), exps.end()), exps.end());

    struct Row { qint64 exp; double qall; double q24; double q1; };
    QVector<Row> rows; rows.reserve(exps.size());

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (qint64 exp : exps) {
        const auto& vec = m_expiryEvents.value(exp);
        double qall = 0.0, q24 = 0.0, q1 = 0.0;
        for (const auto& e : vec) {
            qall += e.qty;                         // 全期間（保持期間内）
            if (now - e.ts <= DAY_MS)  q24 += e.qty;
            if (now - e.ts <= HOUR_MS) q1 += e.qty;
        }
        rows.push_back(Row{ exp, qall, q24, q1 });
    }

    ui->tableExpiryActivity->setSortingEnabled(false);
    ui->tableExpiryActivity->setRowCount(rows.size());

    int r = 0;
    for (const auto& row : rows) {
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(row.exp).toLocalTime();
        ui->tableExpiryActivity->setItem(r, 0, mkTextItem(dt.toString("yyyy-MM-dd HH:mm")));
        ui->tableExpiryActivity->setItem(r, 1, mkNumItem(row.qall, 1));
        ui->tableExpiryActivity->setItem(r, 2, mkNumItem(row.q24, 1));
        ui->tableExpiryActivity->setItem(r, 3, mkNumItem(row.q1, 1));
        ++r;
    }

    ui->tableExpiryActivity->setSortingEnabled(true);
    ui->tableExpiryActivity->sortItems(m_expActSortCol, m_expActSortOrder);
}

/* ================= シグナル：残存推定 ================= */

bool MainWindow::isCallFromInst(const QString& inst) { return inst.endsWith("-C", Qt::CaseInsensitive); }
double MainWindow::strikeFromInst(const QString& inst) {
    const QStringList p = inst.split('-');
    if (p.size() >= 4) {
        bool ok = false; double k = p[2].toDouble(&ok);
        if (ok) return k;
    }
    return 0.0;
}

QString MainWindow::makeClusterKey(qint64 expMs, bool isCall, double strike) const {
    const int kRound = int(std::round(strike / K_BUCKET) * K_BUCKET);
    return QString("%1|%2|%3").arg(expMs).arg(isCall ? 1 : 0).arg(kRound);
}

QPair<double, double> MainWindow::residualForKey(const QString& key) const {
    const double q = m_residualQtyByKey.value(key, 0.0);
    const double dv = m_residualDVolByKey.value(key, 0.0);
    return qMakePair(q, dv);
}

void MainWindow::applyTradeToResidual(const QString& inst, qint64 ts,
    double amount, int sign, double deltaRaw, double /*tradePx*/) {
    const qint64 exp = expiryFromInst(inst);
    if (exp <= 0) return;
    if (!isBigTrade(amount)) return;

    const bool   isCall = isCallFromInst(inst);
    const double k = strikeFromInst(inst);
    if (k <= 0.0) return;

    const QString key = makeClusterKey(exp, isCall, k);

    // 残存枚数（買い:+ / 売り:-）。★下限を設けない：売りの“仕込み”は負で保持する
    double& qty = m_residualQtyByKey[key];
    qty += (sign > 0 ? std::abs(amount) : -std::abs(amount));

    // ★ 買い/売りのネット（買い:+ 売り:-）…表で「買い連続/売り連続」を正しく出すために使う
    double& signedQty = m_residualSignedQtyByKey[key];
    signedQty += (sign > 0 ? +1.0 : -1.0) * std::abs(amount);

    // Δ：無い/0なら距離から推定。符号は必ず Call=＋ / Put=−
    double dAbs = std::abs(deltaRaw);
    if (dAbs <= 1e-9) dAbs = absDeltaGuess(k, m_underlyingPx);
    const double deltaSigned = isCall ? +dAbs : -dAbs;

    // dVol = 約定方向(買い:+ / 売り:-) × 枚数 × (符号付きΔ)
    double& dv = m_residualDVolByKey[key];
    dv += (sign > 0 ? +1.0 : -1.0) * std::abs(amount) * deltaSigned;

    // 付帯
    m_residualLastTsByKey[key] = std::max(m_residualLastTsByKey.value(key, 0ll), ts);
    m_residualTradesByKey[key] = m_residualTradesByKey.value(key, 0) + 1;
    m_residualInstsByKey[key].insert(inst);

    // 既存行があれば即時更新（推定Δは |dVol|/qty）
    const int row = findRowByKey(key);
    if (row >= 0) {
        const double qAbs = std::abs(qty);
        const double absDVol = std::abs(dv);
        const double notionalUSD = (m_underlyingPx > 0.0) ? (qAbs * m_underlyingPx) : 0.0;
        const double avgAbsDelta = (qAbs > 1e-12 ? absDVol / qAbs : 0.0);



        const qint64 anchorTs = m_signalAnchorTsByKey.value(key, m_residualLastTsByKey[key]);
        const QString show = QDateTime::fromMSecsSinceEpoch(anchorTs).toLocalTime()
            .toString("yy/MM/dd HH:mm:ss");
        auto* titem = mkTimeItem(anchorTs, show);
        titem->setData(Qt::UserRole, key);   // ← クラスタkeyを時刻セルに保持
        ui->tableSignals->setItem(row, 0, titem);
        {
            auto* it = new QTableWidgetItem;
            it->setData(Qt::EditRole, std::fabs(qty));                 // ← ソート用は abs
            it->setText(QString::number(qty, 'f', 1));                  // 表示は符号付き
            it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            ui->tableSignals->setItem(row, 5, it);
        }

        ui->tableSignals->setItem(row, 6, mkNumItem(avgAbsDelta, 2));
        ui->tableSignals->setItem(row, 7, mkNumItem(absDVol, 2));
        ui->tableSignals->setItem(row, 8, mkNumItemWithText(notionalUSD, fmtComma0(notionalUSD)));

        const int trades = m_residualTradesByKey.value(key, 0);
        const int uniq = m_residualInstsByKey.value(key).size();
        ui->tableSignals->setItem(row, 9, mkTextItem(QString("件数%1 / 銘柄%2").arg(trades).arg(uniq)));
    }

    // 1h列が止まらないよう、約定ごとに集計更新
    updateExpiryActivityTable();
}


bool MainWindow::passSignalFilter(qint64 expMs) const {
    const qint64 f = displayExpiryFilterMs(); // 0=All
    return (f == 0) || (f == expMs);
}

int MainWindow::findRowByKey(const QString& key) const {
    auto it = m_signalRowIndexByKey.find(key);
    if (it == m_signalRowIndexByKey.end()) return -1;
    const int row = it.value();
    if (row < 0 || row >= ui->tableSignals->rowCount()) return -1;
    return row;
}

void MainWindow::removeSignalRowIfExists(const QString& key) {
    const int row = findRowByKey(key);
    if (row >= 0) {
        ui->tableSignals->removeRow(row);
        m_signalRowIndexByKey.remove(key);
    }
}

void MainWindow::upsertSignalRow(const QString& key, qint64 expMs,
    const FlowBurst& snapshot, double residualQty,
    double absDVol, double avgAbsDelta, double notionalUSD)
{
    // 満期フィルタ
    if (!passSignalFilter(expMs)) { removeSignalRowIfExists(key); return; }

    // 大口閾値未満は非表示
    const double bigUnit = std::max(ui->spinMinSize->value(), 1.0);
    if (std::abs(residualQty) < bigUnit) { removeSignalRowIfExists(key); return; }

    const QString expShow = QDateTime::fromMSecsSinceEpoch(expMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
    const QString side = (snapshot.isBuy ? "買い" : "売り");
    const QString cp = (snapshot.isCall ? "Call" : "Put");
    const QString pat = QString("%1連続（%2）").arg(side, cp);

    const int trades = m_residualTradesByKey.value(key, snapshot.trades);
    const int uniq = m_residualInstsByKey.value(key, snapshot.instruments).size();

    const bool wasSorting = ui->tableSignals->isSortingEnabled();
    ui->tableSignals->setSortingEnabled(false);

    int row = findRowByKey(key);
    if (row < 0) {
        row = ui->tableSignals->rowCount();
        ui->tableSignals->insertRow(row);
        m_signalRowIndexByKey[key] = row;
    }

    // 0: 時刻（初回は startMs、無ければ lastMs。以後は固定）
    {
        const qint64 lastTsForKey = m_residualLastTsByKey.value(key, snapshot.lastMs);
        qint64 anchorTs = m_signalAnchorTsByKey.value(key, (snapshot.startMs > 0 ? snapshot.startMs : lastTsForKey));
        m_signalAnchorTsByKey.insert(key, anchorTs);
        const QString show = QDateTime::fromMSecsSinceEpoch(anchorTs).toLocalTime().toString("yy/MM/dd HH:mm:ss");
        auto* titem = mkTimeItem(anchorTs, show);
        titem->setData(Qt::UserRole, key);   // ← クラスタkeyを時刻セルに保持
        ui->tableSignals->setItem(row, 0, titem);

    }

    // 1: 満期
    ui->tableSignals->setItem(row, 1, mkTextItem(expShow));

    // 2: 方向（↑/↓、強ければ ↑↑/↓↓）
    {
        int dirSign = 0;
        if (std::abs(snapshot.dVolSum) > 1e-9) {
            dirSign = (snapshot.dVolSum >= 0.0) ? +1 : -1;   // ← dVolの符号がそのまま方向
        }
        else {
            // Δが完全に無いケースのみフォールバック
            const int cpSign = snapshot.isCall ? +1 : -1;    // Call=+1, Put=-1
            const int bsSign = snapshot.isBuy ? +1 : -1;    // 買い=+1, 売り=-1
            dirSign = (cpSign * bsSign >= 0) ? +1 : -1;
        }

        const int unit = currentBigUnit();
        const bool strong = (snapshot.qtySum >= double(unit) * 10.0) ||
            (std::abs(snapshot.dVolSum) >= double(unit) * 4.0);

        ui->tableSignals->setItem(row, 2, mkDirItem(dirSign, strong));
    }

    // 3: パターン
    ui->tableSignals->setItem(row, 3, mkTextItem(pat));
    // 4: 行使
    ui->tableSignals->setItem(row, 4, mkNumItem(std::round(snapshot.centerK), 0));
    // 5: 枚数（残存）→ ソートは abs
    {
        auto* it = new QTableWidgetItem;
        it->setData(Qt::EditRole, std::fabs(residualQty));         // ← abs でソート
        it->setText(QString::number(residualQty, 'f', 1));         // 表示は符号付き
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ui->tableSignals->setItem(row, 5, it);
    }
    // 6: 推定Δ
    ui->tableSignals->setItem(row, 6, mkNumItem(avgAbsDelta, 2));
    // 7: 強度（|Δ加重|）
    ui->tableSignals->setItem(row, 7, mkNumItem(absDVol, 2));
    // 8: 名目(USD) 表示はカンマ、EditRoleは数値
    {
        ui->tableSignals->setItem(row, 8, mkNumItemWithText(notionalUSD, fmtComma0(notionalUSD)));
    }
    // 9: 詳細
    ui->tableSignals->setItem(row, 9, mkTextItem(QString("件数%1 / 銘柄%2").arg(trades).arg(uniq)));
    ui->tableSignals->setSortingEnabled(wasSorting);
}


// バースト検出 → シグナル化（※残存に基づいて行は更新される）
void MainWindow::onNewTradeForBurst(const TradeEvent& ev) {
    const int bigUnit = currentBigUnit();
    const double fireQty = double(bigUnit) * 5.0;
    const double fireDVol = double(bigUnit) * 2.0;
    const bool   isCall = isCallFromInst(ev.instrument);
    const double k = strikeFromInst(ev.instrument);
    if (k <= 0.0) return;
    if (!isBigTrade(ev.amount)) return;

    // 符号付きΔ（取れないときは距離から推定）
    double dAbs = std::abs(ev.delta);
    if (dAbs <= 1e-9) dAbs = absDeltaGuess(k, m_underlyingPx);
    const double deltaSigned = isCall ? +dAbs : -dAbs;

    // 古いバースト掃除
    for (int i = m_bursts.size() - 1; i >= 0; --i)
        if (ev.tsMs - m_bursts[i].lastMs > BURST_WINDOW_MS) m_bursts.removeAt(i);

    // 近傍吸収
    const bool isBuy = (ev.sign > 0);
    int best = -1; double bestDist = 1e100;
    for (int i = 0; i < m_bursts.size(); ++i) {
        const auto& b = m_bursts[i];
        if (b.isBuy != isBuy || b.isCall != isCall) continue;
        if (std::abs(k - b.centerK) > STRIKE_CLUSTER_WIDTH) continue;
        if (ev.tsMs - b.lastMs > BURST_WINDOW_MS) continue;
        const double d = std::abs(k - b.centerK);
        if (d < bestDist) { bestDist = d; best = i; }
    }

    if (best < 0) {
        FlowBurst b;
        b.startMs = b.lastMs = ev.tsMs;
        b.isBuy = isBuy; b.isCall = isCall;
        b.centerK = k;
        b.qtySum = std::abs(ev.amount);
        b.dVolSum = ev.sign * std::abs(ev.amount) * deltaSigned;
        b.trades = 1;
        b.instruments.insert(ev.instrument);
        m_bursts.push_back(b);
        best = m_bursts.size() - 1;
    }
    else {
        auto& b = m_bursts[best];
        b.lastMs = ev.tsMs;
        const double wOld = std::max(1, b.trades);
        b.centerK = (b.centerK * wOld + k) / (wOld + 1);
        b.qtySum += std::abs(ev.amount);
        b.dVolSum += ev.sign * std::abs(ev.amount) * deltaSigned;
        b.trades += 1;
        b.instruments.insert(ev.instrument);
    }

    const auto& b = m_bursts[best];
    if (b.qtySum < bigUnit) return;

    if (b.qtySum >= fireQty || std::abs(b.dVolSum) >= fireDVol) {
        const qint64 expMs = expiryFromInst(*b.instruments.begin());

        // 30秒バケットで重複抑制（満期|Call/Put|k丸め|bucket）
        const qint64 bucket = b.lastMs / (30ll * 1000);
        const int kRound = int(std::round(b.centerK / K_BUCKET) * K_BUCKET);
        const QString key0 = QString("%1|%2|%3").arg(expMs).arg(b.isCall ? 1 : 0).arg(kRound);
        const QString key = key0 + "|" + QString::number(bucket);

        const qint64 cutoff = b.lastMs - SIGNAL_DEDUP_MS;
        while (!m_signalKeyQueue.isEmpty() && m_signalKeyQueue.front().first < cutoff) {
            auto old = m_signalKeyQueue.front(); m_signalKeyQueue.pop_front();
            m_signalKeys.remove(old.second);
        }
        if (!m_signalKeys.contains(key)) {
            m_signalKeys.insert(key);
            m_signalKeyQueue.push_back(qMakePair(b.lastMs, key));
            emitSignalRow(b, expMs); // 残存ベースで行を描画
        }
        m_bursts.removeAt(best);
    }
}


void MainWindow::emitSignalRow(const FlowBurst& b, qint64 expMs) {
    const QString key = makeClusterKey(expMs, b.isCall, b.centerK);
    const auto [qty, dvolNet] = residualForKey(key);
    const double qAbs = std::abs(qty);
    const double absDvol = std::abs(dvolNet);
    const double avgAbsDelta = (qAbs > 1e-12 ? absDvol / qAbs : 0.0);
    const double notionalUSD = (m_underlyingPx > 0.0 ? qAbs * m_underlyingPx : 0.0);


    // upsert（内部でフィルタ＆カンマ表示）
    upsertSignalRow(key, expMs, b, qty, absDvol, avgAbsDelta, notionalUSD);
}

/* ================= 残存から一括再構築 ================= */

void MainWindow::rebuildSignalTableFromResidual() {
    if (!ui->tableSignals) return;

    ui->tableSignals->setSortingEnabled(false);
    ui->tableSignals->setRowCount(0);
    m_signalRowIndexByKey.clear();

    const int bigUnit = currentBigUnit();
    const double bigUnitD = double(bigUnit);

    // key 形式: exp|isCall|k
    const auto keys = m_residualQtyByKey.keys();
    for (const QString& key : keys) {
        const QStringList p = key.split('|');
        if (p.size() != 3) continue;
        const qint64 expMs = p[0].toLongLong();
        if (!passSignalFilter(expMs)) continue;

        const double qty = m_residualQtyByKey.value(key, 0.0);
        if (std::abs(qty) < bigUnit) continue;

        const bool   isCall = (p[1].toInt() == 1);
        const double k = p[2].toDouble();
        const double dvolNet = m_residualDVolByKey.value(key, 0.0);
        const double qAbs = std::abs(qty);
        const double absDvol = std::abs(dvolNet);
        const double avgAbsDelta = (qAbs > 1e-12 ? absDvol / qAbs : 0.0);
        const double notionalUSD = (m_underlyingPx > 0.0 ? qAbs * m_underlyingPx : 0.0);

        FlowBurst snap;
        snap.startMs = 0; // 履歴からの復元時は不明。表示は lastTs を使う
        snap.lastMs = m_residualLastTsByKey.value(key, 0ll);
        // ネット残存 qty の符号で「買い/売り」を決める
        snap.isBuy = (qty >= 0.0);
        snap.isCall = isCall;
        snap.centerK = k;
        snap.dVolSum = dvolNet;
        snap.qtySum = qty;
        snap.trades = m_residualTradesByKey.value(key, 0);
        snap.instruments = m_residualInstsByKey.value(key);

        upsertSignalRow(key, expMs, snap, qty, absDvol, avgAbsDelta, notionalUSD);
    }

    ui->tableSignals->setSortingEnabled(true);
    // 既定は「時刻」降順表示にしておくと見やすい
    ui->tableSignals->sortItems(0, Qt::DescendingOrder);
}
// ==== legs: populate detail table for selected cluster key ====
void MainWindow::populateLegDetailsForKey(const QString& key)
{
    if (!m_tableLegs) return;

    m_tableLegs->setSortingEnabled(false);
    m_tableLegs->setRowCount(0);

    const auto& legs = m_legsByKey.value(key);
    if (legs.isEmpty()) {
        m_tableLegs->setSortingEnabled(true);
        return;
    }

    m_tableLegs->setRowCount(legs.size());
    int r = 0;
    for (const auto& lg : legs) {
        const QString show = QDateTime::fromMSecsSinceEpoch(lg.ts)
            .toLocalTime()
            .toString("yy/MM/dd HH:mm:ss");

        // 0: 時刻（DisplayRole=QDateTimeでソート安定）
        m_tableLegs->setItem(r, 0, mkTimeItem(lg.ts, show));
        // 1: LinkID（= クラスタkey）
        m_tableLegs->setItem(r, 1, mkTextItem(lg.linkKey));
        // 2: アグレッサ
        QString agtxt = "Unknown";
        switch (lg.aggressor) {
        case Aggressor::HitBid: agtxt = "HitBid"; break;
        case Aggressor::LiftAsk: agtxt = "LiftAsk"; break;
        case Aggressor::Mid: agtxt = "Mid"; break;
        case Aggressor::Outside: agtxt = "Outside"; break;
        default: break;
        }
        m_tableLegs->setItem(r, 2, mkTextItem(agtxt, Qt::AlignHCenter | Qt::AlignVCenter));
        // 3: Venue
        m_tableLegs->setItem(r, 3, mkTextItem(lg.venue));
        // 4: 銘柄
        m_tableLegs->setItem(r, 4, mkTextItem(lg.inst));
        // 5: Call/Put
        m_tableLegs->setItem(r, 5, mkTextItem(lg.isCall ? "Call" : "Put", Qt::AlignHCenter | Qt::AlignVCenter));
        // 6: 満期
        if (lg.expiryMs > 0) {
            const auto dt = QDateTime::fromMSecsSinceEpoch(lg.expiryMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            m_tableLegs->setItem(r, 6, mkTextItem(dt));
        }
        else {
            m_tableLegs->setItem(r, 6, mkTextItem("-"));
        }
        // 7: 行使
        m_tableLegs->setItem(r, 7, mkNumItem(lg.strike, 0));
        // 8: 数量
        m_tableLegs->setItem(r, 8, mkNumItem(lg.amount, 3));
        // 9: プレミアム
        m_tableLegs->setItem(r, 9, mkNumItem(lg.price, 6));
        // 10: 通貨
        m_tableLegs->setItem(r, 10, mkTextItem(lg.currency.isEmpty() ? "-" : lg.currency));
        // 11: 乗数M
        m_tableLegs->setItem(r, 11, mkNumItem(lg.multiplier, 2));
        // 12: 手数料
        m_tableLegs->setItem(r, 12, mkNumItem(lg.fee, 6));
        // 13: Trade IV
        m_tableLegs->setItem(r, 13, mkNumItem(lg.tradeIV, 4));
        // 14: NBBO Bid
        m_tableLegs->setItem(r, 14, mkNumItem(lg.nbboBid, 6));
        // 15: NBBO Ask
        m_tableLegs->setItem(r, 15, mkNumItem(lg.nbboAsk, 6));
        // 16: Mid
        m_tableLegs->setItem(r, 16, mkNumItem(lg.mid, 6));
        // 17: 乖離(bp)
        m_tableLegs->setItem(r, 17, mkNumItem(lg.bpDiffBp, 1));
        // 18: OrderID
        m_tableLegs->setItem(r, 18, mkTextItem(lg.orderId));

        ++r;
    }

    m_tableLegs->setSortingEnabled(true);
    m_tableLegs->sortItems(0, Qt::DescendingOrder);
}

void MainWindow::updatePinMapTable()
{
    auto* tbl = findChild<QTableWidget*>("tablePinMap");
    if (!tbl) return;
    if (m_underlyingPx <= 0.0) return;

    // モデル構築
    const auto rows = buildPinMap(
        m_residualQtyByKey,
        m_residualDVolByKey,
        m_underlyingPx,
        &m_oi,
        K_BUCKET
    );

    // 表示フィルタ（満期 All or 個別）
    const qint64 fexp = displayExpiryFilterMs();  // ← 追加

    // テーブル更新
    tbl->setSortingEnabled(false);
    tbl->setRowCount(0);
    int r = 0;
    for (const auto& x : rows) {
        if (fexp != 0 && x.expiryMs != fexp) continue;

        tbl->insertRow(r);
        const auto dt = QDateTime::fromMSecsSinceEpoch(x.expiryMs).toLocalTime().toString("yyyy-MM-dd HH:mm");

        // 0 満期
        tbl->setItem(r, 0, mkTextItem(dt));
        // 1 CP
        tbl->setItem(r, 1, mkTextItem(x.isCall ? "Call" : "Put", Qt::AlignHCenter | Qt::AlignVCenter));
        // 2 行使
        tbl->setItem(r, 2, mkNumItem(x.strike, 0));
        // 3 距離%
        tbl->setItem(r, 3, mkNumItem(x.distPct, 2));
        // 4 残存枚数
        tbl->setItem(r, 4, mkNumItem(x.residualQty, 1));
        // 5 残存dVol
        tbl->setItem(r, 5, mkNumItem(x.residualDVol, 2));
        // 6 OI
        tbl->setItem(r, 6, mkNumItem(x.oi, 0));
        // 7 Pin指数
        tbl->setItem(r, 7, mkNumItem(x.pinIndex, 2));

        ++r;
    }
    tbl->setSortingEnabled(true);
}

void MainWindow::updateCurvesTables()
{
    auto* tblG = findChild<QTableWidget*>("tableGexCurve");
    auto* tblV = findChild<QTableWidget*>("tableVannaCurve");
    auto* tblC = findChild<QTableWidget*>("tableCharmCurve");
    if (!tblG || !tblV || !tblC) return;
    if (m_underlyingPx <= 0.0) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // IV 取得関数（クラスタ内の代表銘柄に対する mark_iv）
    auto ivGetter = [this](const QString& inst) -> double {
        auto it = m_lastIV.find(inst);
        return (it != m_lastIV.end() ? it.value() : 0.0);
        };

    const auto rows = buildGreeksCurves(
        m_residualQtyByKey,
        m_residualInstsByKey,
        m_underlyingPx,
        now,
        ivGetter
    );

    const qint64 fexp = displayExpiryFilterMs(); // 0=All

    // ===== 表示行のスケールを見て「×10^n」をヘッダに付け、セルは割って表示 =====
    auto refresh = [&](QTableWidget* tbl, auto valueExtractor, const char* baseName) {
        // 1) スケール決定用：有限値だけを見る
        QVector<double> vals; vals.reserve(rows.size());
        for (const auto& x : rows) {
            if (fexp != 0 && x.expiryMs != fexp) continue;
            double v = fin(valueExtractor(x), std::numeric_limits<double>::quiet_NaN());
            if (std::isfinite(v)) vals.push_back(v);
        }
        EngScale sc = calcEngScale(vals);

        // 2) ヘッダ
        if (tbl->columnCount() < 2) tbl->setColumnCount(2);
        if (!tbl->horizontalHeaderItem(0)) tbl->setHorizontalHeaderItem(0, new QTableWidgetItem("満期"));
        if (!tbl->horizontalHeaderItem(1)) tbl->setHorizontalHeaderItem(1, new QTableWidgetItem());
        auto* h1 = tbl->horizontalHeaderItem(1);
        h1->setText(sc.e3 == 0 ? QString::fromLatin1(baseName)
            : QString("%1 (×10^%2)").arg(baseName).arg(sc.e3));
        h1->setToolTip("この列はヘッダ倍率でスケーリング表示（ソートは生値）");

        // 3) 行描画：非有限はスキップ（表示もソートも壊さない）
        tbl->setSortingEnabled(false);
        tbl->setRowCount(0);
        int r = 0;
        for (const auto& x : rows) {
            if (fexp != 0 && x.expiryMs != fexp) continue;
            double raw = fin(valueExtractor(x), std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(raw)) continue;

            tbl->insertRow(r);
            const auto dt = QDateTime::fromMSecsSinceEpoch(x.expiryMs).toLocalTime().toString("yyyy-MM-dd HH:mm");
            tbl->setItem(r, 0, mkTextItem(dt));
            tbl->setItem(r, 1, mkNumItemScaled(raw, sc, 3));
            ++r;
        }
        tbl->setSortingEnabled(true);
        // tbl->sortItems(1, Qt::DescendingOrder);
        };

    refresh(tblG, [](const CurveRow& x) { return x.netGamma; }, "GEX");
    refresh(tblV, [](const CurveRow& x) { return x.netVanna; }, "Vanna");
    refresh(tblC, [](const CurveRow& x) { return x.netCharm; }, "Charm");
}


void MainWindow::updateCurvesCharts() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto rows = buildGreeksCurves(
        m_residualQtyByKey,
        m_residualInstsByKey,
        m_underlyingPx,
        now,
        [this](const QString& inst) {
            auto it = m_lastIV.find(inst);
            return (it != m_lastIV.end() ? it.value() : 0.0);
        }
    );

    const qint64 fexp = displayExpiryFilterMs(); // 0=All

    // X=残存日, Y=値
    QList<QPointF> gammaPts, vegaPts;
    for (const auto& x : rows) {
        if (fexp != 0 && x.expiryMs != fexp) continue;
        const double days = (x.expiryMs - now) / 86400000.0;
        gammaPts.append(QPointF(days, x.netGamma));
        vegaPts.append(QPointF(days, x.netVega));
    }

    // Cumulative PnL: 受け皿（今は m_cumPnlValue を時系列に積む）
    const double tmin = (now - m_pnlStartMs) / 60000.0; // 分
    if (m_cumPnlPts.isEmpty() || tmin > m_cumPnlPts.back().x()) {
        m_cumPnlPts.append(QPointF(tmin, m_cumPnlValue));
        if (m_cumPnlPts.size() > 600) m_cumPnlPts.pop_front();
    }

    // グローバルの plotLine を呼ぶだけ
    plotLine(ui->viewGamma, gammaPts);
    plotLine(ui->viewVega, vegaPts);
    plotLine(ui->viewCumPnl, m_cumPnlPts, "%.0f", "%.2f");

    if (m_curvesPane) {
        m_curvesPane->setGammaPoints(gammaPts, QStringLiteral("Gamma (残存日)"));
        m_curvesPane->setVegaPoints(vegaPts, QStringLiteral("Vega (残存日)"));
        m_curvesPane->setCumulativePnLPoints(m_cumPnlPts, QStringLiteral("Cumulative PnL"));
    }
}

void MainWindow::requestOIAll()
{
    // Instruments 未取得ならスキップ
    if (m_instruments.isEmpty()) return;

    // Deribit: 全BTCオプションの book summary を一括取得（open_interest を含む）
    QUrl url("https://www.deribit.com/api/v2/public/get_book_summary_by_currency");
    QUrlQuery q;
    q.addQueryItem("currency", "BTC");
    q.addQueryItem("kind", "option");
    q.addQueryItem("expired", "false");
    url.setQuery(q);

    QNetworkRequest req(url);
    QNetworkReply* rep = m_net.get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QByteArray bytes = rep->readAll();
        rep->deleteLater();
        handleOIReply(bytes);
        });
}

void MainWindow::handleOIReply(const QByteArray& bytes)
{
    QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();
    const QJsonArray arr = root.value("result").toArray();
    if (arr.isEmpty()) return;

    int setCnt = 0;
    for (const auto& v : arr) {
        if (!v.isObject()) continue;
        const auto o = v.toObject();

        const QString inst = o.value("instrument_name").toString();
        if (inst.isEmpty()) continue;

        const double oi = o.value("open_interest").toDouble(); // 0可

        const qint64 expMs2 = expiryFromInst(inst);
        const double k = strikeFromInst(inst);
        const bool  isCall = isCallFromInst(inst);
        if (expMs2 <= 0 || k <= 0.0) continue;

        m_oi.setOI(expMs2, k, isCall, oi);
        ++setCnt;
    }

    if (setCnt > 0) updatePinMapTable();
}

void MainWindow::queueIV(const QString& inst)
{
    if (inst.isEmpty()) return;
    if (m_lastIV.value(inst, 0.0) > 0.0) return; // 既に保持
    if (m_ivQueued.contains(inst)) return;       // 去重
    m_ivQueued.insert(inst);
    m_ivQueue.enqueue(inst);
}

void MainWindow::pumpIV()
{
    if (m_ivInflight > 0) return;
    if (m_ivQueue.isEmpty()) return;

    const QString inst = m_ivQueue.dequeue();
    QUrl url("https://www.deribit.com/api/v2/public/ticker");
    QUrlQuery q; q.addQueryItem("instrument_name", inst); url.setQuery(q);

    QNetworkRequest req(url);
    QNetworkReply* rep = m_net.get(req);
    m_ivInflight = 1;
    rep->setProperty("inst", inst);

    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        const QString inst = rep->property("inst").toString();
        const QByteArray bytes = rep->readAll();
        rep->deleteLater();

        QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (doc.isObject()) {
            const QJsonObject res = doc.object().value("result").toObject();
            const double mkiv = res.value("mark_iv").toDouble();
            if (mkiv > 0.0) {
                m_lastIV[inst] = mkiv;
            }
            else {
                const QJsonObject greeks = res.value("greeks").toObject();
                const double alt = greeks.value("iv").toDouble();
                if (alt > 0.0) m_lastIV[inst] = alt;
            }
        }
        m_ivInflight = 0;
        });
}
