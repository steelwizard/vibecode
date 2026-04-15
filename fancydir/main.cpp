#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

namespace fs = std::filesystem;

struct EntryView {
    fs::directory_entry entry;
    bool is_dir;
    bool is_symlink;
    uintmax_t size;
    std::string ext;
    std::string name_lower;
    std::time_t modified;
};

enum class SortKey {
    Name,
    Size,
    Time,
    Type
};

enum class GroupMode {
    DirsFirst,
    FilesFirst,
    None
};

struct Options {
    fs::path target = ".";
    SortKey sort_key = SortKey::Name;
    bool descending = false;
    GroupMode group_mode = GroupMode::DirsFirst;
    bool short_format = false;
};

static std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static std::string human_size(uintmax_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    std::size_t unit_idx = 0;
    while (size >= 1024.0 && unit_idx < (sizeof(units) / sizeof(units[0])) - 1) {
        size /= 1024.0;
        ++unit_idx;
    }

    std::ostringstream out;
    if (unit_idx == 0) {
        out << static_cast<uintmax_t>(size) << ' ' << units[unit_idx];
    } else {
        out << std::fixed << std::setprecision(1) << size << ' ' << units[unit_idx];
    }
    return out.str();
}

static std::time_t to_time_t(fs::file_time_type ftp) {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        ftp - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sctp);
}

static std::string format_time(std::time_t t) {
    std::tm tm_local{};
    localtime_s(&tm_local, &t);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm_local);
    return buffer;
}

static void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [path] [options]\n"
              << "\n"
              << "List directory contents with colorized long format by default.\n"
              << "Path is optional and defaults to the current directory.\n"
              << "Options:\n"
              << "  --sort <name|size|time|type>  Sort key (default: name)\n"
              << "  --desc, -r                    Reverse sort order\n"
              << "  --dirs-first                  Group directories first (default)\n"
              << "  --files-first                 Group files first\n"
              << "  --no-group                    Do not group dirs/files\n"
              << "  --short, -1                   Short format (one name per line)\n"
              << "  --help, -h                    Show this help message\n";
    std::cout << "\nExamples:\n"
              << "  " << prog_name << "\n"
              << "  " << prog_name << " C:\\\\dev --sort time --desc\n"
              << "  " << prog_name << " . --short --sort type --no-group\n";
}

static bool parse_options(int argc, char* argv[], Options& opts, bool& help_requested) {
    help_requested = false;
    bool path_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            help_requested = true;
            return false;
        }
        if (arg == "--desc" || arg == "-r") {
            opts.descending = true;
            continue;
        }
        if (arg == "--dirs-first") {
            opts.group_mode = GroupMode::DirsFirst;
            continue;
        }
        if (arg == "--files-first") {
            opts.group_mode = GroupMode::FilesFirst;
            continue;
        }
        if (arg == "--no-group") {
            opts.group_mode = GroupMode::None;
            continue;
        }
        if (arg == "--short" || arg == "-1") {
            opts.short_format = true;
            continue;
        }
        if (arg == "--sort") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after --sort\n";
                print_usage(argv[0]);
                return false;
            }
            const std::string value = to_lower(argv[++i]);
            if (value == "name") {
                opts.sort_key = SortKey::Name;
            } else if (value == "size") {
                opts.sort_key = SortKey::Size;
            } else if (value == "time") {
                opts.sort_key = SortKey::Time;
            } else if (value == "type") {
                opts.sort_key = SortKey::Type;
            } else {
                std::cerr << "Invalid sort key: " << value << '\n';
                print_usage(argv[0]);
                return false;
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << '\n';
            print_usage(argv[0]);
            return false;
        }
        if (path_set) {
            std::cerr << "Multiple paths provided. Only one path is supported.\n";
            print_usage(argv[0]);
            return false;
        }
        opts.target = arg;
        path_set = true;
    }
    return true;
}

static WORD style_for(const EntryView& item, bool alt_row) {
    const WORD bg_a = BACKGROUND_BLUE;
    const WORD bg_b = BACKGROUND_GREEN;
    WORD bg = alt_row ? bg_a : bg_b;

    if (item.is_dir) {
        return bg | FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN;
    }
    if (item.is_symlink) {
        return bg | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE;
    }

    const std::string ext = to_lower(item.ext);
    if (ext == ".exe" || ext == ".com" || ext == ".bat" || ext == ".cmd") {
        return bg | FOREGROUND_INTENSITY | FOREGROUND_GREEN;
    }
    if (ext == ".cpp" || ext == ".c" || ext == ".h" || ext == ".hpp") {
        return bg | FOREGROUND_INTENSITY | FOREGROUND_BLUE;
    }
    if (ext == ".md" || ext == ".txt") {
        return bg | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN;
    }

    return bg | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
}

int main(int argc, char* argv[]) {
    Options options;
    bool help_requested = false;
    if (!parse_options(argc, argv, options, help_requested)) {
        return help_requested ? 0 : 1;
    }

    const fs::path dir_path(options.target);
    if (!fs::exists(dir_path)) {
        std::cerr << "Path does not exist: " << dir_path.string() << '\n';
        return 1;
    }
    if (!fs::is_directory(dir_path)) {
        std::cerr << "Path is not a directory: " << dir_path.string() << '\n';
        return 1;
    }

    std::vector<EntryView> items;
    items.reserve(256);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        if (ec) {
            std::cerr << "Failed to read directory entry: " << ec.message() << '\n';
            return 1;
        }

        EntryView view{
            entry,
            entry.is_directory(),
            entry.is_symlink(),
            0,
            to_lower(entry.path().extension().string()),
            to_lower(entry.path().filename().string()),
            0
        };

        auto write_time = entry.last_write_time(ec);
        if (!ec) {
            view.modified = to_time_t(write_time);
        } else {
            view.modified = 0;
            ec.clear();
        }

        if (!view.is_dir) {
            view.size = entry.file_size(ec);
            if (ec) {
                view.size = 0;
                ec.clear();
            }
        }

        items.push_back(std::move(view));
    }

    std::sort(items.begin(), items.end(), [&](const EntryView& a, const EntryView& b) {
        if (options.group_mode != GroupMode::None && a.is_dir != b.is_dir) {
            if (options.group_mode == GroupMode::DirsFirst) {
                return a.is_dir > b.is_dir;
            }
            return a.is_dir < b.is_dir;
        }

        auto compare = [&](const auto& lhs, const auto& rhs) {
            if (lhs < rhs) {
                return -1;
            }
            if (rhs < lhs) {
                return 1;
            }
            if (a.name_lower < b.name_lower) {
                return -1;
            }
            if (b.name_lower < a.name_lower) {
                return 1;
            }
            return 0;
        };

        int cmp = 0;
        switch (options.sort_key) {
            case SortKey::Name:
                cmp = compare(a.name_lower, b.name_lower);
                break;
            case SortKey::Size:
                cmp = compare(a.size, b.size);
                break;
            case SortKey::Time:
                cmp = compare(a.modified, b.modified);
                break;
            case SortKey::Type:
                cmp = compare(a.ext, b.ext);
                break;
        }
        return options.descending ? (cmp > 0) : (cmp < 0);
    });

    if (options.short_format) {
        for (const EntryView& item : items) {
            std::string name = item.entry.path().filename().string();
            if (item.is_dir) {
                name += '/';
            } else if (item.is_symlink) {
                name += '@';
            }
            std::cout << name << '\n';
        }
        return 0;
    }

    HANDLE hconsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO buffer_info{};
    GetConsoleScreenBufferInfo(hconsole, &buffer_info);
    const WORD default_style = buffer_info.wAttributes;

    SetConsoleTextAttribute(
        hconsole,
        BACKGROUND_RED | BACKGROUND_BLUE | BACKGROUND_INTENSITY |
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    std::cout << " FANCYDIR  ";
    SetConsoleTextAttribute(hconsole, default_style);
    std::cout << " " << fs::absolute(dir_path).string() << "\n\n";

    SetConsoleTextAttribute(
        hconsole,
        BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
            FOREGROUND_INTENSITY);
    std::cout << std::left << std::setw(18) << "Modified"
              << std::setw(14) << "Type"
              << std::setw(12) << "Size"
              << "Name\n";
    SetConsoleTextAttribute(hconsole, default_style);

    std::size_t dir_count = 0;
    std::size_t file_count = 0;
    uintmax_t bytes_total = 0;

    for (std::size_t i = 0; i < items.size(); ++i) {
        const EntryView& item = items[i];
        SetConsoleTextAttribute(hconsole, style_for(item, (i % 2) == 0));

        const std::string type = item.is_dir ? "<DIR>" : (item.is_symlink ? "<LINK>" : "<FILE>");
        const std::string size = item.is_dir ? "-" : human_size(item.size);

        std::string name = item.entry.path().filename().string();
        if (item.is_dir) {
            name += '\\';
            ++dir_count;
        } else {
            ++file_count;
            bytes_total += item.size;
        }

        std::cout << std::left << std::setw(18)
                  << (item.modified == 0 ? "unknown" : format_time(item.modified))
                  << std::setw(14) << type
                  << std::setw(12) << size
                  << name << '\n';
    }

    std::ostringstream summary;
    summary << dir_count << " dir(s), "
            << file_count << " file(s), "
            << human_size(bytes_total) << " total";

    const WORD summary_style = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
    SetConsoleTextAttribute(hconsole, default_style);
    std::cout << "\n";
    SetConsoleTextAttribute(hconsole, summary_style);
    std::cout << " " << summary.str() << " ";
    SetConsoleTextAttribute(hconsole, default_style);
    std::cout << "\n";

    return 0;
}
