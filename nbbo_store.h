// nbbo_store.h
#pragma once
#include "trade_types.h"
#include <QHash>

class NbboStore {
public:
    void update(const QString& inst, double bid, double ask);
    NbboSnap get(const QString& inst) const;
    Aggressor inferAggressor(const QString& inst, double tradePx, double* bpDiffBp = nullptr) const;
private:
    QHash<QString, NbboSnap> m_nbbo; // instrument -> NBBO
};
