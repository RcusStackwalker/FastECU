#include "clang-tidy/ClangTidyForceLinker.h"
#include "clang-tidy/tool/ClangTidyMain.h"

#include <vector>

int main(int argc, char **argv)
{
    std::vector<const char *> arguments(argv, argv + argc);
    return clang::tidy::clangTidyMain(argc, arguments.data());
}
