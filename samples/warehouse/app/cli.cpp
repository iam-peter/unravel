#include "app/cli.h"
#include "core/inventory.h"
#include "core/pricing.h"
#include "net/server.h"

#include <iostream>

namespace wh {

std::vector<std::string> splitWords(const std::string &text)
{
    std::vector<std::string> words;
    std::string word;
    for (char ch : text) {
        if (ch == ' ') {
            if (!word.empty())
                words.push_back(word);
            word.clear();
        } else {
            word += ch;
        }
    }
    if (!word.empty())
        words.push_back(word);
    return words;
}

void printUsage()
{
    std::cout << "usage: warehouse add|list|price|serve ...\n";
}

int runCommand(Inventory &inventory, const std::vector<std::string> &args)
{
    if (args.empty()) {
        printUsage();
        return 1;
    }
    if (args[0] == "add" && args.size() >= 4) {
        Item item;
        item.sku = args[1];
        item.name = args[2];
        item.quantity = std::stoi(args[3]);
        inventory.add(item);
        return inventory.save() ? 0 : 2;
    }
    if (args[0] == "list") {
        std::cout << inventory.report(true, false, 10, Category::Part, false, 0);
        return 0;
    }
    if (args[0] == "price") {
        SeasonalPricing pricing;
        inventory.applyPricing(pricing);
        std::cout << inventory.totalValue() << "\n";
        return 0;
    }
    if (args[0] == "serve") {
        net::Server server(inventory);
        std::string line;
        while (std::getline(std::cin, line))
            std::cout << server.handle(line) << "\n";
        return 0;
    }
    printUsage();
    return 1;
}

} // namespace wh
