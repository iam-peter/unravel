#pragma once

#include "core/item.h"

#include <string>
#include <vector>

namespace wh {

class Store {
public:
    virtual ~Store() = default;
    virtual bool read(std::vector<Item> &items) = 0;
    virtual bool write(const std::vector<Item> &items) = 0;
    virtual std::string describe() const = 0;
};

Store *createStore(const std::string &url);

} // namespace wh
