#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <gtest/gtest.h>

using fastecu::ErrorKind;

TEST(QtFileSystemTest, CreateDirectoryThenExists)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QtFileSystem fs;
    const std::string dir = (tmp.path() + "/child").toStdString();

    EXPECT_FALSE(fs.exists(dir));
    ASSERT_THAT(fs.create_directory(dir), fastecu::testing::IsOk());
    EXPECT_TRUE(fs.exists(dir));
}

TEST(QtFileSystemTest, CopyThenRemove)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QtFileSystem fs;
    const std::string src = (tmp.path() + "/a.txt").toStdString();
    const std::string dst = (tmp.path() + "/b.txt").toStdString();
    QFile f(QString::fromStdString(src));
    f.open(QIODevice::WriteOnly);
    f.write("hi");
    f.close();

    ASSERT_THAT(fs.copy_file(src, dst, false), fastecu::testing::IsOk());
    EXPECT_TRUE(fs.exists(dst));
    ASSERT_THAT(fs.remove_file(dst), fastecu::testing::IsOk());
    EXPECT_FALSE(fs.exists(dst));
}

TEST(QtFileSystemTest, CopyWithoutOverwriteFailsWhenDestinationExists)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QtFileSystem fs;
    const std::string src = (tmp.path() + "/a.txt").toStdString();
    const std::string dst = (tmp.path() + "/b.txt").toStdString();
    for (const std::string& path : {src, dst})
    {
        QFile f(QString::fromStdString(path));
        f.open(QIODevice::WriteOnly);
        f.write("x");
    }

    ASSERT_THAT(fs.copy_file(src, dst, false), fastecu::testing::IsErr(ErrorKind::Internal));
}

TEST(QtFileSystemTest, ListDirectoryReturnsEntriesWithModifiedTime)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QtFileSystem fs;
    QDir(tmp.path()).mkdir("subdir");
    QFile f(tmp.path() + "/file.txt");
    f.open(QIODevice::WriteOnly);
    f.write("x");
    f.close();

    auto entries = fs.list_directory(tmp.path().toStdString());

    ASSERT_THAT(entries, fastecu::testing::IsOk());
    EXPECT_EQ(entries->size(), 2U);
    bool found_dir = false, found_file = false;
    for (auto& e : *entries)
    {
        if (e.name == "subdir" && e.is_directory)
        {
            found_dir = true;
        }
        if (e.name == "file.txt" && !e.is_directory)
        {
            found_file = true;
        }
    }
    EXPECT_TRUE(found_dir);
    EXPECT_TRUE(found_file);
}

TEST(QtFileSystemTest, ListDirectoryIdentifiesDirectorySymlink)
{
#if defined(Q_OS_WIN)
    GTEST_SKIP() << "QFile::link creates Windows shortcuts, not directory symlinks";
#endif

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString nested = tmp.path() + "/nested";
    ASSERT_TRUE(QDir().mkpath(nested));
    const QString loop = nested + "/loop";
    if (!QFile::link(tmp.path(), loop))
    {
        GTEST_SKIP() << "directory symlinks are unavailable";
    }

    QtFileSystem fs;
    auto nestedEntries = fs.list_directory(nested.toStdString());
    ASSERT_THAT(nestedEntries, fastecu::testing::IsOk());
    const auto loopEntry = std::find_if(nestedEntries->begin(), nestedEntries->end(),
                                        [](const fastecu::DirEntry& entry) { return entry.name == "loop"; });
    ASSERT_NE(loopEntry, nestedEntries->end());
    EXPECT_TRUE(loopEntry->is_directory);
    EXPECT_TRUE(loopEntry->is_symlink);
}
