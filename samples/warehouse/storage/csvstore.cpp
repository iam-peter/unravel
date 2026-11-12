#include "storage/csvstore.h"

#include <fstream>

namespace wh {

CsvStore::CsvStore(std::string path)
    : m_path(std::move(path))
{
}

// The same loop as splitSku in core/item.cpp, written again
std::vector<std::string> splitFields(const std::string &line)
{
    std::vector<std::string> fields;
    std::string field;
    for (char c : line) {
        if (c == ',') {
            if (!field.empty())
                fields.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    if (!field.empty())
        fields.push_back(field);
    return fields;
}

bool CsvStore::read(std::vector<Item> &items)
{
    std::ifstream in(m_path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        const auto f = splitFields(line);
        if (f.size() < 4)
            continue;
        Item item;
        item.sku = f[0];
        item.name = f[1];
        item.quantity = std::stoi(f[2]);
        item.unitPrice = std::stod(f[3]);
        items.push_back(item);
    }
    return true;
}

bool CsvStore::write(const std::vector<Item> &items)
{
    std::ofstream out(m_path);
    for (const Item &item : items)
        out << item.sku << ',' << item.name << ',' << item.quantity << ',' << item.unitPrice << '\n';
    ++m_writes;
    return bool(out);
}

std::string CsvStore::describe() const
{
    return "csv:" + m_path;
}

} // namespace wh
