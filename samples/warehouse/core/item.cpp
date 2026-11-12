#include "core/item.h"

namespace wh {

double Item::value() const
{
    return quantity * unitPrice;
}

bool Item::lowStock(int threshold) const
{
    return quantity < threshold;
}

std::string categoryName(Category c)
{
    switch (c) {
    case Category::Tool: return "tool";
    case Category::Part: return "part";
    case Category::Consumable: return "consumable";
    }
    return "?";
}

std::vector<std::string> splitSku(const std::string &sku)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : sku) {
        if (ch == '-') {
            if (!current.empty())
                parts.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

} // namespace wh
