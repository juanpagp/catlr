#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <io.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

struct Config {
    std::string tree_command = "tree";
    std::string file_command = "bat";
};

struct Filters {
    std::vector<std::string> print_includes;
    std::vector<std::string> print_excludes;
    std::vector<std::string> list_includes;
    std::vector<std::string> list_excludes;
};

// Safe for empty strings and whitespace-only input; cast to unsigned char before isspace.
std::string trim(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return std::string(first, last);
}

std::string normalize_slashes(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

// Supports '*' (any sequence) and '?' (one character). This is deliberately a small,
// predictable glob implementation rather than a claim of full .gitignore/POSIX glob support.
bool glob_matches(const std::string& pattern, const std::string& text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t last_star = std::string::npos;
    std::size_t star_match = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            last_star = p++;
            star_match = t;
        } else if (last_star != std::string::npos) {
            p = last_star + 1;
            t = ++star_match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool starts_with_path(const std::string& path, const std::string& prefix) {
    return path == prefix || (path.size() > prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/');
}

bool pattern_matches(const std::string& raw_relative_path,
                     const std::string& raw_filename,
                     std::string pattern,
                     bool is_directory) {
    if (pattern.empty()) {
        return false;
    }

    std::string relative_path = normalize_slashes(raw_relative_path);
    std::string filename = normalize_slashes(raw_filename);
    pattern = normalize_slashes(pattern);

    const bool anchored = pattern.front() == '/';
    if (anchored) {
        pattern.erase(pattern.begin());
    }
    const bool directory_only = !pattern.empty() && pattern.back() == '/';
    if (directory_only) {
        pattern.pop_back();
    }
    if (pattern.empty()) {
        return false;
    }

    const bool has_slash = pattern.find('/') != std::string::npos;
    if (directory_only) {
        // Directory-only ignores apply to the directory and everything below it.
        if (anchored || has_slash) {
            return starts_with_path(relative_path, pattern);
        }

        // An unanchored directory-only pattern can match a directory name at any depth.
        std::size_t begin = 0;
        while (begin < relative_path.size()) {
            const std::size_t slash = relative_path.find('/', begin);
            const bool is_final_segment = slash == std::string::npos;
            const std::string segment = relative_path.substr(
                begin, is_final_segment ? std::string::npos : slash - begin);
            if ((!is_final_segment || is_directory) && glob_matches(pattern, segment)) {
                return true;
            }
            if (is_final_segment) {
                break;
            }
            begin = slash + 1;
        }
        return false;
    }

    if (anchored || has_slash) {
        return glob_matches(pattern, relative_path);
    }
    // Basename patterns like '*.cpp' and 'build' work at any directory depth.
    return glob_matches(pattern, filename);
}

bool matches_filters(const fs::path& path,
                    const fs::path& base_path,
                    const std::vector<std::string>& includes,
                    const std::vector<std::string>& excludes,
                    bool is_directory) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, base_path, ec);
    if (ec) {
        return false;
    }
    const std::string relative_string = normalize_slashes(relative.generic_string());
    const std::string filename = path.filename().generic_string();

    // Explicit includes take priority over excludes, matching the CLI documentation.
    for (const auto& pattern : includes) {
        if (pattern_matches(relative_string, filename, pattern, is_directory)) {
            return true;
        }
    }
    for (const auto& pattern : excludes) {
        if (pattern_matches(relative_string, filename, pattern, is_directory)) {
            return false;
        }
    }
    return includes.empty();
}

// If a directory itself doesn't match an include, still visit it when an include could match
// one of its descendants (e.g. -i build/main.cpp or -i '*.cpp'). Conservative for globs.
bool may_match_included_descendant(const fs::path& directory,
                                   const fs::path& base_path,
                                   const std::vector<std::string>& includes) {
    if (includes.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path relative = fs::relative(directory, base_path, ec);
    if (ec) {
        return true; // Better to traverse than accidentally hide a requested file.
    }
    const std::string dir = normalize_slashes(relative.generic_string());

    for (std::string pattern : includes) {
        if (pattern.empty()) {
            continue;
        }
        pattern = normalize_slashes(pattern);
        if (pattern.front() == '/') {
            pattern.erase(pattern.begin());
        }
        while (!pattern.empty() && pattern.back() == '/') {
            pattern.pop_back();
        }
        if (pattern.empty()) {
            continue;
        }
        if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos ||
            pattern.find('/') == std::string::npos) {
            return true;
        }
        if (pattern.size() > dir.size() && pattern.compare(0, dir.size(), dir) == 0 &&
            pattern[dir.size()] == '/') {
            return true;
        }
    }
    return false;
}

fs::path get_home_path() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr) {
        home = std::getenv("HOMEDRIVE");
        const char* home_path = std::getenv("HOMEPATH");
        if (home != nullptr && home_path != nullptr) {
            return fs::path(std::string(home) + home_path);
        }
    }
#else
    const char* home = std::getenv("HOME");
#endif
    return home == nullptr ? fs::path{} : fs::path(home);
}

Config parse_config() {
    Config config;
    const fs::path home = get_home_path();
    if (home.empty()) {
        return config;
    }

    std::ifstream file(home / ".config" / "catlr" / "catlr.conf");
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (value.empty()) {
            continue;
        }
        if (key == "treePrintCommand") {
            config.tree_command = value;
        } else if (key == "filePrintCommand") {
            config.file_command = value;
        }
    }
    return config;
}

std::vector<std::string> parse_gitignore(const fs::path& gitignore_path) {
    std::vector<std::string> patterns;
    std::ifstream file(gitignore_path);
    if (!file) {
        return patterns;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line); // Blank and whitespace-only lines are safe.
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '!') {
            // Negation is intentionally unsupported for now; document this limitation.
            continue;
        }
        // Preserve a leading slash so pattern_matches can enforce root anchoring.
        patterns.push_back(normalize_slashes(line));
    }
    return patterns;
}

std::string process_pattern_arg(std::string pattern) {
    // Command-line shorthand: '.cpp' means '*.cpp'. Do not use for .gitignore entries.
    if (pattern.size() > 1 && pattern.front() == '.' &&
        pattern.find('*') == std::string::npos && pattern.find('?') == std::string::npos &&
        pattern.find('/') == std::string::npos && pattern.find('\\') == std::string::npos) {
        return "*" + pattern;
    }
    return pattern;
}

void print_file_native(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "[Could not open file: " << path.string() << "]\n";
        return;
    }
    std::cout << file.rdbuf();
    if (file.bad()) {
        std::cerr << "[Error while reading file: " << path.string() << "]\n";
    }
}

void print_tree_recursive(const fs::path& directory,
                          const fs::path& base_path,
                          const std::string& prefix,
                          const Filters& filters) {
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::directory_entry entry = *it;
        std::error_code type_ec;
        const bool is_symlink = entry.is_symlink(type_ec);
        if (type_ec) {
            continue;
        }
        const bool is_directory = !is_symlink && entry.is_directory(type_ec);
        if (type_ec) {
            continue;
        }
        bool visible = matches_filters(entry.path(), base_path,
                                       filters.list_includes, filters.list_excludes,
                                       is_directory);
        if (!visible && is_directory) {
            visible = may_match_included_descendant(entry.path(), base_path, filters.list_includes);
        }
        if (visible) {
            entries.push_back(entry);
        }
    }
    if (ec) {
        std::cerr << "Warning: Could not fully read directory '" << directory.string()
                  << "': " << ec.message() << '\n';
    }

    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& left,
                                                  const fs::directory_entry& right) {
        return left.path().filename().generic_string() < right.path().filename().generic_string();
    });

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const bool is_last = index + 1 == entries.size();
        std::error_code type_ec;
        const bool is_symlink = entry.is_symlink(type_ec);
        if (type_ec) {
            continue;
        }
        const bool is_directory = !is_symlink && entry.is_directory(type_ec);
        if (type_ec) {
            continue;
        }

        std::cout << prefix << (is_last ? "└── " : "├── ")
                  << entry.path().filename().generic_string();
        if (is_directory) {
            std::cout << "/\n";
            const std::string new_prefix = prefix + (is_last ? "    " : "│   ");
            print_tree_recursive(entry.path(), base_path, new_prefix, filters);
        } else {
            std::cout << '\n';
        }
    }
}

void print_tree_native(const fs::path& path, const Filters& filters) {
    std::cout << path.filename().generic_string() << "/\n";
    print_tree_recursive(path, path, "", filters);
}

// A command name is checked against PATH without invoking a shell.
bool command_exists(const std::string& command) {
    const std::size_t whitespace = command.find_first_of(" \t");
    const std::string executable = command.substr(0, whitespace);
    if (executable.empty()) {
        return false;
    }

    const fs::path executable_path(executable);
    auto is_executable = [](const fs::path& candidate) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) || ec) {
            return false;
        }
#ifndef _WIN32
        return ::access(candidate.c_str(), X_OK) == 0;
#else
        return true;
#endif
    };

    if (executable_path.has_parent_path() || executable_path.is_absolute()) {
        return is_executable(executable_path);
    }

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return false;
    }
#ifdef _WIN32
    constexpr char separator = ';';
    const char* path_ext_env = std::getenv("PATHEXT");
    const std::string extensions = path_ext_env ? path_ext_env : ".EXE;.COM;.BAT;.CMD";
#else
    constexpr char separator = ':';
#endif
    const std::string search_path(path_env);
    std::size_t begin = 0;
    while (begin <= search_path.size()) {
        const std::size_t end = search_path.find(separator, begin);
        const std::string entry = search_path.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        const fs::path directory = entry.empty() ? fs::path(".") : fs::path(entry);
#ifdef _WIN32
        if (is_executable(directory / executable)) {
            return true;
        }
        std::size_t ext_begin = 0;
        while (ext_begin <= extensions.size()) {
            const std::size_t ext_end = extensions.find(';', ext_begin);
            const std::string ext = extensions.substr(ext_begin,
                ext_end == std::string::npos ? std::string::npos : ext_end - ext_begin);
            if (!ext.empty() && is_executable(directory / (executable + ext))) {
                return true;
            }
            if (ext_end == std::string::npos) break;
            ext_begin = ext_end + 1;
        }
#else
        if (is_executable(directory / executable)) {
            return true;
        }
#endif
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

#ifndef _WIN32
// Quote a single shell argument for POSIX shells (paths can contain spaces, quotes, and metacharacters).
std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += '\'';
    return quoted;
}
#endif

bool looks_like_extension_pattern(const std::string& value) {
    if (value.size() <= 1 || value.front() != '.' ||
        value.find('/') != std::string::npos || value.find('\\') != std::string::npos) {
        return false;
    }
    std::error_code ec;
    return !fs::exists(fs::path(value), ec) && !ec;
}

void show_usage(const char* program) {
    std::cerr
        << "Usage: " << program << " [directory_path...] [filter_rules...]\n\n"
        << "Directory paths must come before flags. With no flags, a simple extension argument\n"
        << "after the first path (for example '.cpp') is treated as a print filter.\n\n"
        << "Filtering options (supports '*' and '?' wildcards):\n"
        << "  -e,  --exclude <patterns...>       Exclude from both tree and file output\n"
        << "  -i,  --include <patterns...>       Include in both; explicit includes override excludes\n"
        << "  -li, -il, --list-include <p...>    Include matching paths in the tree\n"
        << "  -le, -el, --list-exclude <p...>    Exclude matching paths from the tree\n"
        << "  -pi, -ip, --print-include <p...>   Print only matching files\n"
        << "  -pe, -ep, --print-exclude <p...>   Do not print matching files\n"
        << "  --no-gitignore                     Ignore the target root's .gitignore\n"
        << "  -h, --help                         Show this help\n\n"
        << "Examples:\n"
        << "  " << program << " . -ip .cpp .h\n"
        << "  " << program << " project -e build/ .o .a\n"
        << "  " << program << " project -i build/main.cpp\n"
        << "  " << program << " . .txt .md\n\n"
        << "Note: .gitignore negation rules ('!pattern') and nested .gitignore files are not supported.\n";
}

int main(int argc, char* argv[]) {
#ifndef _WIN32
    ino_t stdout_inode = 0;
    dev_t stdout_dev = 0;
    if (!isatty(STDOUT_FILENO)) {
        struct stat stdout_stat {};
        if (fstat(STDOUT_FILENO, &stdout_stat) == 0 && S_ISREG(stdout_stat.st_mode)) {
            stdout_inode = stdout_stat.st_ino;
            stdout_dev = stdout_stat.st_dev;
        }
    }
#endif

    int first_flag_idx = argc;
    bool respect_gitignore = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            show_usage(argv[0]);
            return 0;
        }
        if (!arg.empty() && arg.front() == '-' && arg.size() > 1 && first_flag_idx == argc) {
            first_flag_idx = i;
        }
        if (arg == "--no-gitignore") {
            respect_gitignore = false;
        }
    }

    // Preserve the documented shorthand: `catlr . .txt .md`.
    if (first_flag_idx == argc) {
        for (int i = 2; i < argc; ++i) {
            if (looks_like_extension_pattern(argv[i])) {
                first_flag_idx = i;
                break;
            }
        }
    }

    std::vector<fs::path> target_paths;
    for (int i = 1; i < first_flag_idx; ++i) {
        target_paths.emplace_back(argv[i]);
    }
    if (target_paths.empty()) {
        target_paths.emplace_back(".");
    }

    Filters filters;
    for (int i = first_flag_idx; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto consume_patterns = [&](std::vector<std::string>& destination) {
            while (i + 1 < argc && (argv[i + 1][0] != '-' || argv[i + 1][1] == '\0')) {
                ++i;
                const std::string pattern = process_pattern_arg(argv[i]);
                if (!pattern.empty()) {
                    destination.push_back(pattern);
                }
            }
        };

        if (arg == "-e" || arg == "--exclude") {
            const std::size_t old_size = filters.list_excludes.size();
            consume_patterns(filters.list_excludes);
            filters.print_excludes.insert(filters.print_excludes.end(),
                filters.list_excludes.begin() + static_cast<std::ptrdiff_t>(old_size),
                filters.list_excludes.end());
        } else if (arg == "-i" || arg == "--include") {
            const std::size_t old_size = filters.list_includes.size();
            consume_patterns(filters.list_includes);
            filters.print_includes.insert(filters.print_includes.end(),
                filters.list_includes.begin() + static_cast<std::ptrdiff_t>(old_size), filters.list_includes.end());
        } else if (arg == "-li" || arg == "--list-include" || arg == "-il") {
            consume_patterns(filters.list_includes);
        } else if (arg == "-le" || arg == "--list-exclude" || arg == "-el") {
            consume_patterns(filters.list_excludes);
        } else if (arg == "-pi" || arg == "--print-include" || arg == "-ip") {
            consume_patterns(filters.print_includes);
        } else if (arg == "-pe" || arg == "--print-exclude" || arg == "-ep") {
            consume_patterns(filters.print_excludes);
        } else if (arg == "--no-gitignore") {
            continue;
        } else if (!arg.empty() && arg.front() == '.') {
            filters.print_includes.push_back(process_pattern_arg(arg));
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Warning: unknown flag '" << arg << "'. Ignoring.\n";
        }
    }

    Config config = parse_config();
#ifdef _WIN32
    // Native implementations are used on Windows; the external-command path is POSIX-shell based.
    const bool use_external_tree = false;
    const bool use_configured_file_cmd = false;
    const bool use_cat = false;
#else
    const bool use_external_tree = command_exists(config.tree_command);
    const bool use_configured_file_cmd = command_exists(config.file_command);
    const bool use_cat = !use_configured_file_cmd && command_exists("cat");
#endif

    int exit_code = 0;
    for (const auto& path_entry : target_paths) {
        std::error_code path_ec;
        const fs::path target_path = fs::canonical(path_entry, path_ec);
        if (path_ec) {
            std::cerr << "Error: could not resolve path '" << path_entry.string() << "': "
                      << path_ec.message() << '\n';
            exit_code = 1;
            continue;
        }
        if (!fs::is_directory(target_path, path_ec) || path_ec) {
            std::cerr << "Error: target path is not a readable directory: '" << target_path.string() << "'\n";
            exit_code = 1;
            continue;
        }

        Filters path_filters = filters;
        if (respect_gitignore) {
            const fs::path ignore_path = target_path / ".gitignore";
            if (fs::exists(ignore_path, path_ec) && !path_ec) {
                for (const auto& pattern : parse_gitignore(ignore_path)) {
                    if (!pattern.empty()) {
                        path_filters.list_excludes.push_back(pattern);
                        path_filters.print_excludes.push_back(pattern);
                    }
                }
            }
        }

        std::cout << "--- Directory Tree for: " << target_path.filename().generic_string() << " ---\n"
                  << "Located at: " << target_path.string() << "\n\n";
        bool rendered_external_tree = false;
#ifndef _WIN32
        if (use_external_tree && path_filters.list_includes.empty() && path_filters.list_excludes.empty()) {
            const std::string command = config.tree_command + " " + shell_quote(target_path.string());
            rendered_external_tree = (std::system(command.c_str()) == 0);
        }
#endif
        if (!rendered_external_tree) {
            if (use_external_tree && (!path_filters.list_includes.empty() || !path_filters.list_excludes.empty())) {
                std::cout << "Info: external tree command cannot apply filters; using built-in tree.\n";
            } else if (!use_external_tree) {
                std::cout << "Info: external tree command unavailable; using built-in tree.\n";
            }
            print_tree_native(target_path, path_filters);
        }
        std::cout << '\n' << "--- File Contents (Recursive) for: "
                  << target_path.filename().generic_string() << " ---\n";

        std::error_code iter_ec;
        fs::recursive_directory_iterator it(
            target_path, fs::directory_options::skip_permission_denied, iter_ec), end;
        while (!iter_ec && it != end) {
            const fs::directory_entry entry = *it;
            std::error_code type_ec;
            const bool is_directory = entry.is_directory(type_ec);
            if (!type_ec && is_directory) {
                const bool matches = matches_filters(entry.path(), target_path,
                    path_filters.list_includes, path_filters.list_excludes, true);
                if (!matches && !may_match_included_descendant(entry.path(), target_path,
                                                                path_filters.list_includes)) {
                    it.disable_recursion_pending();
                }
            } else if (!type_ec && entry.is_regular_file(type_ec) && !type_ec &&
                       matches_filters(entry.path(), target_path,
                           path_filters.print_includes, path_filters.print_excludes, false)) {
                const fs::path current_path = entry.path();
                const fs::path relative_path = fs::relative(current_path, target_path, type_ec);
                if (type_ec) {
                    std::cerr << "Warning: could not calculate relative path for '"
                              << current_path.string() << "'.\n";
                } else {
                    bool is_program_output = false;
#ifndef _WIN32
                    if (stdout_inode != 0) {
                        struct stat file_stat {};
                        if (stat(current_path.c_str(), &file_stat) == 0 &&
                            file_stat.st_ino == stdout_inode && file_stat.st_dev == stdout_dev) {
                            is_program_output = true;
                        }
                    }
#endif
                    if (is_program_output) {
                        std::cerr << "--- " << relative_path.generic_string() << " ---\n"
                                  << "[Warning: skipping program output file to avoid an I/O loop]\n";
                    } else {
                        std::cout << "--- " << relative_path.generic_string() << " ---\n";
                        bool rendered_external_file = false;
#ifndef _WIN32
                        std::string command;
                        if (use_configured_file_cmd) {
                            command = config.file_command;
                            if (config.file_command == "bat") {
                                command += " --paging=never --style=full";
                            }
                            command += " " + shell_quote(current_path.string());
                            rendered_external_file = (std::system(command.c_str()) == 0);
                        } else if (use_cat) {
                            command = "cat " + shell_quote(current_path.string());
                            rendered_external_file = (std::system(command.c_str()) == 0);
                        }
#endif
                        if (!rendered_external_file) {
                            print_file_native(current_path);
                        }
                        std::cout << '\n';
                    }
                }
            }
            it.increment(iter_ec);
            if (iter_ec) {
                std::cerr << "Warning: traversal stopped early under '" << target_path.string()
                          << "': " << iter_ec.message() << '\n';
                exit_code = 1;
            }
        }
    }

    std::cout << "--- End of Listing ---\n";
    return exit_code;
}
