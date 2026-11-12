#include "core/pricing.h"

#include <cmath>

namespace wh {

double roundPrice(double value)
{
    return std::round(value * 100.0) / 100.0;
}

double FlatPricing::price(const Item &item) const
{
    return roundPrice(item.unitPrice * (1.0 + m_markup));
}

double TieredPricing::price(const Item &item) const
{
    if (item.quantity > 1000)
        return roundPrice(item.unitPrice * 0.8);
    if (item.quantity > 100)
        return roundPrice(item.unitPrice * 0.9);
    return roundPrice(item.unitPrice);
}

double SeasonalPricing::price(const Item &item) const
{
    const double base = TieredPricing::price(item);
    return item.category == Category::Consumable ? roundPrice(base * 0.95) : base;
}

long long fibonacciDiscount(int n)
{
    if (n < 2)
        return n;
    return fibonacciDiscount(n - 1) + fibonacciDiscount(n - 2);
}

} // namespace wh
