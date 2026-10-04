#pragma once

// Reconstructed from Asteroids.exe: SQLiteFileSystem (vtable 0x14025E9E8,
// ctor 0x14009A720, readFile 0x14009AA70). The 2018 class implemented the old
// donut IFileSystem {fileExists, readFile, writeFile, enumerate}; it is ported
// here onto donut::vfs::IFileSystem.

#include <donut/core/vfs/VFS.h>

#include <filesystem>
#include <mutex>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

class SQLiteFileSystem : public donut::vfs::IFileSystem
{
    // Adds no interface and no class ID of its own: IFileSystem's table is correct for it.
    NVRHI_INHERIT_INTERFACE_TABLE()

public:
    // Opens 'databasePath' (media.db). A non-empty 'password' enables per-file
    // AES-128-CBC decryption with PBKDF2-SHA1(password, salt = file name, 1000 iterations).
    SQLiteFileSystem(const std::filesystem::path& databasePath, bool readOnly, const std::string& password);
    ~SQLiteFileSystem() override;

    bool isOpen() const { return m_Statement != nullptr; }

    bool folderExists(const std::filesystem::path& name) override;
    bool fileExists(const std::filesystem::path& name) override;
    nvrhi::FRESULT readFile(const std::filesystem::path& name, nvrhi::IDataBlob** ppBlob) override;
    bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
    int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions,
        donut::vfs::enumerate_callback_t callback, bool allowDuplicates = false) override;
    int enumerateDirectories(const std::filesystem::path& path,
        donut::vfs::enumerate_callback_t callback, bool allowDuplicates = false) override;

private:
    static std::string normalizeName(const std::filesystem::path& name);

    sqlite3* m_Database = nullptr;
    sqlite3_stmt* m_Statement = nullptr;
    std::mutex m_Mutex;

    void* m_AesAlgorithm = nullptr;     // BCRYPT_ALG_HANDLE, "AES"
    void* m_Pbkdf2Algorithm = nullptr;  // BCRYPT_ALG_HANDLE, "PBKDF2"
    void* m_PasswordKey = nullptr;      // BCRYPT_KEY_HANDLE, PBKDF2 secret
    bool m_Encrypted = false;
};
