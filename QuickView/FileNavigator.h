#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <functional>  // for std::hash
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <Windows.h>
#include <Shlwapi.h>   // for StrCmpLogicalW
#include "EditState.h" // for g_runtime
#include "exif.h"      // for easyexif
#include "SupportedExtensions.h" // Unified supported extensions

#pragma comment(lib, "Shlwapi.lib")

// [ImageID Architecture] Stable content-based unique identifier
using ImageID = size_t;  // 64-bit path hash

// Helper: Compute normalized path hash (case-insensitive for Windows)
inline ImageID ComputePathHash(const std::wstring& path) {
    std::wstring normalized = path;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
    return std::hash<std::wstring>{}(normalized);
}

class FileNavigator {
public:
    void Initialize(const std::wstring& currentPath, bool allowAsync = true) {
        namespace fs = std::filesystem;
        fs::path p(currentPath);
        if (!fs::exists(p)) return;

        CancelPendingUpgrade();
        m_files.clear();
        m_sizes.clear();
        m_ids.clear();
        m_currentIndex = -1;
        m_currentDir.clear();

        const bool isDirectory = fs::is_directory(p);

        // If a directory is passed in, scan it directly. Otherwise scan the parent directory.
        fs::path dir = isDirectory ? p : p.parent_path();
        if (dir.empty()) return;
        m_currentDir = dir.wstring();

        try {
            if (g_runtime.SortOrder == 3) {
                DirectorySnapshot fullSnapshot;
                if (TryGetCachedSnapshot(dir, g_runtime.SortOrder, g_runtime.SortDescending, fullSnapshot)) {
                    LoadSnapshot(fullSnapshot);
                } else if (allowAsync) {
                    LoadSnapshot(GetOrBuildSnapshot(dir, 0, g_runtime.SortDescending));
                    StartAsyncUpgrade(dir, isDirectory);
                } else {
                    LoadSnapshot(GetOrBuildSnapshot(dir));
                }
            } else {
                LoadSnapshot(GetOrBuildSnapshot(dir));
            }
        } catch (...) {
            m_files.clear();
            m_sizes.clear();
            m_ids.clear();
        }

        // Find current index
        if (!isDirectory) {
            std::wstring currentFull = p.wstring();
            for (size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i] == currentFull) {
                    m_currentIndex = (int)i;
                    break;
                }
            }
        }
    }

    // Legacy support: We ignore `loop` and strictly use NavLoopMode instead
    std::wstring Next(bool unused = true) {
        if (m_files.empty()) return L"";

        if (m_currentIndex >= (int)m_files.size() - 1) {
            if (g_runtime.NavTraverse) {
                // Through folders
                std::wstring nextFolderImg = FindAdjacentFolderImage(true);
                if (!nextFolderImg.empty()) {
                    m_crossFolderMessage = L">>> Entering [" + std::filesystem::path(nextFolderImg).parent_path().filename().wstring() + L"] >>>";
                    return nextFolderImg;
                }
            }

            if (g_runtime.NavLoop) {
                // Loop
                m_hitEnd = true; // Signal OSD
                m_currentIndex = 0;
                return m_files[m_currentIndex];
            } else {
                // Stop at end
                m_hitEnd = true;
                return L"";
            }
        }

        m_hitEnd = false;
        m_currentIndex++;
        return m_files[m_currentIndex];
    }

    std::wstring Previous(bool unused = true) {
        if (m_files.empty()) return L"";

        if (m_currentIndex <= 0) {
            if (g_runtime.NavTraverse) {
                // Through folders
                std::wstring prevFolderImg = FindAdjacentFolderImage(false);
                if (!prevFolderImg.empty()) {
                    m_crossFolderMessage = L"<<< Entering [" + std::filesystem::path(prevFolderImg).parent_path().filename().wstring() + L"] <<<";
                    return prevFolderImg;
                }
            }

            if (g_runtime.NavLoop) {
                // Loop
                m_hitEnd = true; // Signal OSD
                m_currentIndex = (int)m_files.size() - 1;
                return m_files[m_currentIndex];
            } else {
                // Stop at start
                m_hitEnd = true;
                return L"";
            }
        }

        m_hitEnd = false;
        m_currentIndex--;
        return m_files[m_currentIndex];
    }

    std::wstring First() {
        if (m_files.empty()) return L"";
        m_hitEnd = false;
        m_currentIndex = 0;
        return m_files[m_currentIndex];
    }

    std::wstring Last() {
        if (m_files.empty()) return L"";
        m_hitEnd = false;
        m_currentIndex = (int)m_files.size() - 1;
        return m_files[m_currentIndex];
    }
    
    bool HitEnd() const { return m_hitEnd; }

    std::wstring GetCrossFolderMessage() {
        std::wstring msg = m_crossFolderMessage;
        m_crossFolderMessage.clear(); // Consume
        return msg;
    }

    std::wstring PeekNext() const {
        if (m_files.empty()) return L"";
        size_t nextIdx = (m_currentIndex + 1) % m_files.size();
        return m_files[nextIdx];
    }

    std::wstring PeekPrevious() const {
        if (m_files.empty()) return L"";
        size_t prevIdx = (m_currentIndex - 1 + m_files.size()) % m_files.size();
        return m_files[prevIdx];
    }
    
    // [Fix] Refresh metadata for current file (e.g. after Save)
    void Refresh() {
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
            try {
                namespace fs = std::filesystem;
                m_sizes[m_currentIndex] = fs::file_size(m_files[m_currentIndex]);
            } catch (...) {
                m_sizes[m_currentIndex] = 0;
            }
        }

        if (!m_currentDir.empty()) {
            InvalidateDirectoryCache(m_currentDir);
        }
    }
    
    // Status info
    // Status info
    size_t Count() const { return m_files.size(); }
    int Index() const { return m_currentIndex; }
    bool HasPendingUpgrade() const { return m_hasPendingUpgrade.load(std::memory_order_acquire); }

    // Random Access (For Gallery Virtualization)
    const std::wstring& GetFile(int index) const {
        static std::wstring empty;
        if (index < 0 || index >= (int)m_files.size()) return empty;
        return m_files[index];
    }

    int FindIndex(const std::wstring& path) const {
        auto it = std::find(m_files.begin(), m_files.end(), path);
        if (it != m_files.end()) return (int)std::distance(m_files.begin(), it);
        return -1;
    }

    const std::vector<std::wstring>& GetAllFiles() const { return m_files; }

    uintmax_t GetFileSize(int index) const {
        if (index < 0 || index >= (int)m_sizes.size()) return 0;
        return m_sizes[index];
    }
    
    // [ImageID] Get stable hash ID for image at index
    ImageID GetImageID(int index) const {
        if (index < 0 || index >= (int)m_ids.size()) return 0;
        return m_ids[index];
    }
    
    // [ImageID] Get hash ID for current image
    ImageID GetCurrentImageID() const {
        return GetImageID(m_currentIndex);
    }
    
    // [ImageID] Compute hash from path (for external use)
    static ImageID PathToImageID(const std::wstring& path) {
        return ComputePathHash(path);
    }

    bool ApplyPendingUpgradeIfReady() {
        std::shared_ptr<PendingUpgrade> pending;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pending = m_pendingUpgrade;
            if (!pending || !pending->ready.load(std::memory_order_acquire)) {
                return false;
            }
            if (pending->token != m_upgradeToken.load(std::memory_order_acquire)) {
                m_pendingUpgrade.reset();
                m_hasPendingUpgrade.store(false, std::memory_order_release);
                return false;
            }
        }

        const std::wstring currentPath = GetCurrentPath();
        LoadSnapshot(*pending->snapshot);
        RecomputeCurrentIndex(currentPath);

        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (m_pendingUpgrade == pending) {
                m_pendingUpgrade.reset();
            }
        }
        m_hasPendingUpgrade.store(false, std::memory_order_release);
        return true;
    }

private:
    struct Entry {
        std::wstring path;
        std::wstring name;
        std::wstring type;
        uintmax_t size = 0;
        uint64_t modifiedTicks = 0;
        std::string exifDate;
    };

    struct DirectorySnapshot {
        std::vector<std::wstring> files;
        std::vector<uintmax_t> sizes;
        std::vector<ImageID> ids;
    };

    struct CacheRecord {
        uint64_t directoryWriteTime = 0;
        DirectorySnapshot snapshot;
    };

    struct FolderCacheRecord {
        uint64_t directoryWriteTime = 0;
        std::vector<std::wstring> folders;
    };

    struct PendingUpgrade {
        uint64_t token = 0;
        std::shared_ptr<DirectorySnapshot> snapshot;
        std::atomic<bool> ready = false;
    };

    struct CacheKey {
        std::wstring directory;
        int sortOrder = 0;
        bool sortDescending = false;

        bool operator==(const CacheKey& other) const {
            return sortOrder == other.sortOrder
                && sortDescending == other.sortDescending
                && directory == other.directory;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& key) const {
            size_t h1 = std::hash<std::wstring>{}(key.directory);
            size_t h2 = std::hash<int>{}(key.sortOrder);
            size_t h3 = std::hash<bool>{}(key.sortDescending);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    static bool IsSupportedExtension(std::wstring ext) {
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
        const auto& supported = GetSupportedExtensionSet();
        return supported.find(ext) != supported.end();
    }

    static const std::unordered_set<std::wstring>& GetSupportedExtensionSet() {
        static const std::unordered_set<std::wstring> supported = [] {
            std::unordered_set<std::wstring> exts;
            exts.reserve(std::size(QuickView::SUPPORTED_EXTENSIONS));
            for (const auto& ext : QuickView::SUPPORTED_EXTENSIONS) {
                exts.emplace(ext);
            }
            return exts;
        }();
        return supported;
    }

    static uint64_t FileTimeToUint64(const FILETIME& fileTime) {
        ULARGE_INTEGER value{};
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return value.QuadPart;
    }

    static uint64_t TryGetDirectoryWriteTime(const std::filesystem::path& dir) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &data)) {
            return FileTimeToUint64(data.ftLastWriteTime);
        }
        return 0;
    }

    static std::string ReadExifDate(const std::wstring& path) {
        FILE* fp = _wfopen(path.c_str(), L"rb");
        if (!fp) return {};

        unsigned char buf[65536];
        size_t bytes = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        if (bytes == 0) return {};

        easyexif::EXIFInfo info;
        if (info.parseFrom(buf, (unsigned)bytes) == PARSE_EXIF_SUCCESS) {
            return info.DateTimeOriginal;
        }
        return {};
    }

    static DirectorySnapshot BuildSnapshot(const std::filesystem::path& dir, int sortOrder, bool sortDesc) {
        namespace fs = std::filesystem;

        std::vector<Entry> entries;
        const std::wstring searchPattern = (dir / L"*").wstring();

        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);

        if (findHandle == INVALID_HANDLE_VALUE) {
            return {};
        }

        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;

            std::wstring name = findData.cFileName;
            fs::path path = dir / name;
            std::wstring ext = path.extension().wstring();
            if (!IsSupportedExtension(ext)) continue;

            Entry item;
            item.path = path.wstring();
            item.name = std::move(name);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
            item.type = ext;
            item.size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
            item.modifiedTicks = FileTimeToUint64(findData.ftLastWriteTime);

            if (sortOrder == 3) {
                item.exifDate = ReadExifDate(item.path);
            }

            entries.push_back(std::move(item));
        } while (FindNextFileW(findHandle, &findData));

        FindClose(findHandle);

        std::sort(entries.begin(), entries.end(), [sortOrder, sortDesc](const Entry& a, const Entry& b) {
            int cmp = 0;
            switch (sortOrder) {
                case 1: // Name
                case 0: // Auto (Use Name Natural Sort)
                    cmp = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
                    break;
                case 2: // Modified
                    if (a.modifiedTicks < b.modifiedTicks) cmp = -1;
                    else if (a.modifiedTicks > b.modifiedTicks) cmp = 1;
                    else cmp = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
                    break;
                case 3: // Date Taken
                    if (a.exifDate.empty() && !b.exifDate.empty()) cmp = 1;
                    else if (!a.exifDate.empty() && b.exifDate.empty()) cmp = -1;
                    else {
                        cmp = a.exifDate.compare(b.exifDate);
                        if (cmp == 0) cmp = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
                    }
                    break;
                case 4: // Size
                    if (a.size < b.size) cmp = -1;
                    else if (a.size > b.size) cmp = 1;
                    else cmp = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
                    break;
                case 5: // Type
                    cmp = StrCmpLogicalW(a.type.c_str(), b.type.c_str());
                    if (cmp == 0) cmp = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
                    break;
                default:
                    cmp = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
                    break;
            }

            return sortDesc ? (cmp > 0) : (cmp < 0);
        });

        DirectorySnapshot snapshot;
        snapshot.files.reserve(entries.size());
        snapshot.sizes.reserve(entries.size());
        snapshot.ids.reserve(entries.size());

        for (const auto& entry : entries) {
            snapshot.files.push_back(entry.path);
            snapshot.sizes.push_back(entry.size);
            snapshot.ids.push_back(ComputePathHash(entry.path));
        }

        return snapshot;
    }

    static bool TryGetCachedSnapshot(const std::filesystem::path& dir, int sortOrder, bool sortDesc, DirectorySnapshot& out) {
        const CacheKey key{ dir.wstring(), sortOrder, sortDesc };
        const auto writeTime = TryGetDirectoryWriteTime(dir);

        std::lock_guard<std::mutex> lock(GetCacheMutex());
        auto& cache = GetSnapshotCache();
        auto it = cache.find(key);
        if (it != cache.end() && it->second.directoryWriteTime == writeTime) {
            out = it->second.snapshot;
            return true;
        }
        return false;
    }

    static DirectorySnapshot GetOrBuildSnapshot(const std::filesystem::path& dir, int sortOrder, bool sortDesc) {
        DirectorySnapshot cached;
        if (TryGetCachedSnapshot(dir, sortOrder, sortDesc, cached)) {
            return cached;
        }

        CacheRecord record;
        record.directoryWriteTime = TryGetDirectoryWriteTime(dir);
        record.snapshot = BuildSnapshot(dir, sortOrder, sortDesc);

        {
            std::lock_guard<std::mutex> lock(GetCacheMutex());
            GetSnapshotCache()[CacheKey{ dir.wstring(), sortOrder, sortDesc }] = record;
            PruneCacheIfNeeded();
        }

        return record.snapshot;
    }

    static DirectorySnapshot GetOrBuildSnapshot(const std::filesystem::path& dir) {
        return GetOrBuildSnapshot(dir, g_runtime.SortOrder, g_runtime.SortDescending);
    }

    static std::vector<std::wstring> EnumerateDirectoriesSorted(const std::filesystem::path& dir) {
        std::vector<std::wstring> folders;
        const std::wstring searchPattern = (dir / L"*").wstring();

        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);

        if (findHandle == INVALID_HANDLE_VALUE) {
            return folders;
        }

        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) continue;
            folders.push_back((dir / findData.cFileName).wstring());
        } while (FindNextFileW(findHandle, &findData));

        FindClose(findHandle);

        std::sort(folders.begin(), folders.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
        });

        return folders;
    }

    static std::vector<std::wstring> GetOrBuildDirectoryList(const std::filesystem::path& dir) {
        const std::wstring key = dir.wstring();
        const auto writeTime = TryGetDirectoryWriteTime(dir);

        {
            std::lock_guard<std::mutex> lock(GetCacheMutex());
            auto& cache = GetFolderCache();
            auto it = cache.find(key);
            if (it != cache.end() && it->second.directoryWriteTime == writeTime) {
                return it->second.folders;
            }
        }

        FolderCacheRecord record;
        record.directoryWriteTime = writeTime;
        record.folders = EnumerateDirectoriesSorted(dir);

        {
            std::lock_guard<std::mutex> lock(GetCacheMutex());
            GetFolderCache()[key] = record;
            PruneFolderCacheIfNeeded();
        }

        return record.folders;
    }

    static void InvalidateDirectoryCache(const std::wstring& directory) {
        std::lock_guard<std::mutex> lock(GetCacheMutex());
        auto& cache = GetSnapshotCache();
        for (auto it = cache.begin(); it != cache.end(); ) {
            if (it->first.directory == directory) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }

        GetFolderCache().erase(directory);
    }

    static std::unordered_map<CacheKey, CacheRecord, CacheKeyHash>& GetSnapshotCache() {
        static std::unordered_map<CacheKey, CacheRecord, CacheKeyHash> cache;
        return cache;
    }

    static std::unordered_map<std::wstring, FolderCacheRecord>& GetFolderCache() {
        static std::unordered_map<std::wstring, FolderCacheRecord> cache;
        return cache;
    }

    static void PruneCacheIfNeeded() {
        auto& cache = GetSnapshotCache();
        constexpr size_t kHardLimit = 64;
        if (cache.size() > kHardLimit) {
            cache.clear();
        }
    }

    static void PruneFolderCacheIfNeeded() {
        auto& cache = GetFolderCache();
        constexpr size_t kHardLimit = 64;
        if (cache.size() > kHardLimit) {
            cache.clear();
        }
    }

    static std::mutex& GetCacheMutex() {
        static std::mutex mtx;
        return mtx;
    }

    void LoadSnapshot(const DirectorySnapshot& snapshot) {
        m_files = snapshot.files;
        m_sizes = snapshot.sizes;
        m_ids = snapshot.ids;
    }

    std::wstring GetCurrentPath() const {
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_files.size()) {
            return m_files[m_currentIndex];
        }
        return {};
    }

    void RecomputeCurrentIndex(const std::wstring& preferredPath) {
        if (!preferredPath.empty()) {
            for (size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i] == preferredPath) {
                    m_currentIndex = (int)i;
                    return;
                }
            }
        }
        if (m_files.empty()) {
            m_currentIndex = -1;
        } else if (m_currentIndex >= (int)m_files.size()) {
            m_currentIndex = (int)m_files.size() - 1;
        }
    }

    void CancelPendingUpgrade() {
        m_upgradeToken.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingUpgrade.reset();
        m_hasPendingUpgrade.store(false, std::memory_order_release);
    }

    void StartAsyncUpgrade(const std::filesystem::path& dir, bool isDirectory) {
        if (isDirectory && m_files.empty()) return;
        if (!isDirectory && m_files.empty()) return;

        auto pending = std::make_shared<PendingUpgrade>();
        pending->snapshot = std::make_shared<DirectorySnapshot>();
        pending->token = m_upgradeToken.load(std::memory_order_acquire);

        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingUpgrade = pending;
        }
        m_hasPendingUpgrade.store(true, std::memory_order_release);

        const int sortOrder = g_runtime.SortOrder;
        const bool sortDescending = g_runtime.SortDescending;

        std::thread([this, pending, dir, sortOrder, sortDescending]() {
            DirectorySnapshot snapshot = GetOrBuildSnapshot(dir, sortOrder, sortDescending);
            if (pending->token != m_upgradeToken.load(std::memory_order_acquire)) {
                return;
            }

            *pending->snapshot = std::move(snapshot);
            pending->ready.store(true, std::memory_order_release);
        }).detach();
    }

    std::wstring FindAdjacentFolderImage(bool next) {
        if (m_files.empty()) return L"";

        namespace fs = std::filesystem;
        fs::path currentDir = fs::path(m_files[0]).parent_path();
        
        // [Logic Upgrade] Through Subfolders: 
        // 1. If moving forward, check if currentDir has subfolders first.
        if (next) {
            auto subfolders = GetOrBuildDirectoryList(currentDir);
            if (!subfolders.empty()) {
                for (const auto& sub : subfolders) {
                    FileNavigator tempNav;
                    tempNav.Initialize(sub, false);
                    if (tempNav.Count() > 0) return tempNav.First();
                }
            }
        }

        // 2. Siblings navigation
        fs::path parentDir = currentDir.parent_path();
        if (parentDir.empty() || parentDir == currentDir) return L"";

        std::vector<std::wstring> folders = GetOrBuildDirectoryList(parentDir);

        if (folders.empty()) return L"";

        std::wstring currentStr = currentDir.wstring();
        auto it = std::find(folders.begin(), folders.end(), currentStr);
        int idx = (it == folders.end()) ? -1 : (int)std::distance(folders.begin(), it);

        int startIdx = idx;
        while (true) {
            if (next) idx++; else idx--;

            // Boundary logic
            if (idx < 0 || idx >= (int)folders.size()) {
                if (g_runtime.NavLoop) {
                    // Loop globally: wrap around
                    idx = (idx < 0) ? (int)folders.size() - 1 : 0;
                } else {
                    return L""; // Stop at global boundary
                }
            }
            
            if (idx == startIdx) break; // Wrapped full circle and found nothing

            FileNavigator tempNav;
            tempNav.Initialize(folders[idx], false);
            if (tempNav.Count() > 0) {
                 return next ? tempNav.First() : tempNav.Last();
            }
        }

        return L"";
    }

    std::vector<std::wstring> m_files;
    std::vector<uintmax_t> m_sizes;
    std::vector<ImageID> m_ids;  // [ImageID] Precomputed path hashes
    std::wstring m_currentDir;
    std::atomic<uint64_t> m_upgradeToken{ 1 };
    std::atomic<bool> m_hasPendingUpgrade{ false };
    mutable std::mutex m_pendingMutex;
    std::shared_ptr<PendingUpgrade> m_pendingUpgrade;
    int m_currentIndex = -1;
    bool m_hitEnd = false;
    std::wstring m_crossFolderMessage;
};
