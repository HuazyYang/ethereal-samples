#include "SQLiteFileSystem.h"

#include <donut/core/log.h>

#include <windows.h>
#include <bcrypt.h>
#include <lz4.h>
#include <sqlite3.h>

#include <cstdlib>
#include <cstring>

using namespace donut;

namespace
{
    constexpr ULONG c_Pbkdf2Iterations = 1000;
    constexpr ULONG c_AesKeySize = 16;
    constexpr ULONG c_AesBlockSize = 16;
}

SQLiteFileSystem::SQLiteFileSystem(const std::filesystem::path& databasePath, bool readOnly, const std::string& password)
{
    const std::string path = databasePath.string();
    if (sqlite3_open_v2(path.c_str(), &m_Database, readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        return;

    if (sqlite3_prepare_v2(m_Database, "SELECT data, compressed, original_size FROM files WHERE name=?1 LIMIT 1",
        -1, &m_Statement, nullptr) != SQLITE_OK)
    {
        m_Statement = nullptr;
        return;
    }

    if (password.empty())
        return;

    BCRYPT_ALG_HANDLE aes = nullptr, pbkdf2 = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&pbkdf2, BCRYPT_PBKDF2_ALGORITHM, nullptr, 0)))
    {
        BCRYPT_KEY_HANDLE key = nullptr;
        BCryptGenerateSymmetricKey(pbkdf2, &key, nullptr, 0,
            (PUCHAR)password.data(), (ULONG)password.size(), 0);
        m_PasswordKey = key;
    }
    m_AesAlgorithm = aes;
    m_Pbkdf2Algorithm = pbkdf2;
    m_Encrypted = true; // set even if a provider failed to open, as in the original
}

SQLiteFileSystem::~SQLiteFileSystem()
{
    if (m_PasswordKey)
        BCryptDestroyKey((BCRYPT_KEY_HANDLE)m_PasswordKey);
    if (m_Pbkdf2Algorithm)
        BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)m_Pbkdf2Algorithm, 0);
    if (m_AesAlgorithm)
        BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)m_AesAlgorithm, 0);
    if (m_Statement)
        sqlite3_finalize(m_Statement);
    if (m_Database)
        sqlite3_close(m_Database);
}

std::string SQLiteFileSystem::normalizeName(const std::filesystem::path& name)
{
    // The original converts the path to a wide string, replaces '\' with '/', then narrows it.
    std::wstring wide = name.wstring();
    for (auto& c : wide)
        if (c == L'\\')
            c = L'/';
    return std::filesystem::path(wide).string();
}

std::shared_ptr<vfs::IBlob> SQLiteFileSystem::readFile(const std::filesystem::path& name)
{
    const std::string key = normalizeName(name);

    uint8_t* data = nullptr;
    int dataSize = 0;
    int compressedSize = 0;
    int originalSize = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Statement || sqlite3_reset(m_Statement) != SQLITE_OK)
            return nullptr;
        if (sqlite3_bind_text(m_Statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            return nullptr;
        if (sqlite3_step(m_Statement) != SQLITE_ROW)
            return nullptr;

        dataSize = sqlite3_column_bytes(m_Statement, 0);
        const void* blob = sqlite3_column_blob(m_Statement, 0);
        if (dataSize <= 0 || !blob)
            return nullptr;

        compressedSize = sqlite3_column_int(m_Statement, 1);
        originalSize = sqlite3_column_int(m_Statement, 2);

        data = (uint8_t*)malloc(dataSize);
        memcpy(data, blob, dataSize);
    }

    if (m_Encrypted)
    {
        // Key = PBKDF2(SHA1, password, salt = file name, 1000 iterations), 16 bytes.
        ULONGLONG iterations = c_Pbkdf2Iterations;
        BCryptBuffer params[3] = {
            { (ULONG)(wcslen(BCRYPT_SHA1_ALGORITHM) + 1) * sizeof(wchar_t), KDF_HASH_ALGORITHM, (PVOID)BCRYPT_SHA1_ALGORITHM },
            { (ULONG)key.size(), KDF_SALT, (PVOID)key.data() },
            { sizeof(iterations), KDF_ITERATION_COUNT, &iterations },
        };
        BCryptBufferDesc paramList = { BCRYPTBUFFER_VERSION, 3, params };

        UCHAR derivedKey[c_AesKeySize];
        ULONG derivedSize = 0;
        BCRYPT_KEY_HANDLE aesKey = nullptr;
        ULONG plainSize = 0;

        // The first 16 bytes of the record are the CBC IV; BCryptDecrypt updates it in place.
        if (!BCRYPT_SUCCESS(BCryptKeyDerivation((BCRYPT_KEY_HANDLE)m_PasswordKey, &paramList, derivedKey, c_AesKeySize, &derivedSize, 0)) ||
            !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey((BCRYPT_ALG_HANDLE)m_AesAlgorithm, &aesKey, nullptr, 0, derivedKey, c_AesKeySize, 0)) ||
            !BCRYPT_SUCCESS(BCryptSetProperty(aesKey, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) ||
            !BCRYPT_SUCCESS(BCryptDecrypt(aesKey, data + c_AesBlockSize, dataSize - c_AesBlockSize, nullptr,
                data, c_AesBlockSize, nullptr, 0, &plainSize, 0)))
        {
            if (aesKey)
                BCryptDestroyKey(aesKey);
            free(data);
            return nullptr;
        }

        uint8_t* plain = (uint8_t*)malloc(plainSize);
        if (!BCRYPT_SUCCESS(BCryptDecrypt(aesKey, data + c_AesBlockSize, dataSize - c_AesBlockSize, nullptr,
            data, c_AesBlockSize, plain, plainSize, &plainSize, 0)))
        {
            BCryptDestroyKey(aesKey);
            free(plain);
            free(data);
            return nullptr;
        }
        BCryptDestroyKey(aesKey);
        free(data);
        data = plain;
        dataSize = (int)plainSize;
    }

    size_t resultSize = (size_t)originalSize;
    if (compressedSize > 0)
    {
        uint8_t* uncompressed = (uint8_t*)malloc(originalSize);
        const int inputSize = compressedSize < dataSize ? compressedSize : dataSize;
        const int written = LZ4_decompress_safe((const char*)data, (char*)uncompressed, inputSize, originalSize);
        free(data);
        if (written <= 0)
        {
            free(uncompressed);
            return nullptr;
        }
        data = uncompressed;
    }

    // vfs::Blob takes ownership and releases the memory with free().
    return std::make_shared<vfs::Blob>(data, resultSize);
}

// The 2018 implementation only supported readFile; the other entry points returned false.
// fileExists is answered from the database here because donut's loaders probe files first
// (deviation from the original, which always returned false).
bool SQLiteFileSystem::fileExists(const std::filesystem::path& name)
{
    const std::string key = normalizeName(name);
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Statement || sqlite3_reset(m_Statement) != SQLITE_OK)
        return false;
    if (sqlite3_bind_text(m_Statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        return false;
    return sqlite3_step(m_Statement) == SQLITE_ROW;
}

bool SQLiteFileSystem::folderExists(const std::filesystem::path&)
{
    return false;
}

bool SQLiteFileSystem::writeFile(const std::filesystem::path&, const void*, size_t)
{
    return false;
}

int SQLiteFileSystem::enumerateFiles(const std::filesystem::path&, const std::vector<std::string>&,
    vfs::enumerate_callback_t, bool)
{
    return vfs::status::NotImplemented;
}

int SQLiteFileSystem::enumerateDirectories(const std::filesystem::path&, vfs::enumerate_callback_t, bool)
{
    return vfs::status::NotImplemented;
}
