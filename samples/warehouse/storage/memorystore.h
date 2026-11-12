#pragma once

#include "storage/store.h"

namespace wh {

class MemoryStore : public Store {
public:
    bool read(std::vector<Item> &items) override;
    bool write(const std::vector<Item> &items) override;
    std::string describe() const override;
    void clear();

private:
    std::vector<Item> m_items;
};

} // namespace wh
