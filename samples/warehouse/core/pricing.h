#pragma once

#include "core/item.h"

namespace wh {

// Prices items. A polymorphic base, but its destructor isn't virtual.
class PricingPolicy {
public:
    ~PricingPolicy() {}
    virtual double price(const Item &item) const = 0;
    virtual const char *name() const { return "base"; }
};

class FlatPricing : public PricingPolicy {
public:
    explicit FlatPricing(double markup) : m_markup(markup) {}
    double price(const Item &item) const override;
private:
    double m_markup;
};

class TieredPricing : public PricingPolicy {
public:
    double price(const Item &item) const override;
    const char *name() const override { return "tiered"; }
};

class SeasonalPricing : public TieredPricing {
public:
    double price(const Item &item) const override;
};

double roundPrice(double value);
long long fibonacciDiscount(int n);

} // namespace wh
