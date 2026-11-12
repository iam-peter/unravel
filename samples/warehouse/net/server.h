#pragma once

#include "core/inventory.h"

#include <string>

namespace wh::net {

// Answers "GET <sku>" and "RESTOCK <sku> <n>" requests
class Server {
public:
    explicit Server(Inventory &inventory);
    std::string handle(const std::string &request);
    int requests() const { return m_requests; }

private:
    std::string get(const std::string &sku);
    std::string restock(const std::string &sku, int amount);
    Inventory &m_inventory;
    int m_requests = 0;
};

} // namespace wh::net
