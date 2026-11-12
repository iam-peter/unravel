#pragma once

#include "storage/store.h"

namespace wh {

class CsvStore : public Store {
public:
    explicit CsvStore(std::string path);
    bool read(std::vector<Item> &items) override;
    bool write(const std::vector<Item> &items) override;
    std::string describe() const override;

private:
    std::string m_path;
    int m_writes = 0;
};

std::vector<std::string> splitFields(const std::string &line);

} // namespace wh
