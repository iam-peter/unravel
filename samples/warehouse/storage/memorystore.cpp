#include "storage/memorystore.h"

namespace wh {

bool MemoryStore::read(std::vector<Item> &items)
{
    items = m_items;
    return true;
}

bool MemoryStore::write(const std::vector<Item> &items)
{
    m_items = items;
    return true;
}

std::string MemoryStore::describe() const
{
    return "memory";
}

void MemoryStore::clear()
{
    m_items.clear();
}

} // namespace wh
