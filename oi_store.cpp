// oi_store.cpp
#include "oi_store.h"

void OIStore::setOI(qint64 expiryMs, double strike, bool isCall, double oi) {
    m_oi[{expiryMs, strike, isCall}] = oi;
}
double OIStore::getOI(qint64 expiryMs, double strike, bool isCall) const {
    auto it = m_oi.find({ expiryMs,strike,isCall });
    return (it != m_oi.end() ? it.value() : 0.0);
}
double OIStore::computeRatio(qint64 expiryMs, const QMap<double, double>& myAbsQtyAtStrike, bool isCall) const {
    double mx = 0.0;
    for (auto it = myAbsQtyAtStrike.begin(); it != myAbsQtyAtStrike.end(); ++it) {
        const double k = it.key();
        const double my = it.value();
        const double oi = getOI(expiryMs, k, isCall);
        if (oi > 0.0) mx = std::max(mx, std::abs(my) / oi);
    }
    return mx;
}
