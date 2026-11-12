#pragma once

#include "core/item.h"
#include "storage/store.h"

#include <map>
#include <memory>

namespace wh {

class PricingPolicy;

// Holds every item; saves through a Store
class Inventory {
public:
    explicit Inventory(Store *store);

    void add(const Item &item);
    bool remove(const std::string &sku);
    Item *find(const std::string &sku);
    int restock(const std::string &sku, int amount);
    double totalValue() const;
    std::vector<Item> lowStock(int threshold) const;
    std::vector<Item> byCategory(Category c) const;
    void applyPricing(const PricingPolicy &policy);
    bool save();
    bool load();
    std::string report(bool verbose, bool onlyLow, int threshold, Category filter, bool useFilter, int limit) const;

private:
    std::map<std::string, Item> m_items;
    Store *m_store;
    int m_revision = 0;
    int m_lastAudit = 0;
};

extern int g_operations;

} // namespace wh
