// nbbo_store.cpp
#include "nbbo_store.h"
#include <cmath>

void NbboStore::update(const QString& inst, double bid, double ask) {
    if (inst.isEmpty() || bid <= 0.0 || ask <= 0.0 || ask < bid) return;
    m_nbbo[inst] = NbboSnap{ bid, ask };
}

NbboSnap NbboStore::get(const QString& inst) const {
    auto it = m_nbbo.find(inst);
    return (it != m_nbbo.end()) ? it.value() : NbboSnap{};
}

Aggressor NbboStore::inferAggressor(const QString& inst, double tradePx, double* bpDiffBp) const {
    const auto nb = get(inst);
    if (!nb.valid() || tradePx <= 0.0) return Aggressor::Unknown;
    const double mid = nb.mid();
    const double spread = nb.ask - nb.bid;
    const double diffBp = (mid > 0.0 ? (tradePx - mid) / mid * 10000.0 : 0.0);
    if (bpDiffBp) *bpDiffBp = diffBp;

    // しきい値（板中央±5%スプレッド幅の外側ならHit/Lift）
    const double tol = spread * 0.05;
    if (tradePx <= nb.bid - tol) return Aggressor::HitBid;
    if (tradePx >= nb.ask + tol) return Aggressor::LiftAsk;
    if (std::abs(tradePx - mid) <= tol) return Aggressor::Mid;

    // 中間ゾーン（BidよりならHitBid、AskよりならLiftAsk）
    return (tradePx < mid ? Aggressor::HitBid : Aggressor::LiftAsk);
}
