/*
 * Focused source-contract regression test for the command server's bounded read buffer.
 */

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
std::filesystem::path ModuleRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string ReadSource(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open())
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string_view FunctionBody(std::string const& source)
{
    std::size_t const begin = source.find("bool ReadLine");
    if (begin == std::string::npos)
        return {};

    std::size_t const end = source.find("void session", begin);
    if (end == std::string::npos)
        return std::string_view(source).substr(begin);

    return std::string_view(source).substr(begin, end - begin);
}
}  // namespace

TEST(PlayerbotCommandServerContract, ReadLineUsesExactReadLengthWithoutFullBufferTerminatorWrite)
{
    std::string const source = ReadSource(ModuleRoot() / "src/Bot/Cmd/PlayerbotCommandServer.cpp");
    ASSERT_FALSE(source.empty());

    std::string_view const readLine = FunctionBody(source);
    ASSERT_FALSE(readLine.empty());

    EXPECT_NE(readLine.find("char buf[1025]"), std::string_view::npos);
    EXPECT_NE(readLine.find("read_some(boost::asio::buffer(buf), error)"), std::string_view::npos);
    EXPECT_EQ(readLine.find("buf[n]"), std::string_view::npos)
        << "ReadLine must not write a terminator at n when the full 1025-byte buffer is read";
    EXPECT_EQ(readLine.find("*buffer += buf"), std::string_view::npos);

    std::size_t const append = readLine.find("buffer->append(buf, n)");
    ASSERT_NE(append, std::string_view::npos);
    EXPECT_LT(readLine.find("read_some(boost::asio::buffer(buf), error)"), append);
}

TEST(PlayerbotCommandServerContract, LengthAwareAppendRetainsExactlyAll1025ReadBytes)
{
    constexpr std::size_t bufferSize = 1025;
    std::string chunk(bufferSize, 'x');
    chunk.back() = '\0';

    std::string buffer;
    buffer.append(chunk.data(), bufferSize);

    EXPECT_EQ(buffer.size(), bufferSize);
    EXPECT_EQ(buffer, chunk);
    EXPECT_EQ(buffer.back(), '\0');
}
