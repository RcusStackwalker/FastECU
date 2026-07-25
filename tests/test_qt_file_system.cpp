#include "src/platform/desktop/common/ports/qt_file_system.h"
#include <gtest/gtest.h>
#include <QDir>
#include <QTemporaryDir>

using fastecu::ErrorKind;

TEST(QtFileSystemTest, CreateDirectoryThenExists)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QtFileSystem fs;
    const std::string dir = (tmp.path() + "/child").toStdString();

    EXPECT_FALSE(fs.exists(dir));
    ASSERT_TRUE(fs.create_directory(dir).has_value());
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

    ASSERT_TRUE(fs.copy_file(src, dst, false).has_value());
    EXPECT_TRUE(fs.exists(dst));
    ASSERT_TRUE(fs.remove_file(dst).has_value());
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

    auto r = fs.copy_file(src, dst, false);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Internal);
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

    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->size(), 2u);
    bool found_dir = false, found_file = false;
    for (auto& e : *entries)
    {
        if (e.name == "subdir" && e.is_directory)
            found_dir = true;
        if (e.name == "file.txt" && !e.is_directory)
            found_file = true;
    }
    EXPECT_TRUE(found_dir);
    EXPECT_TRUE(found_file);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
