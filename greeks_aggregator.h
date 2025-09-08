// greeks_aggregator.h
#pragma once
#include "trade_types.h"

class GreeksAggregator {
public:
    // 先物価格Sを渡す（BSのデルタはS依存）
    static void aggregate(LinkedOrder& g, double S);
};
