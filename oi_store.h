// oi_store.h
#pragma once
#include "op_types.h"
#include <QMap>

// OI のキー（満期/行使/Call-Put）
struct StrikeKey {
    qint64 expiryMs{};
    double strike{};
    bool   isCall{};
    // QMap 用の順序付け
        bool operator<(const StrikeKey & o) const {
        if (expiryMs != o.expiryMs) return expiryMs < o.expiryMs;
        if (isCall != o.isCall)   return isCall < o.isCall;
        return strike < o.strike;
        
    }
    
};

class OIStore {
public:
    void setOI(qint64 expiryMs, double strike, bool isCall, double oi);
    double getOI(qint64 expiryMs, double strike, bool isCall) const;
    // 指定ストライク群に対する “自分の枚数 / OI” の最大比率を返す
    double computeRatio(qint64 expiryMs, const QMap<double, double>& myAbsQtyAtStrike, bool isCall) const;
private:
    QMap<StrikeKey, double> m_oi; // key -> OI
};
