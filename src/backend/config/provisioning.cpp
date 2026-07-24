#include "src/backend/config/provisioning.h"

#include <algorithm>
#include <vector>

namespace fastecu::config
{
namespace
{

Status ensure_directory(IFileSystem& fs, const std::string& path, IEventSink& events)
{
    if (fs.exists(path))
        return {};
    Status result = fs.create_directory(path);
    if (!result.has_value())
    {
        events.log(LogLevel::Error, "Unable to create directory: " + path);
        return result;
    }
    return {};
}

void copy_bundle_if_absent(IFileSystem& fs, IResourceBundle& bundle, const std::string& bundle_id,
                           const std::string& target_directory, IEventSink& events)
{
    Result<std::vector<std::string>> names = bundle.list(bundle_id);
    if (!names.has_value())
        return;
    for (const std::string& name : *names)
    {
        const std::string target = target_directory + name;
        if (fs.exists(target))
            continue;
        events.log(LogLevel::Debug, "Provisioning default file: " + target);
        // The bundle port has no direct filesystem-to-filesystem copy; write
        // through IFileSystem by reading bytes from the bundle and treating
        // the write as a same-content copy is out of this port's scope, so
        // this loop asks the filesystem port to copy from a bundle-resolved
        // source name, matching how the legacy code copied from the ":/..."
        // resource path directly with QFile::copy.
        fs.copy_file(bundle_id + "/" + name, target, false);
    }
}

} // namespace

Status provision_config_directories(const ConfigPaths& paths, IFileSystem& fs,
                                    IResourceBundle& resource_bundle, IEventSink& events)
{
    if (Status r = ensure_directory(fs, paths.base_config_directory, events); !r.has_value())
        return r;

    const bool has_version_subdirectory = paths.version_config_directory != paths.base_config_directory;
    if (has_version_subdirectory && !fs.exists(paths.version_config_directory))
    {
        Result<std::vector<DirEntry>> siblings = fs.list_directory(paths.base_config_directory);
        if (siblings.has_value() && !siblings->empty())
        {
            std::vector<DirEntry> dirs;
            std::copy_if(siblings->begin(), siblings->end(), std::back_inserter(dirs),
                         [](const DirEntry& e)
                         { return e.is_directory; });
            std::sort(dirs.begin(), dirs.end(), [](const DirEntry& a, const DirEntry& b)
                      { return a.modified_time_epoch_seconds > b.modified_time_epoch_seconds; });
            if (!dirs.empty())
            {
                const std::string previous_config_file =
                    paths.base_config_directory + "/" + dirs.front().name + "/config/fastecu.cfg";
                // A missing previous config is not an error for this step;
                // matches QFile::copy's legacy silent-failure behavior.
                fs.copy_file(previous_config_file, paths.config_files_directory + "fastecu.cfg", false);
            }
        }
        if (Status r = ensure_directory(fs, paths.version_config_directory, events); !r.has_value())
            return r;
    }

    for (const std::string& dir : {paths.calibration_files_directory, paths.config_files_directory,
                                   paths.definition_files_directory, paths.kernel_files_directory,
                                   paths.datalog_files_directory, paths.syslog_files_directory})
    {
        if (Status r = ensure_directory(fs, dir, events); !r.has_value())
            return r;
    }

    copy_bundle_if_absent(fs, resource_bundle, "config", paths.config_files_directory, events);
    copy_bundle_if_absent(fs, resource_bundle, "kernels", paths.kernel_files_directory, events);

    Result<std::vector<DirEntry>> syslogs = fs.list_directory(paths.syslog_files_directory);
    if (syslogs.has_value())
    {
        std::vector<DirEntry> files;
        std::copy_if(syslogs->begin(), syslogs->end(), std::back_inserter(files),
                     [](const DirEntry& e)
                     { return !e.is_directory; });
        std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b)
                  { return a.modified_time_epoch_seconds > b.modified_time_epoch_seconds; });
        for (std::size_t i = 20; i < files.size(); ++i)
        {
            Status r = fs.remove_file(paths.syslog_files_directory + files[i].name);
            if (!r.has_value())
                return r;
        }
    }

    return {};
}

} // namespace fastecu::config
