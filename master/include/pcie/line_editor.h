#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pcie {

enum class ReadLineStatus {
    kLine,
    kCancelled,
    kEndOfInput,
};

struct ReadLineResult {
    ReadLineStatus status{ReadLineStatus::kEndOfInput};
    std::string line;
};

class LineEditor {
   public:
    ReadLineResult ReadLine(std::string_view prompt);

   private:
    std::vector<std::string> history_;
};

}  // namespace pcie
