#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"

#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace
{

void WriteTestFile(const QString& path, const char *contents)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(contents), static_cast<qint64>(std::char_traits<char>::length(contents)));
}

std::string ReadTestFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll().toStdString();
}

} // namespace

TEST(QtAtomicFileWriterTest, ReplacesExistingFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("definition.xml");
    WriteTestFile(path, "old");
    QtAtomicFileWriter writer;
    const std::array<std::uint8_t, 3> bytes{'n', 'e', 'w'};

    ASSERT_THAT(writer.Replace(path.toStdString(), bytes), fastecu::testing::IsOk());
    EXPECT_EQ(ReadTestFile(path), "new");
}

TEST(QtAtomicFileWriterTest, InvalidDestinationDoesNotCreateFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("missing/definition.xml");
    QtAtomicFileWriter writer;
    const std::array<std::uint8_t, 3> bytes{'n', 'e', 'w'};

    ASSERT_THAT(writer.Replace(path.toStdString(), bytes), fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_FALSE(QFile::exists(path));
}

#if !defined(Q_OS_WIN)
TEST(QtAtomicFileWriterTest, FailedReplacementPreservesExistingFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("definition.xml");
    WriteTestFile(path, "old");
    ASSERT_TRUE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QtAtomicFileWriter writer;
    const std::array<std::uint8_t, 3> bytes{'n', 'e', 'w'};

    const fastecu::Status status = writer.Replace(path.toStdString(), bytes);

    EXPECT_TRUE(
        QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    ASSERT_THAT(status, fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    EXPECT_EQ(ReadTestFile(path), "old");
}
#endif
