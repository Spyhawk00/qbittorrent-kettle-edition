/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Spyhawk
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

// Installs a downloaded Kettle Edition update. qBittorrent starts this program, writes a state file first, and exits.
//
//   kettle-update-helper.exe <state file>
//
// Steps: wait for qBittorrent to exit, check the downloaded files again, back up the profile, replace the program files,
// start the new version and wait for it to report "healthy". If it stops, hangs or never reports, everything is put back:
// the old files, the profile, and the old version is started again. The state file is plain "key=value" lines (UTF-8):
//
//   pid, old, new, level, notes     process id of the exiting qBittorrent, editions, release level and notes
//   program, exe, update            program folder, exe name, folder with the downloaded files
//   root                            folder for healthy, clean-exit, result.txt, failures.txt and skipped.txt
//   window, healthy_timeout         how the old window looked, seconds to wait for "healthy"
//   previous, backup, log           kettle-previous folder, profile backup folder, update.log
//   profile, exclude                profile folders to back up; top-level names inside them that are not backed up or restored
//   arg                             command line argument for the program (repeated)
//   file                            path|size|sha256 of a downloaded file (repeated)

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace
{
    using Text = std::wstring;

    struct FileEntry
    {
        Text path;
        unsigned long long size = 0;
        std::string sha256;
    };

    struct State
    {
        Text statePath;
        DWORD pid = 0;
        Text oldEdition, newEdition, level, notes;
        Text program, exe, update, root;
        Text window;
        DWORD healthyTimeout = 300;
        Text previous, backup, log;
        std::vector<Text> profiles, excludes, args;
        std::vector<FileEntry> files;
    };

    // one entry of the swap, kept so that it can be undone
    struct Swapped
    {
        Text target;
        Text backup;  // target + ".old"
        bool hadOld = false;
    };

    State state;
    std::vector<Swapped> swapped;

    // ---- text ----

    Text widen(const std::string &utf8)
    {
        if (utf8.empty())
            return {};
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        Text result(size, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
        return result;
    }

    std::string narrow(const Text &text)
    {
        if (text.empty())
            return {};
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    Text join(const Text &folder, const Text &name)
    {
        Text result = folder;
        if (!result.empty() && (result.back() != L'\\') && (result.back() != L'/'))
            result += L'\\';
        result += name;
        std::replace(result.begin(), result.end(), L'/', L'\\');
        return result;
    }

    Text lower(Text text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return text;
    }

    // ---- files ----

    bool readFile(const Text &path, std::string &content)
    {
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        content.clear();
        char buffer[16384];
        DWORD read = 0;
        while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && (read > 0))
            content.append(buffer, read);
        ::CloseHandle(file);
        return true;
    }

    bool writeFile(const Text &path, const std::string &content, const bool append = false)
    {
        const HANDLE file = ::CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr
                , append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const bool ok = ::WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) && (written == content.size());
        ::CloseHandle(file);
        return ok;
    }

    bool exists(const Text &path)
    {
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool isDirectory(const Text &path)
    {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return (attributes != INVALID_FILE_ATTRIBUTES) && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    void makeFolder(const Text &path)
    {
        if (path.empty() || isDirectory(path))
            return;
        const size_t cut = path.find_last_of(L"\\/");
        if ((cut != Text::npos) && (cut > 2))
            makeFolder(path.substr(0, cut));
        ::CreateDirectoryW(path.c_str(), nullptr);
    }

    void makeParentFolder(const Text &path)
    {
        const size_t cut = path.find_last_of(L"\\/");
        if (cut != Text::npos)
            makeFolder(path.substr(0, cut));
    }

    // Defender and the search indexer often lock a new file briefly, so most file operations are retried for a while
    bool retry(const unsigned int seconds, const std::function<bool()> &action)
    {
        const ULONGLONG deadline = ::GetTickCount64() + (seconds * 1000ULL);
        while (true)
        {
            if (action())
                return true;
            if (::GetTickCount64() >= deadline)
                return false;
            ::Sleep(500);
        }
    }

    bool fileSize(const Text &path, unsigned long long &size)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return false;
        size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        return true;
    }

    std::string sha256(const Text &path)
    {
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};

        std::string result;
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectSize = 0, got = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
        {
            std::vector<UCHAR> object;
            if ((BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &got, 0) >= 0)
                    && (object.resize(objectSize), BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0))
            {
                std::vector<UCHAR> buffer(1 << 20);
                DWORD read = 0;
                bool ok = true;
                while (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && (read > 0))
                    ok = ok && (BCryptHashData(hash, buffer.data(), read, 0) >= 0);

                UCHAR digest[32];
                if (ok && (BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0))
                {
                    const char *digits = "0123456789abcdef";
                    for (const UCHAR byte : digest)
                    {
                        result += digits[byte >> 4];
                        result += digits[byte & 15];
                    }
                }
                BCryptDestroyHash(hash);
            }
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        ::CloseHandle(file);
        return result;
    }

    // checks a file against the size and SHA-256 of the state file
    bool matches(const Text &path, const FileEntry &entry)
    {
        unsigned long long size = 0;
        return fileSize(path, size) && (size == entry.size) && (sha256(path) == entry.sha256);
    }

    bool removeFile(const Text &path)
    {
        ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return ::DeleteFileW(path.c_str()) || (::GetLastError() == ERROR_FILE_NOT_FOUND);
    }

    bool isExcluded(const Text &name, const std::vector<Text> &excludes)
    {
        return std::any_of(excludes.begin(), excludes.end(), [&name](const Text &item) { return lower(item) == lower(name); });
    }

    // Deletes the content of a folder; the names in `keep` are left alone at the top level. Returns false if something stayed.
    bool clearFolder(const Text &folder, const std::vector<Text> &keep, const bool removeSelf)
    {
        bool ok = true;
        WIN32_FIND_DATAW found;
        const HANDLE search = ::FindFirstFileW(join(folder, L"*").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE)
        {
            do
            {
                const Text name = found.cFileName;
                if ((name == L".") || (name == L"..") || isExcluded(name, keep))
                    continue;
                const Text path = join(folder, name);
                if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    ok = ok && (::RemoveDirectoryW(path.c_str()) || removeFile(path));  // a link: the link, never what it points to
                else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ok = clearFolder(path, {}, true) && ok;
                else
                    ok = retry(5, [&path]() { return removeFile(path); }) && ok;
            }
            while (::FindNextFileW(search, &found));
            ::FindClose(search);
        }
        if (removeSelf && ok)
            ok = retry(5, [&folder]() { return ::RemoveDirectoryW(folder.c_str()) || !exists(folder); });
        return ok;
    }

    // Copies a folder with everything in it; the names in `skip` are not copied at the top level
    bool copyFolder(const Text &from, const Text &to, const std::vector<Text> &skip)
    {
        makeFolder(to);
        bool ok = true;
        WIN32_FIND_DATAW found;
        const HANDLE search = ::FindFirstFileW(join(from, L"*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
            return isDirectory(from);
        do
        {
            const Text name = found.cFileName;
            if ((name == L".") || (name == L"..") || isExcluded(name, skip) || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            const Text source = join(from, name);
            const Text target = join(to, name);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ok = copyFolder(source, target, {}) && ok;
            else
                ok = retry(10, [&]() { return ::CopyFileW(source.c_str(), target.c_str(), FALSE) != 0; }) && ok;
        }
        while (::FindNextFileW(search, &found));
        ::FindClose(search);
        return ok;
    }

    // ---- log and result ----

    void log(const Text &message)
    {
        if (state.log.empty())
            return;
        SYSTEMTIME now;
        ::GetLocalTime(&now);
        wchar_t stamp[40];
        ::swprintf_s(stamp, L"%04d-%02d-%02d %02d:%02d:%02d  ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        makeParentFolder(state.log);
        writeFile(state.log, narrow(stamp + message + L"\r\n"), true);
    }

    // result.txt is read by qBittorrent when it starts again; skipped.txt keeps a failed version from being tried every night
    void writeResult(const Text &status, const Text &reason, const bool skip)
    {
        const std::string content = narrow(L"status=" + status + L"\r\nedition=" + state.newEdition + L"\r\nold=" + state.oldEdition
                + L"\r\nreason=" + reason + L"\r\n");
        writeFile(join(state.root, L"result.txt"), content);
        if (skip)
            writeFile(join(state.root, L"skipped.txt"), narrow(state.newEdition + L"\r\n"), true);
    }

    // A failure that may pass (locked file, busy disk) skips the version only after this many failures in a row;
    // failures.txt keeps "<edition> <count>"
    const int TEMPORARY_FAILURES_BEFORE_SKIP = 3;

    // Counts one more failure of the new version; true when it has failed often enough to be skipped
    bool countTemporaryFailure()
    {
        const Text path = join(state.root, L"failures.txt");
        long count = 0;
        std::string content;
        if (readFile(path, content))
        {
            const Text text = widen(content);
            const size_t space = text.find(L' ');
            if ((space != Text::npos) && (text.substr(0, space) == state.newEdition))
                count = std::wcstol(text.c_str() + space + 1, nullptr, 10);
        }
        ++count;
        writeFile(path, narrow(state.newEdition + L" " + std::to_wstring(count) + L"\r\n"));
        return count >= TEMPORARY_FAILURES_BEFORE_SKIP;
    }

    // ---- state file checks ----

    // The full path in lower case without a closing backslash, to compare folders; empty if it can't be made
    Text fullPath(const Text &path)
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if ((length == 0) || (length >= buffer.size()))
            return {};
        Text result = lower(Text(buffer.data(), length));
        while ((result.size() > 3) && (result.back() == L'\\'))
            result.pop_back();
        return result;
    }

    // true if `path` is `folder` itself or something inside it
    bool isSameOrInside(const Text &folder, const Text &path)
    {
        Text parent = fullPath(folder);
        const Text child = fullPath(path);
        if (parent.empty() || child.empty())
            return false;
        if (child == parent)
            return true;
        if (parent.back() != L'\\')
            parent += L'\\';
        return child.compare(0, parent.size(), parent) == 0;
    }

    Text lastName(const Text &path)
    {
        const Text full = fullPath(path);
        const size_t cut = full.find_last_of(L'\\');
        return (cut == Text::npos) ? full : full.substr(cut + 1);
    }

    // a path below the program folder: no drive, no leading backslash, no ".."
    bool isSafeRelativePath(const Text &path)
    {
        if (path.empty() || (path.find(L':') != Text::npos) || (path.front() == L'\\') || (path.front() == L'/'))
            return false;
        size_t start = 0;
        while (start <= path.size())
        {
            size_t end = path.find_first_of(L"\\/", start);
            if (end == Text::npos)
                end = path.size();
            const Text part = path.substr(start, end - start);
            if ((part == L"..") || (part == L"."))
                return false;
            start = end + 1;
        }
        return true;
    }

    // qBittorrent writes the state file, but the helper deletes whole folders it names, so it accepts only expected
    // folders: none may be, or contain, the program or a profile.
    bool isStateSafe()
    {
        if (!isDirectory(state.program) || (state.exe.find_first_of(L"\\/:") != Text::npos)
                || !exists(join(state.program, state.exe)))
            return false;
        for (const FileEntry &entry : state.files)
        {
            if (!isSafeRelativePath(entry.path))
                return false;
        }

        // the downloaded files are in a folder of their own below the folder of the update checks
        if (!isSameOrInside(state.root, state.update) || (fullPath(state.update) == fullPath(state.root)))
            return false;
        const Text backupSuffix = L"-update-backup";
        const Text backupName = lastName(state.backup);
        if ((lastName(state.previous) != L"kettle-previous")
                || (backupName.size() <= backupSuffix.size()) || (backupName.compare(backupName.size() - backupSuffix.size(), backupSuffix.size(), backupSuffix) != 0)
                || (!state.log.empty() && (lastName(state.log) != L"update.log")))
            return false;

        // these three are emptied and removed
        for (const Text *folder : {&state.update, &state.previous, &state.backup})
        {
            if (isSameOrInside(*folder, state.program))
                return false;
            for (const Text &profile : state.profiles)
            {
                if (isSameOrInside(*folder, profile))
                    return false;
            }
        }
        // a profile is emptied when the update is undone
        for (const Text &profile : state.profiles)
        {
            if ((fullPath(profile).size() <= 3) || isSameOrInside(profile, state.program))
                return false;
        }
        return true;
    }

    // ---- state file ----

    bool readState(const Text &path)
    {
        std::string content;
        if (!readFile(path, content))
            return false;

        state.statePath = path;
        size_t start = 0;
        while (start < content.size())
        {
            size_t end = content.find('\n', start);
            if (end == std::string::npos)
                end = content.size();
            std::string line = content.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && (line.back() == '\r'))
                line.pop_back();
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;

            const std::string key = line.substr(0, equals);
            const Text value = widen(line.substr(equals + 1));
            if (key == "pid") state.pid = static_cast<DWORD>(std::stoul(narrow(value)));
            else if (key == "old") state.oldEdition = value;
            else if (key == "new") state.newEdition = value;
            else if (key == "level") state.level = value;
            else if (key == "notes") state.notes = value;
            else if (key == "program") state.program = value;
            else if (key == "exe") state.exe = value;
            else if (key == "update") state.update = value;
            else if (key == "root") state.root = value;
            else if (key == "window") state.window = value;
            else if (key == "healthy_timeout") state.healthyTimeout = static_cast<DWORD>(std::stoul(narrow(value)));
            else if (key == "previous") state.previous = value;
            else if (key == "backup") state.backup = value;
            else if (key == "log") state.log = value;
            else if (key == "profile") state.profiles.push_back(value);
            else if (key == "exclude") state.excludes.push_back(value);
            else if (key == "arg") state.args.push_back(value);
            else if (key == "file")
            {
                const size_t first = value.find(L'|');
                const size_t second = value.find(L'|', first + 1);
                if ((first == Text::npos) || (second == Text::npos))
                    return false;
                FileEntry entry;
                entry.path = value.substr(0, first);
                entry.size = std::stoull(narrow(value.substr(first + 1, second - first - 1)));
                entry.sha256 = narrow(value.substr(second + 1));
                state.files.push_back(entry);
            }
        }
        while (!state.program.empty() && ((state.program.back() == L'\\') || (state.program.back() == L'/')))
            state.program.pop_back();
        return !state.program.empty() && !state.exe.empty() && !state.update.empty() && !state.root.empty() && !state.files.empty()
            && isStateSafe();
    }

    // ---- program ----

    Text quote(const Text &argument)
    {
        return L"\"" + argument + L"\"";
    }

    // Starts qBittorrent as the same user that runs this helper. `marked` tells the new version that this helper started it.
    HANDLE startProgram(const bool marked)
    {
        Text command = quote(join(state.program, state.exe));
        for (const Text &argument : state.args)
            command += L' ' + quote(argument);

        if (marked)
            ::SetEnvironmentVariableW(L"KETTLE_UPDATE_STATE", state.statePath.c_str());
        else
            ::SetEnvironmentVariableW(L"KETTLE_UPDATE_STATE", nullptr);

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info = {};
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');
        HANDLE process = nullptr;
        if (::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, state.program.c_str(), &startup, &info))
        {
            ::CloseHandle(info.hThread);
            process = info.hProcess;
        }
        ::SetEnvironmentVariableW(L"KETTLE_UPDATE_STATE", nullptr);
        return process;
    }

    // true if the process with this id is gone (or never existed)
    bool waitForExit(const DWORD pid, const DWORD milliseconds)
    {
        const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process)
            return ::GetLastError() == ERROR_INVALID_PARAMETER;  // no such process
        const bool exited = (::WaitForSingleObject(process, milliseconds) == WAIT_OBJECT_0);
        ::CloseHandle(process);
        return exited;
    }

    // ---- the steps ----

    bool filesAreIntact()
    {
        for (const FileEntry &entry : state.files)
        {
            if (!retry(10, [&]() { return matches(join(state.update, entry.path), entry); }))
            {
                log(L"The downloaded file " + entry.path + L" does not match its checksum.");
                return false;
            }
        }
        return true;
    }

    bool backupProfile()
    {
        clearFolder(state.backup, {}, true);
        for (size_t i = 0; i < state.profiles.size(); ++i)
        {
            if (!copyFolder(state.profiles[i], join(state.backup, std::to_wstring(i)), state.excludes))
                return false;
        }
        return true;
    }

    bool restoreProfile()
    {
        bool ok = true;
        for (size_t i = 0; i < state.profiles.size(); ++i)
        {
            const Text source = join(state.backup, std::to_wstring(i));
            if (!isDirectory(source))
                continue;
            ok = clearFolder(state.profiles[i], state.excludes, false) && ok;
            ok = copyFolder(source, state.profiles[i], {}) && ok;
        }
        return ok;
    }

    // moves each installed file aside (to "<name>.old") and puts the downloaded one in its place
    bool swapFiles()
    {
        for (const FileEntry &entry : state.files)
        {
            Swapped item;
            item.target = join(state.program, entry.path);
            item.backup = item.target + L".old";
            makeParentFolder(item.target);

            if (exists(item.target))
            {
                if (!retry(30, [&]() { return ::MoveFileExW(item.target.c_str(), item.backup.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }))
                {
                    log(L"Could not move aside " + entry.path + L" (error " + std::to_wstring(::GetLastError()) + L").");
                    return false;
                }
                item.hadOld = true;
            }
            swapped.push_back(item);

            const Text source = join(state.update, entry.path);
            if (!retry(30, [&]() { return ::CopyFileW(source.c_str(), item.target.c_str(), FALSE) != 0; }))
            {
                log(L"Could not copy in " + entry.path + L" (error " + std::to_wstring(::GetLastError()) + L").");
                return false;
            }
        }
        return true;
    }

    bool undoSwap()
    {
        bool ok = true;
        for (auto it = swapped.rbegin(); it != swapped.rend(); ++it)
        {
            ok = retry(30, [&]() { return removeFile(it->target); }) && ok;
            if (it->hadOld)
                ok = retry(30, [&]() { return ::MoveFileExW(it->backup.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }) && ok;
        }
        swapped.clear();
        return ok;
    }

    // keeps one generation of the replaced files and deletes the rest of the leftovers
    void finishInstall()
    {
        clearFolder(state.previous, {}, true);
        for (const Swapped &item : swapped)
        {
            if (!item.hadOld)
                continue;
            const Text relative = item.target.substr(state.program.size() + 1);
            const Text kept = join(state.previous, relative);
            makeParentFolder(kept);
            if (!::MoveFileExW(item.backup.c_str(), kept.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
                removeFile(item.backup);
        }
        swapped.clear();
        clearFolder(state.backup, {}, true);
        clearFolder(state.update, {}, true);
    }

    enum class Health { Healthy, Stopped, Silent };

    // Waits until the new version writes "healthy", stops, or the time is up.
    // A normal quit by the user before that (it writes "clean-exit") is no failure.
    Health waitForHealthy(const HANDLE process, DWORD &exitCode)
    {
        const Text healthy = join(state.root, L"healthy");
        const Text cleanExit = join(state.root, L"clean-exit");
        const ULONGLONG deadline = ::GetTickCount64() + (state.healthyTimeout * 1000ULL);
        while (::GetTickCount64() < deadline)
        {
            if (exists(healthy))
                return Health::Healthy;
            if (::WaitForSingleObject(process, 500) == WAIT_OBJECT_0)
            {
                ::GetExitCodeProcess(process, &exitCode);
                return exists(healthy) || exists(cleanExit) ? Health::Healthy : Health::Stopped;
            }
        }
        return Health::Silent;
    }

    // Ends the run after something went wrong: nothing is left half-installed, and the old version runs again
    int abandon(const Text &status, const Text &reason, const bool skip)
    {
        log(L"Update " + state.newEdition + L" was not installed: " + reason);
        writeResult(status, reason, skip);
        clearFolder(state.backup, {}, true);
        clearFolder(state.update, {}, true);
        removeFile(state.statePath);
        if (const HANDLE old = startProgram(false))
            ::CloseHandle(old);
        else
            log(L"The old version could not be started again (error " + std::to_wstring(::GetLastError()) + L").");
        return 1;
    }

    int run()
    {
        log(L"Update " + state.oldEdition + L" -> " + state.newEdition + L" (" + state.level + L") started.");

        if (!waitForExit(state.pid, 15 * 60 * 1000))
        {
            log(L"qBittorrent did not exit within 15 minutes. Nothing was changed.");
            removeFile(state.statePath);
            return 1;
        }
        ::Sleep(1000);  // the exe is released a moment after the process ends

        if (!filesAreIntact())
            return abandon(L"discarded", L"a downloaded file was damaged", false);

        if (!backupProfile())
            return abandon(L"failed", L"your settings could not be backed up first", countTemporaryFailure());
        log(L"Profile backed up.");

        removeFile(join(state.root, L"healthy"));
        removeFile(join(state.root, L"clean-exit"));
        if (!swapFiles())
        {
            const bool undone = undoSwap();
            return abandon(L"failed", L"program files could not be replaced" + Text(undone ? L"" : L" and not all of them could be put back"), countTemporaryFailure());
        }
        log(L"Program files replaced. Starting the new version.");

        const HANDLE process = startProgram(true);
        if (!process)
        {
            const bool undone = undoSwap();
            return abandon(L"failed", L"the new version could not be started" + Text(undone ? L"" : L" and not all files could be put back"), countTemporaryFailure());
        }

        DWORD exitCode = 0;
        const Health health = waitForHealthy(process, exitCode);
        if (health == Health::Healthy)
        {
            ::CloseHandle(process);
            finishInstall();
            removeFile(join(state.root, L"failures.txt"));
            removeFile(state.statePath);
            log(L"The new version is healthy. Update installed.");
            return 0;
        }

        Text reason = (health == Health::Stopped)
                ? (L"the new version stopped (exit code " + std::to_wstring(exitCode) + L")")
                : L"the new version did not start properly within " + std::to_wstring(state.healthyTimeout) + L" seconds";
        if (health == Health::Silent)
        {
            ::TerminateProcess(process, 1);
            ::WaitForSingleObject(process, 15000);
        }
        ::CloseHandle(process);

        const bool filesBack = undoSwap();
        const bool profileBack = restoreProfile();
        if (!filesBack || !profileBack)
            reason += L"; some files could not be put back, the backup is kept in " + state.backup;
        else
            clearFolder(state.backup, {}, true);
        log(filesBack && profileBack ? L"The old version and the profile were restored." : L"Restoring the old version was not complete.");

        writeResult(L"rolled-back", reason, true);
        log(L"Update " + state.newEdition + L" was undone: " + reason);
        clearFolder(state.update, {}, true);
        removeFile(state.statePath);
        if (const HANDLE old = startProgram(false))
            ::CloseHandle(old);
        else
            log(L"The old version could not be started again (error " + std::to_wstring(::GetLastError()) + L").");
        return 1;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t **arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    bool ok = false;
    try
    {
        ok = arguments && (count >= 2) && readState(arguments[1]);
    }
    catch (...)  // a number in the state file that is not a number
    {
    }
    if (arguments)
        ::LocalFree(arguments);
    return ok ? run() : 2;
}
