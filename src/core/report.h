#pragma once

#include "analysis.h"
#include "model.h"

// A review report in Markdown: the project in numbers, its modules and
// their coupling, and every finding with where it is. Meant to be read by
// a reviewer, pasted into a pull request, or handed to an assistant.
namespace Unravel::Reporting {

struct Options {
    int maxPerKind = 25;
    bool withModules = true;
    bool withHotspots = true;
};

QString markdown(const CodeModel &m, const Analysis::Report &report, const Options &options = {});

} // namespace Unravel::Reporting
