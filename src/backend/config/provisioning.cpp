#include "src/backend/config/provisioning.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace fastecu::config
{
namespace
{

// Provisioning errors reach the operator at startup, so each names the path
// it failed on; the port's own detail is only the reason.
std::unexpected<Error> AtPath(const Error& error, std::string_view path)
{
    return std::unexpected(Error{error.kind, std::format("{}: {}", path, error.detail)});
}

Status EnsureDirectory(IFileSystem& fs, const std::string& path, IEventSink& events)
{
    if (fs.Exists(path))
    {
        return {};
    }
    if (Status result = fs.MakeDirectory(path); !result.has_value())
    {
        events.Log(LogLevel::kError, std::format("Unable to create directory: {}", path));
        return AtPath(result.error(), path);
    }
    return {};
}

Status CopyBundleIfAbsent(IFileSystem& fs, IResourceBundle& bundle, IFileRepository& file_repository,
                          const std::string& bundle_id, const std::string& target_directory, IEventSink& events)
{
    Result<std::vector<std::string>> names = bundle.List(bundle_id);
    if (!names.has_value())
    {
        return {};
    }
    for (const std::string& name : *names)
    {
        const std::string target = target_directory + name;
        if (fs.Exists(target))
        {
            continue;
        }
        events.Log(LogLevel::kDebug, std::format("Provisioning default file: {}", target));
        // The bytes come from the bundle port itself: the bundle's files are
        // compiled-in resources (Qt ":/..." in production) that no file path
        // the filesystem port understands reaches. Any failure past the
        // already-provisioned check above is a genuine error and stops the
        // sequence.
        Result<std::vector<std::uint8_t>> bytes = bundle.Read(bundle_id, name);
        if (!bytes.has_value())
        {
            events.Log(LogLevel::kError, std::format("Unable to provision default file: {}", target));
            return AtPath(bytes.error(), target);
        }
        if (Status written = file_repository.Write(target, *bytes); !written.has_value())
        {
            events.Log(LogLevel::kError, std::format("Unable to provision default file: {}", target));
            return AtPath(written.error(), target);
        }
    }
    return {};
}

// Directory entries of one kind, newest first.
std::vector<DirEntry> NewestFirst(const std::vector<DirEntry>& entries, bool directories)
{
    std::vector<DirEntry> selected;
    std::ranges::copy_if(entries, std::back_inserter(selected),
                         [directories](const DirEntry& e) { return e.is_directory == directories; });
    std::ranges::sort(selected, std::ranges::greater{}, &DirEntry::modified_time_epoch_seconds);
    return selected;
}

// The config file of the newest sibling version directory, if any.
std::optional<std::string> PreviousConfigFile(IFileSystem& fs, const ConfigPaths& paths)
{
    Result<std::vector<DirEntry>> siblings = fs.ListDirectory(paths.base_config_directory);
    if (!siblings.has_value())
    {
        return std::nullopt;
    }
    const std::vector<DirEntry> dirs = NewestFirst(*siblings, true);
    if (dirs.empty())
    {
        return std::nullopt;
    }
    return paths.base_config_directory + "/" + dirs.front().name + "/config/fastecu.cfg";
}

// Creates a new version directory, carrying over the newest previous
// version's config file.
Status CreateVersionDirectory(const ConfigPaths& paths, IFileSystem& fs, IEventSink& events)
{
    // Find the previous version before touching the filesystem: discovering
    // it needs the version directory's siblings to be exactly what's there
    // today.
    const std::optional<std::string> previous_config_file = PreviousConfigFile(fs, paths);

    // Create the version directory and its config subdirectory *before*
    // attempting the migration copy -- mirroring the legacy
    // FileActions::check_config_dirs ordering (parent directories exist
    // before the copy is attempted). copy_file has no mkpath behavior
    // (matches QFile::copy), so doing this in the other order would
    // make the migration copy always target a directory tree that
    // doesn't exist yet, permanently defeating it.
    for (const std::string& dir : {paths.version_config_directory, paths.config_files_directory})
    {
        if (Status r = EnsureDirectory(fs, dir, events); !r.has_value())
        {
            return r;
        }
    }
    if (previous_config_file.has_value())
    {
        // A missing previous config is not an error for this step;
        // matches QFile::copy's legacy silent-failure behavior.
        std::ignore = fs.CopyFileTo(*previous_config_file, paths.config_files_directory + "fastecu.cfg", false);
    }
    return {};
}

// Keeps the 20 newest system logs.
Status PruneSyslogs(const ConfigPaths& paths, IFileSystem& fs)
{
    Result<std::vector<DirEntry>> syslogs = fs.ListDirectory(paths.syslog_files_directory);
    if (!syslogs.has_value())
    {
        return {};
    }
    const std::vector<DirEntry> files = NewestFirst(*syslogs, false);
    for (std::size_t i = 20; i < files.size(); ++i)
    {
        const std::string path = paths.syslog_files_directory + files[i].name;
        if (Status r = fs.RemoveFile(path); !r.has_value())
        {
            return AtPath(r.error(), path);
        }
    }
    return {};
}

} // namespace

Status ProvisionConfigDirectories(const ConfigPaths& paths, IFileSystem& fs, IResourceBundle& resource_bundle,
                                  IFileRepository& file_repository, IEventSink& events)
{
    if (Status r = EnsureDirectory(fs, paths.base_config_directory, events); !r.has_value())
    {
        return r;
    }
    if (paths.version_config_directory != paths.base_config_directory && !fs.Exists(paths.version_config_directory))
    {
        if (Status r = CreateVersionDirectory(paths, fs, events); !r.has_value())
        {
            return r;
        }
    }
    for (const std::string& dir :
         {paths.calibration_files_directory, paths.config_files_directory, paths.definition_files_directory,
          paths.kernel_files_directory, paths.datalog_files_directory, paths.syslog_files_directory})
    {
        if (Status r = EnsureDirectory(fs, dir, events); !r.has_value())
        {
            return r;
        }
    }
    for (const auto& [bundle_id, directory] :
         {std::pair{"config", &paths.config_files_directory}, std::pair{"kernels", &paths.kernel_files_directory}})
    {
        if (Status r = CopyBundleIfAbsent(fs, resource_bundle, file_repository, bundle_id, *directory, events);
            !r.has_value())
        {
            return r;
        }
    }
    return PruneSyslogs(paths, fs);
}

} // namespace fastecu::config
