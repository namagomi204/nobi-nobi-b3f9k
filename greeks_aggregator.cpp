// greeks_aggregator.cpp
#include "greeks_aggregator.h"
#include "iv_greeks.h"

void GreeksAggregator::aggregate(LinkedOrder& g, double S) {
    double d = 0, ga = 0, va = 0, ch = 0;
    for (auto& l : g.legs) {
        const auto cp = (l.cp == OptionCP::Call ? OptionCP::Call : OptionCP::Put);
        auto gk = IVGreeks::solveAndGreeks(cp, l.premium, S, l.strike, l.tteMin, 0.0, 0.0);
        l.tradeIV = (l.tradeIV > 0.0 ? l.tradeIV : gk.iv); // 既に外部提供IVがあれば尊重
        // レッグGreeksを“枚数×乗数”で重み付けして合算
        const double w = l.qty * l.multiplier;
        d += w * gk.delta;
        ga += w * gk.gamma;
        va += w * gk.vanna;
        ch += w * gk.charm;
    }
    g.delta = d; g.gamma = ga; g.vanna = va; g.charm = ch;
}
