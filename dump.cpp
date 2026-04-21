#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

enum class FilterMode {
    Blacklist,
    Whitelist,
};

struct Config {
    FilterMode filter_mode = FilterMode::Blacklist;
    fs::path work_directory = ".";
    std::unordered_set<std::string> blacklisted_files;
    std::unordered_set<std::string> blacklisted_directories;
    std::unordered_set<std::string> whitelisted_files;
    std::unordered_set<std::string> whitelisted_directories;
};

struct CliOptions {
    bool show_help = false;
    std::vector<std::string> directory_inputs;
};

struct ListedEntry {
    fs::path path;
    std::string name;
    std::string sort_key;
    bool is_directory = false;
    std::vector<ListedEntry> children;
};

struct FileReadResult {
    enum class Kind {
        Text,
        Binary,
        Error,
    };

    Kind kind = Kind::Error;
    std::string payload;
};

struct RootDumpChunk {
    std::string markdown;
};

constexpr std::size_t kBinarySampleSize = 4096;

std::string trim_copy(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::string to_lower_ascii(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());

    for (unsigned char character : value) {
        lower.push_back(static_cast<char>(std::tolower(character)));
    }

    return lower;
}

void add_csv_values(std::unordered_set<std::string>& target, std::string_view values) {
    std::size_t cursor = 0;

    while (cursor <= values.size()) {
        const std::size_t comma = values.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos ? values.size() : comma;
        std::string value = trim_copy(values.substr(cursor, end - cursor));
        if (!value.empty()) {
            target.insert(std::move(value));
        }

        if (comma == std::string_view::npos) {
            break;
        }

        cursor = comma + 1;
    }
}

fs::path get_executable_dir() {
    std::vector<char> buffer(256);

    while (true) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length < 0) {
            throw std::runtime_error("Unable to resolve executable path.");
        }

        if (static_cast<std::size_t>(length) < buffer.size() - 1) {
            buffer[static_cast<std::size_t>(length)] = '\0';
            return fs::path(buffer.data()).parent_path();
        }

        buffer.resize(buffer.size() * 2);
    }
}

FilterMode parse_filter_mode(std::string_view value) {
    const std::string normalized = to_lower_ascii(trim_copy(value));
    if (normalized.empty() || normalized == "blacklist") {
        return FilterMode::Blacklist;
    }

    if (normalized == "whitelist") {
        return FilterMode::Whitelist;
    }

    throw std::runtime_error("Invalid filter_mode in config.ini: " + std::string(value));
}

fs::path resolve_path_from_base(const fs::path& base_directory, const fs::path& candidate) {
    const fs::path combined = candidate.is_absolute() ? candidate : base_directory / candidate;

    std::error_code canonical_error;
    const fs::path normalized = fs::weakly_canonical(combined, canonical_error);
    return canonical_error ? combined.lexically_normal() : normalized;
}

std::string format_path_for_display(const fs::path& path, const fs::path& base_directory) {
    const fs::path relative = path.lexically_relative(base_directory);
    const std::string relative_string = relative.generic_string();

    if (relative_string == ".") {
        return "./";
    }

    if (!relative_string.empty() && relative_string.rfind("..", 0) != 0) {
        return "./" + relative_string;
    }

    if (!relative_string.empty()) {
        return relative_string;
    }

    return path.string();
}

bool is_settings_section(const std::string& section) {
    return
        section.empty() || section == "settings" || section == "general" ||
        section == "options" || section == "paths";
}

bool is_blacklist_section(const std::string& section) {
    return
        section.empty() || section == "blacklist" || section == "blacklists";
}

bool is_whitelist_section(const std::string& section) {
    return
        section == "whitelist" || section == "whitelists";
}

Config load_config(const fs::path& config_path, const fs::path& executable_dir) {
    std::ifstream input(config_path);
    if (!input) {
        throw std::runtime_error("Missing config.ini next to the binary: " + config_path.string());
    }

    Config config;
    std::string current_section;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = to_lower_ascii(trim_copy(
                std::string_view(trimmed).substr(1, trimmed.size() - 2)
            ));
            continue;
        }

        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "Invalid config line " + std::to_string(line_number) + ": " + trimmed
            );
        }

        const std::string key = to_lower_ascii(trim_copy(trimmed.substr(0, separator)));
        const std::string value = trim_copy(trimmed.substr(separator + 1));
        if (is_settings_section(current_section)) {
            if (key == "filter_mode" || key == "mode") {
                config.filter_mode = parse_filter_mode(value);
                continue;
            }

            if (
                key == "work_directory" || key == "working_directory" ||
                key == "workdir" || key == "workingdir" ||
                key == "relative_work_directory" || key == "relative_working_directory" ||
                key == "relative_workdir" || key == "relative_workingdir"
            ) {
                config.work_directory = value.empty() ? fs::path(".") : fs::path(value);
                continue;
            }
        }

        if (is_blacklist_section(current_section)) {
            if (key == "files" || key == "file" || key == "blacklisted_files") {
                add_csv_values(config.blacklisted_files, value);
                continue;
            }

            if (
                key == "directories" || key == "directory" || key == "folders" ||
                key == "folder" || key == "blacklisted_directories" || key == "blacklisted_folders"
            ) {
                add_csv_values(config.blacklisted_directories, value);
                continue;
            }
        }

        if (is_whitelist_section(current_section)) {
            if (key == "files" || key == "file" || key == "whitelisted_files") {
                add_csv_values(config.whitelisted_files, value);
                continue;
            }

            if (
                key == "directories" || key == "directory" || key == "folders" ||
                key == "folder" || key == "whitelisted_directories" || key == "whitelisted_folders"
            ) {
                add_csv_values(config.whitelisted_directories, value);
                continue;
            }
        }
    }

    config.work_directory = resolve_path_from_base(executable_dir, config.work_directory);
    return config;
}

std::string display_name_for_path(const fs::path& path) {
    std::string name = path.filename().string();
    if (!name.empty()) {
        return name;
    }

    name = path.root_name().string() + path.root_directory().string();
    if (!name.empty()) {
        return name;
    }

    return "root";
}

std::vector<std::string> build_root_labels(const std::vector<fs::path>& directories) {
    std::unordered_map<std::string, int> label_counts;
    std::vector<std::string> labels;
    labels.reserve(directories.size());

    for (const fs::path& directory : directories) {
        std::string label = display_name_for_path(directory);
        const int count = ++label_counts[label];
        if (count > 1) {
            label += "_" + std::to_string(count);
        }

        labels.push_back(std::move(label));
    }

    return labels;
}

std::size_t worker_count_for(std::size_t item_count) {
    if (item_count <= 1) {
        return 1;
    }

    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    const std::size_t default_threads = hardware_threads == 0 ? 4 : hardware_threads;
    return std::min(item_count, default_threads);
}

template <typename Result, typename Builder>
std::vector<Result> parallel_collect_ordered(std::size_t item_count, Builder&& builder) {
    std::vector<Result> results(item_count);
    if (item_count == 0) {
        return results;
    }

    const std::size_t worker_count = worker_count_for(item_count);
    if (worker_count == 1) {
        for (std::size_t index = 0; index < item_count; ++index) {
            results[index] = builder(index);
        }
        return results;
    }

    std::atomic<std::size_t> next_index{0};
    std::atomic<bool> cancelled{false};
    std::exception_ptr first_exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        workers.emplace_back([&]() {
            while (!cancelled.load(std::memory_order_relaxed)) {
                const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= item_count) {
                    return;
                }

                try {
                    results[index] = builder(index);
                } catch (...) {
                    cancelled.store(true, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> guard(exception_mutex);
                    if (!first_exception) {
                        first_exception = std::current_exception();
                    }
                    return;
                }
            }
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    return results;
}

std::string language_from_path(const fs::path& path) {
    std::string extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }

    return extension.empty() ? "text" : extension;
}

bool has_known_binary_extension(const fs::path& path) {
    static const std::unordered_set<std::string> binary_extensions = {
        ".7z", ".a", ".apk", ".avif", ".bin", ".bmp", ".bz2", ".class", ".dat", ".dll",
        ".dmg", ".doc", ".docx", ".eot", ".exe", ".gif", ".gz", ".ico", ".iso", ".jar",
        ".jpeg", ".jpg", ".lib", ".lockb", ".mp3", ".mp4", ".o", ".obj", ".otf", ".pdf",
        ".png", ".pyc", ".pyd", ".rar", ".so", ".tar", ".tif", ".tiff", ".ttf", ".wasm",
        ".webm", ".webp", ".woff", ".woff2", ".xz", ".zip",
    };

    return binary_extensions.contains(to_lower_ascii(path.extension().string()));
}

bool is_probably_binary(std::string_view sample) {
    if (sample.find('\0') != std::string_view::npos) {
        return true;
    }

    std::size_t control_count = 0;
    for (unsigned char character : sample) {
        if (character < 32 && character != '\n' && character != '\r' && character != '\t' && character != '\f') {
            ++control_count;
        }
    }

    return !sample.empty() && (control_count * 5 > sample.size());
}

bool is_probably_text_file(const fs::path& file_path) {
    if (has_known_binary_extension(file_path)) {
        return false;
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        return true;
    }

    std::string sample(kBinarySampleSize, '\0');
    input.read(sample.data(), static_cast<std::streamsize>(sample.size()));
    sample.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad()) {
        return true;
    }

    return !is_probably_binary(sample);
}

FileReadResult read_file_contents(const fs::path& file_path) {
    if (has_known_binary_extension(file_path)) {
        return {FileReadResult::Kind::Binary, {}};
    }

    std::error_code size_error;
    const std::uintmax_t raw_size = fs::file_size(file_path, size_error);
    if (size_error) {
        return {FileReadResult::Kind::Error, "Error reading file: " + size_error.message()};
    }

    if (raw_size > static_cast<std::uintmax_t>(std::string().max_size())) {
        return {FileReadResult::Kind::Error, "Error reading file: file is too large."};
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        const int open_errno = errno;
        return {
            FileReadResult::Kind::Error,
            "Error reading file: " + std::system_category().message(open_errno)
        };
    }

    const std::size_t file_size = static_cast<std::size_t>(raw_size);
    const std::size_t sample_size = static_cast<std::size_t>(
        std::min<std::uintmax_t>(raw_size, kBinarySampleSize)
    );
    std::string sample(sample_size, '\0');
    if (sample_size > 0) {
        input.read(sample.data(), static_cast<std::streamsize>(sample.size()));
        sample.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad()) {
            return {FileReadResult::Kind::Error, "Error reading file."};
        }
    }

    if (is_probably_binary(sample)) {
        return {FileReadResult::Kind::Binary, {}};
    }

    if (sample.size() == file_size) {
        return {FileReadResult::Kind::Text, std::move(sample)};
    }

    std::string data(file_size, '\0');
    if (!sample.empty()) {
        std::memcpy(data.data(), sample.data(), sample.size());
    }

    const std::size_t remaining_size = data.size() - sample.size();
    if (remaining_size > 0) {
        input.read(
            data.data() + static_cast<std::ptrdiff_t>(sample.size()),
            static_cast<std::streamsize>(remaining_size)
        );
        data.resize(sample.size() + static_cast<std::size_t>(input.gcount()));
        if (input.bad()) {
            return {FileReadResult::Kind::Error, "Error reading file."};
        }
    }

    return {FileReadResult::Kind::Text, std::move(data)};
}

bool should_include_file_name(const Config& config, const std::string& name) {
    if (config.filter_mode == FilterMode::Blacklist) {
        return !config.blacklisted_files.contains(name);
    }

    return config.whitelisted_files.contains(name);
}

bool should_include_directory_name(const Config& config, const std::string& name) {
    if (config.filter_mode == FilterMode::Blacklist) {
        return !config.blacklisted_directories.contains(name);
    }

    return config.whitelisted_directories.contains(name);
}

std::vector<ListedEntry> collect_visible_entries(
    const fs::path& root,
    const fs::path& directory,
    const Config& config,
    const fs::path& excluded_output_file
) {
    std::vector<ListedEntry> entries;
    std::error_code iteration_error;
    fs::directory_iterator iterator(
        directory,
        fs::directory_options::skip_permission_denied,
        iteration_error
    );

    if (iteration_error) {
        return entries;
    }

    for (const fs::directory_entry& entry : iterator) {
        std::error_code status_error;
        const fs::file_status status = entry.symlink_status(status_error);
        if (status_error || fs::is_symlink(status)) {
            continue;
        }

        const bool is_directory = fs::is_directory(status);
        const bool is_regular_file = fs::is_regular_file(status);
        if (!is_directory && !is_regular_file) {
            continue;
        }

        const fs::path normalized_path = entry.path().lexically_normal();
        const std::string name = normalized_path.filename().string();
        if (is_directory) {
            if (
                config.filter_mode == FilterMode::Blacklist &&
                !should_include_directory_name(config, name)
            ) {
                continue;
            }

            std::vector<ListedEntry> children = collect_visible_entries(
                root,
                normalized_path,
                config,
                excluded_output_file
            );

            if (
                config.filter_mode == FilterMode::Whitelist &&
                !should_include_directory_name(config, name) &&
                children.empty()
            ) {
                continue;
            }

            entries.push_back({
                normalized_path.lexically_relative(root),
                name,
                to_lower_ascii(name),
                true,
                std::move(children),
            });
        } else {
            if (normalized_path == excluded_output_file) {
                continue;
            }
            if (!should_include_file_name(config, name)) {
                continue;
            }
            if (!is_probably_text_file(normalized_path)) {
                continue;
            }

            entries.push_back({
                normalized_path.lexically_relative(root),
                name,
                to_lower_ascii(name),
                false,
                {},
            });
        }
    }

    std::sort(entries.begin(), entries.end(), [](const ListedEntry& left, const ListedEntry& right) {
        if (left.is_directory != right.is_directory) {
            return left.is_directory > right.is_directory;
        }

        if (left.sort_key != right.sort_key) {
            return left.sort_key < right.sort_key;
        }

        return left.name < right.name;
    });

    return entries;
}

void write_tree_recursive(
    const std::vector<ListedEntry>& entries,
    std::ostream& output,
    const std::string& prefix,
    std::vector<fs::path>& files
) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const ListedEntry& entry = entries[index];
        const bool is_last = index + 1 == entries.size();
        const char* connector = is_last ? "└── " : "├── ";
        output << prefix << connector << entry.name;

        if (entry.is_directory) {
            output << "/\n";
            const std::string next_prefix = prefix + (is_last ? "    " : "│   ");
            write_tree_recursive(entry.children, output, next_prefix, files);
            continue;
        }

        output << '\n';
        files.push_back(entry.path);
    }
}

void write_tree_and_collect_files(
    const fs::path& directory,
    const std::vector<ListedEntry>& entries,
    std::ostream& output,
    std::vector<fs::path>& files
) {
    output << display_name_for_path(directory) << "/\n";
    write_tree_recursive(entries, output, "", files);
}

std::string render_file_section(
    const fs::path& root,
    const fs::path& relative_path,
    const std::string& root_label,
    bool include_root_label
) {
    const fs::path file_path = root / relative_path;
    const FileReadResult file_contents = read_file_contents(file_path);
    if (file_contents.kind == FileReadResult::Kind::Binary) {
        return {};
    }

    std::ostringstream output;
    output << "\n### ./";
    if (include_root_label) {
        output << root_label << '/';
    }
    output << relative_path.generic_string() << '\n';
    output << "```" << language_from_path(file_path) << '\n';

    output << file_contents.payload;
    if (file_contents.payload.empty() || file_contents.payload.back() != '\n') {
        output << '\n';
    }
    output << "```\n";
    return output.str();
}

std::vector<std::string> render_file_sections(
    const fs::path& root,
    const std::vector<fs::path>& files,
    const std::string& root_label,
    bool include_root_label,
    bool parallelize
) {
    auto render_section = [&](std::size_t index) {
        return render_file_section(root, files[index], root_label, include_root_label);
    };

    if (!parallelize) {
        std::vector<std::string> sections;
        sections.reserve(files.size());
        for (std::size_t index = 0; index < files.size(); ++index) {
            sections.push_back(render_section(index));
        }
        return sections;
    }

    return parallel_collect_ordered<std::string>(files.size(), render_section);
}

RootDumpChunk build_root_dump(
    const fs::path& directory,
    const std::string& label,
    const Config& config,
    const fs::path& work_directory,
    const fs::path& output_path,
    bool multiple_directories,
    bool parallelize_files
) {
    std::ostringstream output;
    const std::vector<ListedEntry> entries = collect_visible_entries(
        directory,
        directory,
        config,
        output_path
    );
    std::vector<fs::path> files;
    files.reserve(128);

    if (multiple_directories) {
        output << "## " << label << '\n';
        output << "Source: " << format_path_for_display(directory, work_directory) << "\n\n";
    }

    write_tree_and_collect_files(directory, entries, output, files);
    output << '\n';

    const std::vector<std::string> sections = render_file_sections(
        directory,
        files,
        label,
        multiple_directories,
        parallelize_files
    );
    for (const std::string& section : sections) {
        output << section;
    }

    return {output.str()};
}

std::vector<std::string> prompt_directories() {
    std::vector<std::string> directories;
    std::string line;

    while (directories.empty()) {
        std::cout << "Enter one or more directories to code-dump (space or comma separated):\n> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        std::size_t start = 0;
        while (start < line.size()) {
            while (start < line.size() && (std::isspace(static_cast<unsigned char>(line[start])) != 0 || line[start] == ',')) {
                ++start;
            }

            std::size_t end = start;
            while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end])) == 0 && line[end] != ',') {
                ++end;
            }

            if (end > start) {
                directories.emplace_back(line.substr(start, end - start));
            }

            start = end;
        }

        if (directories.empty()) {
            std::cout << "Please enter at least one directory.\n";
        }
    }

    return directories;
}

CliOptions parse_args(int argc, char** argv) {
    CliOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "-h" || argument == "--help") {
            options.show_help = true;
            return options;
        }

        if (argument == "-d" || argument == "--directories") {
            ++index;
            bool consumed_directory = false;
            while (index < argc) {
                const std::string value = argv[index];
                if (!value.empty() && value.front() == '-') {
                    --index;
                    break;
                }

                options.directory_inputs.push_back(value);
                consumed_directory = true;
                ++index;
            }

            if (!consumed_directory) {
                throw std::runtime_error(argument + " requires at least one directory.");
            }

            continue;
        }

        if (!argument.empty() && argument.front() == '-') {
            throw std::runtime_error("Unknown option: " + argument);
        }

        options.directory_inputs.push_back(argument);
    }

    return options;
}

void print_usage(const char* executable_name) {
    std::cout
        << "Usage: " << executable_name << " -d <dir> [dir...]\n"
        << "       " << executable_name << " <dir> [dir...]\n\n"
        << "Writes dump.md next to the binary.\n"
        << "Resolves relative input paths against work_directory from config.ini.\n"
        << "Loads config.ini from the same directory as the binary.\n";
}

std::vector<fs::path> resolve_directories(
    const std::vector<std::string>& directory_inputs,
    const fs::path& base_directory
) {
    std::vector<fs::path> directories;
    directories.reserve(directory_inputs.size());

    for (const std::string& input : directory_inputs) {
        directories.push_back(resolve_path_from_base(base_directory, fs::path(input)));
    }

    return directories;
}

void validate_work_directory(const fs::path& work_directory) {
    std::error_code status_error;
    if (!fs::is_directory(work_directory, status_error) || status_error) {
        throw std::runtime_error("Invalid work_directory in config.ini: " + work_directory.string());
    }
}

void validate_directories(const std::vector<fs::path>& directories) {
    if (directories.empty()) {
        throw std::runtime_error("No directories provided.");
    }

    std::vector<std::string> invalid_paths;
    invalid_paths.reserve(directories.size());

    for (const fs::path& directory : directories) {
        std::error_code status_error;
        if (!fs::is_directory(directory, status_error) || status_error) {
            invalid_paths.push_back(directory.string());
        }
    }

    if (invalid_paths.empty()) {
        return;
    }

    std::string message = "Invalid directory path(s): ";
    for (std::size_t index = 0; index < invalid_paths.size(); ++index) {
        if (index > 0) {
            message += ", ";
        }
        message += invalid_paths[index];
    }

    throw std::runtime_error(message);
}

void write_dump(
    const std::vector<fs::path>& directories,
    const Config& config,
    const fs::path& work_directory,
    const fs::path& output_path,
    std::ostream& output
) {
    const bool multiple_directories = directories.size() > 1;
    const std::vector<std::string> labels = build_root_labels(directories);
    const fs::path normalized_output_path = output_path.lexically_normal();

    if (!multiple_directories) {
        const RootDumpChunk chunk = build_root_dump(
            directories.front(),
            labels.front(),
            config,
            work_directory,
            normalized_output_path,
            false,
            true
        );
        output << chunk.markdown;
        return;
    }

    const std::vector<RootDumpChunk> chunks = parallel_collect_ordered<RootDumpChunk>(
        directories.size(),
        [&](std::size_t index) {
            return build_root_dump(
                directories[index],
                labels[index],
                config,
                work_directory,
                normalized_output_path,
                true,
                false
            );
        }
    );

    for (std::size_t index = 0; index < chunks.size(); ++index) {
        output << chunks[index].markdown;
        if (index + 1 < chunks.size()) {
            output << '\n';
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    try {
        const CliOptions options = parse_args(argc, argv);
        if (options.show_help) {
            print_usage(argv[0]);
            return 0;
        }

        std::vector<std::string> directory_inputs = options.directory_inputs;
        if (directory_inputs.empty()) {
            directory_inputs = prompt_directories();
        }

        const fs::path executable_directory = get_executable_dir();
        const fs::path config_path = executable_directory / "config.ini";
        const Config config = load_config(config_path, executable_directory);
        validate_work_directory(config.work_directory);

        const std::vector<fs::path> directories = resolve_directories(
            directory_inputs,
            config.work_directory
        );
        validate_directories(directories);

        const fs::path output_path = (executable_directory / "dump.md").lexically_normal();
        std::ofstream output;
        std::vector<char> output_buffer(1 << 20);
        output.rdbuf()->pubsetbuf(output_buffer.data(), static_cast<std::streamsize>(output_buffer.size()));
        output.open(output_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to open output file: " + output_path.string());
        }

        write_dump(directories, config, config.work_directory, output_path, output);
        output.flush();
        if (!output) {
            throw std::runtime_error("Failed while writing dump.md.");
        }

        std::cout << "Successfully dumped code from ";
        for (std::size_t index = 0; index < directories.size(); ++index) {
            if (index > 0) {
                std::cout << ", ";
            }
            std::cout << format_path_for_display(directories[index], config.work_directory);
        }
        std::cout << " into\n  " << format_path_for_display(output_path, config.work_directory) << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
