#include "akkado/diagnostics.hpp"
#include <algorithm>
#include <cstdio>  // snprintf - locale-free alternative to ostringstream

namespace akkado {

namespace {

std::string_view severity_string(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Info:    return "info";
        case Severity::Hint:    return "hint";
    }
    return "unknown";
}

std::string_view severity_color(Severity s) {
    switch (s) {
        case Severity::Error:   return "\033[1;31m"; // Bold red
        case Severity::Warning: return "\033[1;33m"; // Bold yellow
        case Severity::Info:    return "\033[1;36m"; // Bold cyan
        case Severity::Hint:    return "\033[1;32m"; // Bold green
    }
    return "";
}

constexpr std::string_view RESET = "\033[0m";
constexpr std::string_view BOLD = "\033[1m";

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Get the line from source at the given line number (1-based)
std::string_view get_line(std::string_view source, std::uint32_t line_num) {
    std::uint32_t current_line = 1;
    std::size_t line_start = 0;

    for (std::size_t i = 0; i < source.size(); ++i) {
        if (current_line == line_num) {
            line_start = i;
            break;
        }
        if (source[i] == '\n') {
            ++current_line;
        }
    }

    if (current_line != line_num) {
        return {};
    }

    auto line_end = source.find('\n', line_start);
    if (line_end == std::string_view::npos) {
        line_end = source.size();
    }

    return source.substr(line_start, line_end - line_start);
}

} // namespace

std::string format_diagnostic(const Diagnostic& diag, std::string_view source) {
    std::string out;
    out.reserve(512);  // Reasonable initial capacity
    char num_buf[32];

    // Header: filename:line:column: severity[code]: message
    out += BOLD;
    out += diag.filename;
    out += ':';
    std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.line);
    out += num_buf;
    out += ':';
    std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.column);
    out += num_buf;
    out += ": ";
    out += RESET;

    out += severity_color(diag.severity);
    out += severity_string(diag.severity);
    if (!diag.code.empty()) {
        out += '[';
        out += diag.code;
        out += ']';
    }
    out += RESET;
    out += ": ";
    out += BOLD;
    out += diag.message;
    out += RESET;
    out += '\n';

    // Source line with caret
    if (!source.empty() && diag.location.line > 0) {
        auto line = get_line(source, diag.location.line);
        if (!line.empty()) {
            // Line number gutter
            out += "    ";
            std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.line);
            out += num_buf;
            out += " | ";
            out += line;
            out += '\n';

            // Caret line
            out += "      | ";
            for (std::uint32_t i = 1; i < diag.location.column; ++i) {
                out += ' ';
            }
            out += severity_color(diag.severity);
            out += '^';
            for (std::uint32_t i = 1; i < diag.location.length && i < 80; ++i) {
                out += '~';
            }
            out += RESET;
            out += '\n';
        }
    }

    // Related information
    for (const auto& rel : diag.related) {
        out += BOLD;
        out += rel.filename;
        out += ':';
        std::snprintf(num_buf, sizeof(num_buf), "%u", rel.location.line);
        out += num_buf;
        out += ':';
        std::snprintf(num_buf, sizeof(num_buf), "%u", rel.location.column);
        out += num_buf;
        out += ": ";
        out += RESET;
        out += "note: ";
        out += rel.message;
        out += '\n';
    }

    // Suggested fix
    if (diag.fix) {
        out += "  = help: ";
        out += diag.fix->description;
        out += '\n';
    }

    return out;
}

std::string format_diagnostic_json(const Diagnostic& diag) {
    std::string out;
    out.reserve(256);
    char num_buf[32];

    out += R"({"severity":")";
    out += severity_string(diag.severity);
    out += R"(",)";

    out += R"("code":")";
    out += escape_json(diag.code);
    out += R"(",)";

    out += R"("message":")";
    out += escape_json(diag.message);
    out += R"(",)";

    out += R"("file":")";
    out += escape_json(diag.filename);
    out += R"(",)";

    out += R"("range":{"start":{"line":)";
    std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.line - 1);
    out += num_buf;
    out += R"(,"character":)";
    std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.column - 1);
    out += num_buf;
    out += R"(},)";

    out += R"("end":{"line":)";
    std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.line - 1);
    out += num_buf;
    out += R"(,"character":)";
    std::snprintf(num_buf, sizeof(num_buf), "%u", diag.location.column - 1 + diag.location.length);
    out += num_buf;
    out += R"(}}})";

    return out;
}

bool has_errors(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

} // namespace akkado
