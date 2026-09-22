#include "Cryptography/Authentication/SRP6.h"
#include <array>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <openssl/rand.h>

namespace
{
uint8_t HexNibble(char value)
{
    value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    if (value >= '0' && value <= '9')
        return static_cast<uint8_t>(value - '0');
    if (value >= 'A' && value <= 'F')
        return static_cast<uint8_t>(value - 'A' + 10);
    throw std::runtime_error("invalid hex");
}

template <std::size_t Size>
std::array<uint8_t, Size> DecodeHex(std::string const& value)
{
    if (value.size() != Size * 2)
        throw std::runtime_error("invalid hex length");
    std::array<uint8_t, Size> result{};
    for (std::size_t i = 0; i < Size; ++i)
        result[i] = static_cast<uint8_t>((HexNibble(value[i * 2]) << 4) | HexNibble(value[i * 2 + 1]));
    return result;
}
}

namespace Acore::Crypto
{
void GetRandomBytes(uint8_t* buffer, std::size_t length)
{
    if (RAND_bytes(buffer, static_cast<int>(length)) != 1)
        throw std::runtime_error("RAND_bytes failed");
}
}

namespace Acore::Impl
{
void HexStrToByteArray(std::string_view value, uint8_t* output, std::size_t outputLength, bool reverse)
{
    if (value.size() != outputLength * 2)
        throw std::runtime_error("invalid constant hex length");
    for (std::size_t i = 0; i < outputLength; ++i)
    {
        std::size_t const source = reverse ? outputLength - i - 1 : i;
        output[i] = static_cast<uint8_t>((HexNibble(value[source * 2]) << 4) | HexNibble(value[source * 2 + 1]));
    }
}
}

int main()
{
    std::string username;
    std::string password;
    std::string saltHex;
    std::string verifierHex;
    if (!std::getline(std::cin, username) || !std::getline(std::cin, password) ||
        !std::getline(std::cin, saltHex) || !std::getline(std::cin, verifierHex))
        return 2;
    bool const matches = Acore::Crypto::SRP6::CheckLogin(
        username,
        password,
        DecodeHex<Acore::Crypto::SRP6::SALT_LENGTH>(saltHex),
        DecodeHex<Acore::Crypto::SRP6::VERIFIER_LENGTH>(verifierHex));
    std::fill(password.begin(), password.end(), '\0');
    std::cout << (matches ? "MATCH" : "MISMATCH") << '\n';
    return matches ? 0 : 1;
}
