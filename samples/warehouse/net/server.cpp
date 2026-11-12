#include "net/server.h"
#include "app/cli.h"

namespace wh::net {

Server::Server(Inventory &inventory)
    : m_inventory(inventory)
{
}

std::string Server::handle(const std::string &request)
{
    ++m_requests;
    const auto words = splitWords(request);   // net depends on app
    if (words.empty())
        return "ERR empty";
    if (words[0] == "GET" && words.size() == 2)
        return get(words[1]);
    if (words[0] == "RESTOCK" && words.size() == 3)
        return restock(words[1], std::stoi(words[2]));
    return "ERR unknown";
}

std::string Server::get(const std::string &sku)
{
    const Item *item = m_inventory.find(sku);
    return item ? item->name + " " + std::to_string(item->quantity) : "ERR missing";
}

std::string Server::restock(const std::string &sku, int amount)
{
    const int now = m_inventory.restock(sku, amount);
    return now < 0 ? "ERR missing" : "OK " + std::to_string(now);
}

} // namespace wh::net
