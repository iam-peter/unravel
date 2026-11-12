#include "storage/store.h"
#include "storage/csvstore.h"
#include "storage/memorystore.h"
#include "core/inventory.h"

namespace wh {

Store *createStore(const std::string &url)
{
    ++g_operations;   // storage reaching back into core
    if (url.rfind("csv:", 0) == 0)
        return new CsvStore(url.substr(4));
    return new MemoryStore;
}

} // namespace wh
