#include "src/backend/config/provisioning.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
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
    if (Status result = fs.CreateDirectory(path); !result.has_value())
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

} // namespace

Status ProvisionConfigDirectories(const ConfigPaths& paths, IFileSystem& fs, IResourceBundle& resource_bundle,
                                  IFileRepository& file_repository, IEventSink& events)
{
    if (Status r = EnsureDirectory(fs, paths.base_config_directory, events); !r.has_value())
    {
        return r;
    }

    if (const bool has_version_subdirectory = paths.version_config_directory != paths.base_config_directory;
        has_version_subdirectory && !fs.Exists(paths.version_config_directory))
    {
        // Find the newest previous-version sibling directory (if any) before
        // touching the filesystem, since discovering it needs the
        // not-yet-created version directory's siblings to still be exactly
        // what's there today.
        std::string previous_config_file;
        bool has_previous_config_file = false;
        if (Result<std::vector<DirEntry>> siblings = fs.ListDirectory(paths.base_config_directory);
            siblings.has_value() && !siblings->empty())
        {
            std::vector<DirEntry> dirs;
            std::copy_if(siblings->begin(), siblings->end(), std::back_inserter(dirs),
                         [](const DirEntry& e) { return e.is_directory; });
            std::sort(dirs.begin(), dirs.end(), [](const DirEntry& a, const DirEntry& b)
                      { return a.modified_time_epoch_seconds > b.modified_time_epoch_seconds; });
            if (!dirs.empty())
            {
                previous_config_file = paths.base_config_directory + "/" + dirs.front().name + "/config/fastecu.cfg";
                has_previous_config_file = true;
            }
        }

        // Create the version directory and its config subdirectory *before*
        // attempting the migration copy -- mirroring the legacy
        // FileActions::check_config_dirs ordering (parent directories exist
        // before the copy is attempted). copy_file has no mkpath behavior
        // (matches QFile::copy), so doing this in the other order would
        // make the migration copy always target a directory tree that
        // doesn't exist yet, permanently defeating it.
        if (Status r = EnsureDirectory(fs, paths.version_config_directory, events); !r.has_value())
        {
            return r;
        }
        if (Status r = EnsureDirectory(fs, paths.config_files_directory, events); !r.has_value())
        {
            return r;
        }

        if (has_previous_config_file)
        {
            // A missing previous config is not an error for this step;
            // matches QFile::copy's legacy silent-failure behavior.
            std::ignore = fs.CopyFile(previous_config_file, paths.config_files_directory + "fastecu.cfg", false);
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

    if (Status r =
            CopyBundleIfAbsent(fs, resource_bundle, file_repository, "config", paths.config_files_directory, events);
        !r.has_value())
    {
        return r;
    }
    if (Status r =
            CopyBundleIfAbsent(fs, resource_bundle, file_repository, "kernels", paths.kernel_files_directory, events);
        !r.has_value())
    {
        return r;
    }

    if (Result<std::vector<DirEntry>> syslogs = fs.ListDirectory(paths.syslog_files_directory); syslogs.has_value())
    {
        std::vector<DirEntry> files;
        std::copy_if(syslogs->begin(), syslogs->end(), std::back_inserter(files),
                     [](const DirEntry& e) { return !e.is_directory; });
        std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b)
                  { return a.modified_time_epoch_seconds > b.modified_time_epoch_seconds; });
        for (std::size_t i = 20; i < files.size(); ++i)
        {
            Status r = fs.RemoveFile(paths.syslog_files_directory + files[i].name);
            if (!r.has_value())
            {
                return AtPath(r.error(), paths.syslog_files_directory + files[i].name);
            }
        }
    }

    return {};
}

} // namespace fastecu::config
