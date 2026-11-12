#include "core/inventory.h"
#include "core/pricing.h"

#include <sstream>

namespace wh {

int g_operations = 0;

Inventory::Inventory(Store *store)
    : m_store(store)
{
}

void Inventory::add(const Item &item)
{
    ++g_operations;
    ++m_revision;
    m_items[item.sku] = item;
}

bool Inventory::remove(const std::string &sku)
{
    ++g_operations;
    m_lastAudit = m_revision;
    return m_items.erase(sku) > 0;
}

Item *Inventory::find(const std::string &sku)
{
    auto it = m_items.find(sku);
    return it == m_items.end() ? nullptr : &it->second;
}

int Inventory::restock(const std::string &sku, int amount)
{
    Item *item = find(sku);
    if (!item)
        return -1;
    item->quantity += amount;
    ++g_operations;
    return item->quantity;
}

double Inventory::totalValue() const
{
    double sum = 0;
    for (const auto &[sku, item] : m_items)
        sum += item.value();
    return sum;
}

std::vector<Item> Inventory::lowStock(int threshold) const
{
    std::vector<Item> out;
    for (const auto &[sku, item] : m_items) {
        if (item.lowStock(threshold))
            out.push_back(item);
    }
    return out;
}

std::vector<Item> Inventory::byCategory(Category c) const
{
    std::vector<Item> out;
    for (const auto &[sku, item] : m_items) {
        if (item.category == c)
            out.push_back(item);
    }
    return out;
}

void Inventory::applyPricing(const PricingPolicy &policy)
{
    for (auto &[sku, item] : m_items)
        item.unitPrice = policy.price(item);
}

bool Inventory::save()
{
    std::vector<Item> items;
    for (const auto &[sku, item] : m_items)
        items.push_back(item);
    return m_store->write(items);
}

bool Inventory::load()
{
    std::vector<Item> items;
    if (!m_store->read(items))
        return false;
    m_items.clear();
    for (const Item &item : items)
        m_items[item.sku] = item;
    return true;
}

std::string Inventory::report(bool verbose, bool onlyLow, int threshold, Category filter, bool useFilter, int limit) const
{
    std::ostringstream out;
    int shown = 0;
    for (const auto &[sku, item] : m_items) {
        if (useFilter && item.category != filter)
            continue;
        if (onlyLow && !item.lowStock(threshold))
            continue;
        if (limit > 0 && shown >= limit)
            break;
        if (verbose) {
            out << sku << " " << item.name << " (" << categoryName(item.category) << ") ";
            if (item.quantity == 0) {
                out << "OUT OF STOCK";
            } else if (item.quantity < threshold) {
                if (item.category == Category::Consumable && item.quantity < threshold / 2)
                    out << "reorder now";
                else
                    out << "low: " << item.quantity;
            } else {
                out << item.quantity;
            }
            out << " @ " << item.unitPrice << "\n";
        } else {
            out << sku << " " << item.quantity << "\n";
        }
        ++shown;
    }
    return out.str();
}

} // namespace wh
