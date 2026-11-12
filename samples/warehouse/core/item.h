#pragma once

#include <string>
#include <vector>

namespace wh {

enum class Category { Tool, Part, Consumable };

struct Item {
    std::string sku;
    std::string name;
    Category category = Category::Part;
    int quantity = 0;
    double unitPrice = 0.0;

    double value() const;
    bool lowStock(int threshold) const;
};

std::string categoryName(Category c);
std::vector<std::string> splitSku(const std::string &sku);

} // namespace wh
