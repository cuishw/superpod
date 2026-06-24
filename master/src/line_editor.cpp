#include "pcie/line_editor.h"

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#include <unistd.h>
#endif

namespace pcie {
namespace {

#if defined(__unix__) || defined(__APPLE__)

class RawTerminal {
   public:
    RawTerminal() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_) != 0) {
            return;
        }

        auto raw = saved_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
    }

    ~RawTerminal() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
        }
    }

    [[nodiscard]] bool Active() const { return active_; }

   private:
    termios saved_{};
    bool active_{false};
};

bool ReadByte(char& byte) {
    while (true) {
        const auto count = read(STDIN_FILENO, &byte, 1);
        if (count == 1) {
            return true;
        }
        if (count == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

void Write(std::string_view text) {
    while (!text.empty()) {
        const auto count = write(STDOUT_FILENO, text.data(), text.size());
        if (count > 0) {
            text.remove_prefix(static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

void Redraw(std::string_view prompt, const std::string& line,
            std::size_t cursor) {
    Write("\r");
    Write(prompt);
    Write(line);
    Write("\x1b[K");
    if (cursor < line.size()) {
        Write("\x1b[" + std::to_string(line.size() - cursor) + "D");
    }
}

void DeletePreviousWord(std::string& line, std::size_t& cursor) {
    const auto original = cursor;
    while (cursor > 0 && line[cursor - 1] == ' ') {
        --cursor;
    }
    while (cursor > 0 && line[cursor - 1] != ' ') {
        --cursor;
    }
    line.erase(cursor, original - cursor);
}

#endif

}  // namespace

ReadLineResult LineEditor::ReadLine(std::string_view prompt) {
#if defined(__unix__) || defined(__APPLE__)
    RawTerminal terminal;
    if (terminal.Active()) {
        std::string line;
        std::string draft;
        std::size_t cursor = 0;
        std::size_t history_index = history_.size();
        Write(prompt);

        while (true) {
            char byte = 0;
            if (!ReadByte(byte)) {
                Write("\r\n");
                return {ReadLineStatus::kEndOfInput, {}};
            }

            const auto key = static_cast<unsigned char>(byte);
            if (key == '\r' || key == '\n') {
                Write("\r\n");
                if (!line.empty() &&
                    (history_.empty() || history_.back() != line)) {
                    history_.push_back(line);
                }
                return {ReadLineStatus::kLine, std::move(line)};
            }
            if (key == 3) {  // Ctrl+C
                Write("^C\r\n");
                return {ReadLineStatus::kCancelled, {}};
            }
            if (key == 4) {  // Ctrl+D
                if (line.empty()) {
                    Write("\r\n");
                    return {ReadLineStatus::kEndOfInput, {}};
                }
                if (cursor < line.size()) {
                    line.erase(cursor, 1);
                    Redraw(prompt, line, cursor);
                }
                continue;
            }
            if (key == 1) {  // Ctrl+A
                cursor = 0;
                Redraw(prompt, line, cursor);
                continue;
            }
            if (key == 5) {  // Ctrl+E
                cursor = line.size();
                Redraw(prompt, line, cursor);
                continue;
            }
            if (key == 11) {  // Ctrl+K
                line.erase(cursor);
                Redraw(prompt, line, cursor);
                continue;
            }
            if (key == 21) {  // Ctrl+U
                line.clear();
                cursor = 0;
                Redraw(prompt, line, cursor);
                continue;
            }
            if (key == 23) {  // Ctrl+W
                DeletePreviousWord(line, cursor);
                Redraw(prompt, line, cursor);
                continue;
            }
            if (key == 8 || key == 127) {  // Backspace
                if (cursor > 0) {
                    line.erase(--cursor, 1);
                    Redraw(prompt, line, cursor);
                }
                continue;
            }
            if (key == 27) {  // ANSI escape sequence
                char first = 0;
                char code = 0;
                if (!ReadByte(first) || first != '[' || !ReadByte(code)) {
                    continue;
                }
                if (code == 'A' && history_index > 0) {  // Up
                    if (history_index == history_.size()) {
                        draft = line;
                    }
                    line = history_[--history_index];
                    cursor = line.size();
                    Redraw(prompt, line, cursor);
                } else if (code == 'B' &&
                           history_index < history_.size()) {  // Down
                    ++history_index;
                    line = history_index == history_.size()
                               ? draft
                               : history_[history_index];
                    cursor = line.size();
                    Redraw(prompt, line, cursor);
                } else if (code == 'C' && cursor < line.size()) {  // Right
                    ++cursor;
                    Redraw(prompt, line, cursor);
                } else if (code == 'D' && cursor > 0) {  // Left
                    --cursor;
                    Redraw(prompt, line, cursor);
                } else if (code == 'H') {  // Home
                    cursor = 0;
                    Redraw(prompt, line, cursor);
                } else if (code == 'F') {  // End
                    cursor = line.size();
                    Redraw(prompt, line, cursor);
                } else if (code == '3') {  // Delete: ESC [ 3 ~
                    char suffix = 0;
                    if (ReadByte(suffix) && suffix == '~' &&
                        cursor < line.size()) {
                        line.erase(cursor, 1);
                        Redraw(prompt, line, cursor);
                    }
                }
                continue;
            }
            if (key >= 32 && key < 127) {
                line.insert(cursor++, 1, byte);
                Redraw(prompt, line, cursor);
            }
        }
    }
#endif

    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return {ReadLineStatus::kEndOfInput, {}};
    }
    if (!line.empty() && (history_.empty() || history_.back() != line)) {
        history_.push_back(line);
    }
    return {ReadLineStatus::kLine, std::move(line)};
}

}  // namespace pcie
