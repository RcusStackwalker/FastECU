#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"

#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace
{

void write_test_file(const QString& path, const char *contents)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(contents), static_cast<qint64>(std::char_traits<char>::length(contents)));
}

std::string read_test_file(const QString& path)
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
    write_test_file(path, "old");
    QtAtomicFileWriter writer;
    const std::array<std::uint8_t, 3> bytes{'n', 'e', 'w'};

    ASSERT_TRUE(writer.replace(path.toStdString(), bytes));
    EXPECT_EQ(read_test_file(path), "new");
}

TEST(QtAtomicFileWriterTest, InvalidDestinationDoesNotCreateFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("missing/definition.xml");
    QtAtomicFileWriter writer;
    const std::array<std::uint8_t, 3> bytes{'n', 'e', 'w'};

    const fastecu::Status status = writer.replace(path.toStdString(), bytes);

    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_FALSE(QFile::exists(path));
}
