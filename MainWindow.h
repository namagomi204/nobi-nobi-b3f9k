#pragma once
#define NOMINMAX

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QDateTime>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QPair>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <deque>
#include "oi_store.h"
#include "pin_map.h"
#include "nbbo_store.h"
#include "curves.h"
#include "CurvesChartPane.h"

class WebSocketClient;
class QTableWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// ========= 定数 =========
static constexpr int     ONE_MIN_MS = 60 * 1000;
static constexpr int     FIVE_MIN_MS = 5 * 60 * 1000;
static constexpr qint64  HOUR_MS = 60ll * 60 * 1000;
static constexpr qint64  DAY_MS = 24ll * 60 * 60 * 1000;

static constexpr int     AUTO_MAX_INFLIGHT = 8;     // 自動バックフィル同時実行

// バースト検出・重複抑制
static constexpr int     BURST_WINDOW_MS = 6 * 1000;    // 連続判定窓
static constexpr double  STRIKE_CLUSTER_WIDTH = 1500.0; // 行使のクラスタ幅
static constexpr int     SIGNAL_DEDUP_MS = 90 * 1000;   // シグナル重複抑制

// ========= 構造体 =========
struct TradeEvent {
    qint64  tsMs{};          // 取引時刻(ms)
    double  amount{};        // 枚数
    double  delta{};         // その時のΔ
    int     sign{};          // +1=buy, -1=sell
    QString instrument;      // 銘柄
};

struct MiniEv {
    qint64 ts{};
    double qty{};            // 枚数(>0)
    double dvol{};           // Δ加重出来高（参考）
};

struct FlowBurst {
    qint64 startMs{};
    qint64 lastMs{};
    bool   isBuy{};
    bool   isCall{};
    double centerK{};
    double qtySum{};
    double dVolSum{};
    int    trades{};
    QSet<QString> instruments;
};

// ★第三弾：レッグ明細1件（NBBO/Aggressor対応）
struct LegDetail {
    qint64  ts{};
    QString linkKey;     // クラスタkey（exp|isCall|kRound）
    QString inst;
    int     sign{};      // +1/-1
    double  amount{};    // >0で保存
    double  estDelta{};  // |Δ|（表示用）
    double  price{};     // 約定プレミアム（清算通貨基準）

    // 解析用付帯
    Aggressor aggressor{ Aggressor::Unknown };
    QString   venue{ "Deribit" };

    // 銘柄属性
    qint64    expiryMs{};
    double    strike{};
    bool      isCall{};

    // 市場状態（NBBO）
    double nbboBid{};
    double nbboAsk{};
    double mid{};
    double bpDiffBp{}; // (price-mid)/mid*10000

    // 任意
    double  tradeIV{};      // 取得できれば
    QString currency;       // 取得不可なら空でOK
    double  multiplier{ 1.0 };
    double  fee{ 0.0 };
    QString orderId;        // trade_id 等
};


class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private: // ===== UI =====
    void hookUiActions();
    void refreshWatchList();

private: // ===== WS / RPC =====
    void bootstrapAuto();
    void onRpc(int id, const QJsonObject& reply);
    void handleDeribitMsg(const QJsonObject& obj);

private: // ===== 銘柄・購読 =====
    void   populateExpiryChoices();          // 先頭に「All」を入れる
    qint64 selectedExpiryMs() const;         // UIの現在値（Allなら0）
    qint64 displayExpiryFilterMs() const;    // 表示フィルタ（Allなら0）
    double currentMoneynessBand() const;
    void   chooseAndSubscribe();             // 表示はAllでも購読は期近

private slots:
    void onBackfillClicked();                // 手動バックフィル

private:
    void prefetchTickersForTargets();
    void requestTickerFor(const QString& inst);
    void requestBackfillFor(const QString& inst, qint64 fromMs, qint64 toMs);

private: // ===== 集計 =====
    bool   isBigTrade(double amount) const;   // 単発が閾値以上か？
    void   addEvent(const TradeEvent& ev);
    void   pruneOld(qint64 nowMs);
    double sumDeltaVolume(qint64 nowMs, int windowMs) const;

private: // ===== 満期アクティビティ =====
    qint64 expiryFromInst(const QString& inst) const;
    void   recordExpiryEvent(const QString& inst, qint64 ts, double amount, int sign, double delta);
    void   updateExpiryActivityTable();
    bool   alreadySeenTrade(const QString& tradeId, qint64 ts);

private: // ===== シグナル =====
    bool   isCallFromInst(const QString& inst);
    double strikeFromInst(const QString& inst);
    void   onNewTradeForBurst(const TradeEvent& ev);

    void   emitSignalRow(const FlowBurst& b, qint64 expMs);
    void   upsertSignalRow(const QString& key, qint64 expMs,
        const FlowBurst& snapshot, double residualQty,
        double absDVol, double avgAbsDelta, double notionalUSD);

    void   removeSignalRowIfExists(const QString& key);
    int    findRowByKey(const QString& key) const;

    // 一括再構築（初回/フィルタ変更時）
    void   rebuildSignalTableFromResidual();

    struct AmtSample { qint64 ts; double absAmt; };
    QList<AmtSample> m_amtSamples;        // 直近24hの全約定サンプル
    void pushAmtSample(qint64 ts, double absAmt);
    void pruneAmtSamples(qint64 now);
    int  currentBigUnit() const;          // ← ここを宣言（実装はcpp）

private: // ===== 自動バックフィル =====
    // 従来の“全期間バックフィル”（スナップショットが無い初回のみ使う）
    void  autoBackfillAllExpiriesInit();
    void  autoBackfillPump();
    void  requestBackfillAuto(const QString& inst, qint64 fromMs, qint64 toMs);
    QList<QString> m_autoBackfillQueue;
    int     m_autoInflight{ 0 };
    qint64  m_autoBackFromMs{ 0 }, m_autoBackToMs{ 0 };
    bool    m_autoBackfillDone{ false };

private: // ===== 差分バックフィル（前回スナップショット → 現在）=====
    void  autoBackfillDeltaInit();
    void  autoBackfillDeltaPump();
    void  requestBackfillDelta(const QString& inst, qint64 fromMs, qint64 toMs);
    QList<QString> m_deltaQueue;
    int     m_deltaInflight{ 0 };
    qint64  m_deltaFromMs{ 0 }, m_deltaToMs{ 0 };
    bool    m_deltaDone{ false };

private: // ===== フルバックフィル（生存満期・全期間）=====
    struct FullTask { QString inst; qint64 fromMs; qint64 toMs; qint64 stepMs; };
    std::deque<FullTask> m_fullQueue;
    int   m_fullInflight{ 0 };
    bool  m_fullDone{ false };

    void  fullBackfillLiveExpiriesInit();
    void  fullBackfillPump();
    void  requestBackfillWindow(const QString& inst, qint64 fromMs, qint64 toMs, qint64 stepMs);

private: // ===== 残存推定（=オフライン清算反映）=====
    QString makeClusterKey(qint64 expMs, bool isCall, double strike) const;

    // ★第三弾：price を渡せるように引数追加
    void    applyTradeToResidual(const QString& inst, qint64 ts,
        double amount, int sign, double delta,
        double tradePx = 0.0);

    QPair<double, double> residualForKey(const QString& key) const;
    bool    passSignalFilter(qint64 expMs) const;

private:
    CurvesChartPane* m_curvesPane{ nullptr };

    void updateCurvesCharts();               // ★追加
    QList<QPointF> m_cumPnlPts;              // ★追加: Cumulative PnL用（時刻→値）
    qint64 m_pnlStartMs{ 0 };                  // ★追加
    double m_cumPnlValue{ 0.0 };               // ★追加

private: // ===== 状態スナップショット =====
    bool  loadSnapshot();        // 復元（あれば即座にUIへ反映）
    void  saveSnapshot() const;  // 保存（終了時）
    qint64 m_lastSnapshotTs{ 0 };  // 前回保存時刻(ms)

protected:
    void closeEvent(QCloseEvent* e) override;  // 終了時に保存

private: // ===== メンバ =====
    Ui::MainWindow* ui{ nullptr };
    WebSocketClient* m_ws{ nullptr };
    QTimer           m_uiTick;

    // 価格・銘柄
    double     m_underlyingPx{ 0.0 };
    qint64     m_nearestExpiryMs{ 0 };
    bool       m_subscribedOnce{ false };
    QJsonArray m_instruments;
    QStringList m_targetInstruments;
    QStringList m_channels;

    // ギリシャ・IV
    QHash<QString, double> m_lastDelta; // inst → delta
    QHash<QString, double> m_lastIV;    // inst → iv

    // スカッシュ用イベント
    QVector<TradeEvent> m_events;

    // 満期アクティビティ
    QHash<qint64, QVector<MiniEv>> m_expiryEvents;    // expiryMs → events
    QHash<QString, qint64>         m_instToExpiryMs;  // inst → expiryMs

    // 二重受信防止
    QSet<QString>                    m_seenTradeIds;
    QVector<QPair<qint64, QString>>  m_seenTradeQueue; // (ts, id)

    // シグナル重複抑制
    QSet<QString>                    m_signalKeys;
    QVector<QPair<qint64, QString>>  m_signalKeyQueue; // (ts, key)

    // バースト中間
    QVector<FlowBurst> m_bursts;

    // 手動履歴
    qint64 m_backFromMs{ 0 }, m_backToMs{ 0 };
    int    m_pendingTickers{ 0 };
    int    m_backfillPending{ 0 };

    // RPC id
    int m_idGetInstruments{ 0 };
    int m_idPerpTicker{ 0 };

    // REST
    QNetworkAccessManager m_net;
    QTimer                m_oiTimer;         // 定期OI更新（軽め）
    QTimer                m_ivTimer;         // IVポンピング（200msごとに1件）
    QSet<QString>         m_ivQueued;        // 既にキューイン済みの去重用
    QQueue<QString>       m_ivQueue;         // リクエスト待ち行列
    int                   m_ivInflight{ 0 };   // 同時実行数（控えめに1）

    // 満期アクティビティのソート保持（1=全期間, 2=24h, 3=1h）
    int           m_expActSortCol{ 1 };
    Qt::SortOrder m_expActSortOrder{ Qt::DescendingOrder };

    // 残存推定：クラスターごとの残枚数・Δ加重
    QHash<QString, double> m_residualQtyByKey;   // key → 残枚数
    QHash<QString, double> m_residualDVolByKey;  // key → 残Δ加重
    // 買い(+)/売り(-)をそのまま合算した“符号付き枚数”
    QHash<QString, double> m_residualSignedQtyByKey;

    // 追加：クラスタの最終時刻・件数・ユニーク銘柄
    QHash<QString, qint64>         m_residualLastTsByKey;  // key → 最終約定時刻(ms)
    QHash<QString, int>            m_residualTradesByKey;  // key → 件数
    QHash<QString, QSet<QString>>  m_residualInstsByKey;   // key → 参加銘柄セット

    // 仕込み時刻（アンカー）: シグナル行ごとに固定
    QHash<QString, qint64> m_signalAnchorTsByKey;  // key → anchorMs

    // シグナル行のインデックス
    QHash<QString, int>     m_signalRowIndexByKey;  // key → row

    // ★第三弾：レッグ明細（クラスタkey -> 直近レッグ列）
    QHash<QString, QVector<LegDetail>> m_legsByKey;

    // ★第三弾：レッグ明細テーブル（存在すれば使う。名前は動的探索）
    QTableWidget* m_tableLegs{ nullptr };

    // ★第三弾：シグナル選択 → 明細反映
    void populateLegDetailsForKey(const QString& key);

    // OI 保存（将来のフェッチ用。無ければ0で進む）
    OIStore m_oi;

    // ピンマップ更新
    void updatePinMapTable();
    int  m_pinMapTick{ 0 }; // UI tick で軽くスロットル

    // OI取得
    void requestOIAll();
    void handleOIReply(const QByteArray& bytes);

    void updateCurvesTables();
    int  m_curvesTick{ 0 };

    // IVオンデマンド取得
    void queueIV(const QString & inst);
    void pumpIV();

    // NBBOキャッシュ
    NbboStore m_nbbo;

};
