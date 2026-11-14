#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace isvn::cli {

int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out,
        std::ostream& err);

}  // namespace isvn::cli
