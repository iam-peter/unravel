#include "app/cli.h"
#include "core/inventory.h"

#include <memory>

int main(int argc, char **argv)
{
    std::unique_ptr<wh::Store> store(wh::createStore(argc > 1 ? argv[1] : "memory"));
    wh::Inventory inventory(store.get());
    inventory.load();
    std::vector<std::string> args(argv + 2 > argv + argc ? argv + argc : argv + 2, argv + argc);
    return wh::runCommand(inventory, args);
}
