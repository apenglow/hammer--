#include "VmfDocument.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
void usage(const char* executable)
{
    std::cerr << "Usage:\n"
              << "  " << executable << " inspect <map.vmf>\n"
              << "  " << executable << " rewrite <input.vmf> <output.vmf>\n"
              << "  " << executable << " normalize <input.vmf> <output.vmf>\n"
              << "  " << executable << " new <output.vmf>\n";
}

std::optional<hammer::vmf::Document> load(const std::filesystem::path& path)
{
    hammer::vmf::ParseError parseError;
    std::string ioError;
    auto document = hammer::vmf::Document::load(path, &parseError, &ioError);
    if (!document) {
        if (!ioError.empty()) {
            std::cerr << path << ": " << ioError << '\n';
        } else {
            std::cerr << path << ':' << parseError.line << ':' << parseError.column
                      << ": " << parseError.message << '\n';
        }
    }
    return document;
}

bool save(hammer::vmf::Document& document, const std::filesystem::path& path)
{
    std::string error;
    if (!document.save(path, &error)) {
        std::cerr << path << ": " << error << '\n';
        return false;
    }
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string command = argv[1];
    if (command == "inspect" && argc == 3) {
        auto document = load(argv[2]);
        if (!document) return EXIT_FAILURE;
        const auto stats = document->statistics();
        std::cout << "top-level blocks: " << stats.topLevelBlocks << '\n'
                  << "all blocks: " << stats.totalBlocks << '\n'
                  << "key/value pairs: " << stats.keyValues << '\n'
                  << "worlds: " << stats.worlds << '\n'
                  << "entities: " << stats.entities << '\n'
                  << "solids: " << stats.solids << '\n'
                  << "sides: " << stats.sides << '\n'
                  << "displacements: " << stats.displacements << '\n'
                  << "map version: " << stats.mapVersion << '\n'
                  << "format version: " << stats.formatVersion << '\n';
        return EXIT_SUCCESS;
    }

    if ((command == "rewrite" || command == "normalize") && argc == 4) {
        auto document = load(argv[2]);
        if (!document) return EXIT_FAILURE;
        if (command == "normalize") document->markDirty();
        return save(*document, argv[3]) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (command == "new" && argc == 3) {
        auto document = hammer::vmf::Document::createDefault();
        return save(document, argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    usage(argv[0]);
    return EXIT_FAILURE;
}
