#pragma once

#include <string>
#include <vector>

namespace wh {

class Inventory;

std::vector<std::string> splitWords(const std::string &text);
int runCommand(Inventory &inventory, const std::vector<std::string> &args);
void printUsage();

} // namespace wh
