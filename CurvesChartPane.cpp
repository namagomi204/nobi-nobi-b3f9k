#include "CurvesChartPane.h"
#include <QGridLayout>
#include <QPainter>

static inline void setAxisDefaults(QValueAxis* axX, QValueAxis* axY) {
    if (!axX || !axY) return;
    axX->setLabelFormat("%.4f");   // 必要に応じて桁数調整
    axY->setLabelFormat("%.4f");
    axX->setMinorTickCount(2);
    axY->setMinorTickCount(2);
    axX->applyNiceNumbers();
    axY->applyNiceNumbers();
}

CurvesChartPane::CurvesChartPane(QWidget* parent) : QWidget(parent) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setSpacing(6);

    setupChart(m_chartG, m_serG, m_axGx, m_axGy, QStringLiteral("Gamma"));
    setupChart(m_chartV, m_serV, m_axVx, m_axVy, QStringLiteral("Vega"));
    setupChart(m_chartC, m_serC, m_axCx, m_axCy, QStringLiteral("Cumulative PnL"));

    m_viewG = new QChartView(m_chartG, this);
    m_viewV = new QChartView(m_chartV, this);
    m_viewC = new QChartView(m_chartC, this);

    for (auto v : { m_viewG, m_viewV, m_viewC }) {
        v->setRenderHint(QPainter::Antialiasing);
        v->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        v->setContentsMargins(0, 0, 0, 0);
    }
    // 上段がペチャンコにならないよう最低サイズ
    m_viewG->setMinimumHeight(140);
    m_viewV->setMinimumHeight(140);
    m_viewC->setMinimumHeight(220);

    grid->addWidget(m_viewG, 0, 0);
    grid->addWidget(m_viewV, 0, 1);
    grid->addWidget(m_viewC, 1, 0, 1, 2);

    // 伸縮バランス（下段を大きめ）
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 2);

    setLayout(grid);
}

void CurvesChartPane::setupChart(QChart*& chart, QLineSeries*& series,
    QValueAxis*& axX, QValueAxis*& axY,
    const QString& title) {
    chart = new QChart();
    chart->legend()->setVisible(true);
    chart->setTitle(title);

    series = new QLineSeries(chart);
    axX = new QValueAxis(chart);
    axY = new QValueAxis(chart);
    chart->addSeries(series);
    setAxisDefaults(axX, axY);

    chart->addAxis(axX, Qt::AlignBottom);
    chart->addAxis(axY, Qt::AlignLeft);
    series->attachAxis(axX);
    series->attachAxis(axY);
    chart->setMargins(QMargins(6, 6, 6, 6));
}

void CurvesChartPane::applyPoints(QLineSeries* series, QValueAxis* axX, QValueAxis* axY,
    const QList<QPointF>& pts, const QString& label) {
    if (!series || !axX || !axY) return;

    series->blockSignals(true);
    series->replace(pts);
    series->setName(label);
    series->blockSignals(false);

    if (pts.isEmpty()) return;

    double minX = pts.first().x(), maxX = pts.first().x();
    double minY = pts.first().y(), maxY = pts.first().y();
    for (const auto& p : pts) {
        if (p.x() < minX) minX = p.x();
        if (p.x() > maxX) maxX = p.x();
        if (p.y() < minY) minY = p.y();
        if (p.y() > maxY) maxY = p.y();
    }
    if (minX == maxX) { minX -= 1.0; maxX += 1.0; }
    if (minY == maxY) { minY -= 1.0; maxY += 1.0; }

    axX->setRange(minX, maxX);
    axY->setRange(minY, maxY);
}

void CurvesChartPane::setGammaPoints(const QList<QPointF>& pts, const QString& label) {
    applyPoints(m_serG, m_axGx, m_axGy, pts, label);
}
void CurvesChartPane::setVegaPoints(const QList<QPointF>& pts, const QString& label) {
    applyPoints(m_serV, m_axVx, m_axVy, pts, label);
}
void CurvesChartPane::setCumulativePnLPoints(const QList<QPointF>& pts, const QString& label) {
    applyPoints(m_serC, m_axCx, m_axCy, pts, label);
}
