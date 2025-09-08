#pragma once
#include <QWidget>
#include <QList>
#include <QPointF>
#include <QString>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

class CurvesChartPane : public QWidget {
    Q_OBJECT
public:
    explicit CurvesChartPane(QWidget* parent = nullptr);

    void setGammaPoints(const QList<QPointF>& pts, const QString& label);
    void setVegaPoints(const QList<QPointF>& pts, const QString& label);
    void setCumulativePnLPoints(const QList<QPointF>& pts, const QString& label);

private:
    void setupChart(QChart*& chart, QLineSeries*& series,
        QValueAxis*& axX, QValueAxis*& axY,
        const QString& title);
    void applyPoints(QLineSeries* series, QValueAxis* axX, QValueAxis* axY,
        const QList<QPointF>& pts, const QString& label);

private:
    // Gamma
    QLineSeries* m_serG = nullptr;
    QValueAxis* m_axGx = nullptr;
    QValueAxis* m_axGy = nullptr;
    QChart* m_chartG = nullptr;
    QChartView* m_viewG = nullptr;

    // Vega
    QLineSeries* m_serV = nullptr;
    QValueAxis* m_axVx = nullptr;
    QValueAxis* m_axVy = nullptr;
    QChart* m_chartV = nullptr;
    QChartView* m_viewV = nullptr;

    // Cumulative PnL
    QLineSeries* m_serC = nullptr;
    QValueAxis* m_axCx = nullptr;
    QValueAxis* m_axCy = nullptr;
    QChart* m_chartC = nullptr;
    QChartView* m_viewC = nullptr;
};
