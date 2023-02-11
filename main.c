#include <windows.h>
#include <dismapi.h>
#include <pathcch.h>

#include <stdio.h>
#include <string.h>

#ifdef PRINT_ERR
#undef PRINT_ERR
#endif

#ifdef PRINT_OUT
#undef PRINT_OUT
#endif

#define PRINT_ERR(...) fwprintf(stderr, __VA_ARGS__)
#define PRINT_OUT(...) fwprintf(stdout, __VA_ARGS__)

#ifdef NT_MAX_PATH
#undef NT_MAX_PATH
#endif

#define NT_MAX_PATH 32767

static const WCHAR CHARSET[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

UINT WINAPI GenerateRandomString(PWSTR lpBuffer, UINT* dwLength)
{
    if (*dwLength == 0)
    {
        *dwLength = 8;
    }
    for (UINT i = 0; i < *dwLength; ++i)
    {
        lpBuffer[i] = CHARSET[rand() % (sizeof(CHARSET) / sizeof(WCHAR))];
    }
    return *dwLength;
}

PCWSTR WINAPI GetTempMountPath(void)
{
    static WCHAR* szTempMountPath = NULL;
    if (szTempMountPath != NULL)
    {
        return szTempMountPath;
    }
    szTempMountPath = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NT_MAX_PATH + 1);
    WCHAR szSysTempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, szSysTempPath);
    WCHAR szTempBuf[9] = { 0 };
    UINT nCount = 8;
    GenerateRandomString(szTempBuf, &nCount);
    if (FAILED(PathCchCombineEx(szTempMountPath, NT_MAX_PATH, szSysTempPath, szTempBuf, PATHCCH_ALLOW_LONG_PATHS)))
    {
        HeapFree(GetProcessHeap(), 0, szTempMountPath);
        szTempMountPath = NULL;
        return NULL;
    }
    if (!CreateDirectoryW(szTempMountPath, NULL))
    {
        szTempMountPath[0] = '\0';
        return GetTempMountPath();
    }
    return szTempMountPath;
}

void WINAPI Cleanup(void)
{
    PCWSTR pszPath = GetTempMountPath();
    if (pszPath)
    {
        HeapFree(GetProcessHeap(), 0, (LPVOID) pszPath);
    }
}

PCWSTR WINAPI MountImage(PCWSTR pszWimFile, UINT nIndex)
{
    PCWSTR pszTempPath = GetTempMountPath();
    if (!pszTempPath)
    {
        return NULL;
    }
    if (FAILED(DismMountImage(pszWimFile, pszTempPath, nIndex, NULL, DismImageIndex, DISM_MOUNT_READWRITE, NULL, NULL, NULL)))
    {
        return NULL;
    }
    return pszTempPath;
}

BOOL WINAPI EnableReFSInRegistry(PCWSTR pszMountPath)
{
    HKEY hSystemRootKey = INVALID_HANDLE_VALUE;
    HKEY hFeatureKey = INVALID_HANDLE_VALUE;
    WCHAR szRegistryKeyFile[NT_MAX_PATH + 1] = { 0 };
    if (FAILED(PathCchCombineEx(szRegistryKeyFile, NT_MAX_PATH, pszMountPath, L"Windows\\System32\\config\\SYSTEM", PATHCCH_ALLOW_LONG_PATHS)))
    {
        return FALSE;
    }
    LRESULT lr = RegLoadAppKeyW(szRegistryKeyFile, &hSystemRootKey, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0);
    if (lr != ERROR_SUCCESS || hSystemRootKey == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    lr = RegCreateKeyExW(hSystemRootKey, L"ControlSet001\\Control\\FeatureManagement\\Overrides\\8\\3689412748", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hFeatureKey, NULL);
    if (lr != ERROR_SUCCESS || hFeatureKey == INVALID_HANDLE_VALUE)
    {
        RegCloseKey(hSystemRootKey);
        return FALSE;
    }
    const DWORD dwEnabledState = 2;
    const DWORD dwEnabledStateOptions = 0;
    RegSetKeyValueW(hFeatureKey, NULL, L"EnabledState", REG_DWORD, &dwEnabledState, sizeof(DWORD));
    RegSetKeyValueW(hFeatureKey, NULL, L"EnabledStateOptions", REG_DWORD, &dwEnabledStateOptions, sizeof(DWORD));
    RegCloseKey(hFeatureKey);
    RegCloseKey(hFeatureKey);
    return TRUE;
}

BOOL WINAPI UnmountImage(PCWSTR pszMountPath)
{
    return SUCCEEDED(DismUnmountImage(pszMountPath, DISM_COMMIT_IMAGE, NULL, NULL, NULL));
}

UINT WINAPI EnableReFS(PCWSTR pszWimFile)
{
    UINT nImgCount = 0;
    DismImageInfo* pImgInfo = NULL;
    if (FAILED(DismGetImageInfo(pszWimFile, &pImgInfo, &nImgCount)))
    {
        PRINT_ERR(L"Failed to open wim file %s. HRESULT = 0x%08lX\r\n", pszWimFile, GetLastError());
        return 0;
    }
    if (nImgCount == 0)
    {
        PRINT_ERR(L"No suitable image found in %s.\r\n", pszWimFile);
        return 0;
    }
    UINT nEnabled = 0;
    for (UINT i = 0; i < nImgCount; ++i)
    {
        BOOL bSuccess = TRUE;
        DismImageInfo info = pImgInfo[i];
        if (info.MajorVersion < 10 || info.Build < 25281 || lstrcmpW(L"WindowsPE", info.EditionId) || lstrcmpW(L"WindowsPE", info.InstallationType))
        {
            continue;
        }
        PRINT_OUT(L"Enable ReFS Installation on image \"%s\" #%u...\r\n", info.ImageName, info.ImageIndex);
        PCWSTR pszMountPath = MountImage(pszWimFile, i);
        if (pszMountPath == NULL)
        {
            PRINT_ERR(L"[#%u] Failed to mount image. HRESULT = 0x%08lX\r\n", info.ImageIndex, GetLastError());
            continue;
        }
        else
        {
            PRINT_OUT(L"[#%u] Image mounted.\r\n", info.ImageIndex);
        }
        if (!EnableReFSInRegistry(pszMountPath))
        {
            PRINT_ERR(L"[#%u] Failed to enable ReFS in registry. HRESULT = 0x%08lx\r\n", info.ImageIndex, GetLastError());
            bSuccess = FALSE;
        }
        else
        {
            PRINT_OUT(L"[#%u] Feature 42189933 enabled.\r\n", info.ImageIndex);
        }
        if (!UnmountImage(pszMountPath))
        {
            PRINT_ERR(L"[#%u] Failed to unmount image. HRESULT = 0x%08lx\r\n", info.ImageIndex, GetLastError());
            continue;
        }
        else
        {
            PRINT_OUT(L"[#%u] Image committed and unmounted.\r\n", info.ImageIndex);
        }
        if (bSuccess)
        {
            ++nEnabled;
        }
    }
    DismDelete(pImgInfo);
    RemoveDirectoryW(GetTempMountPath());
    return nEnabled;
}

BOOL WINAPI IsRunningFromTerminal(void)
{
    DWORD szProcIDs[64] = { 0 };
    DWORD dwProcCount = GetConsoleProcessList(szProcIDs, 64);
    return dwProcCount > 1;
}

int wmain(int argc, const wchar_t* argv[])
{
    if (argc != 2)
    {
        PRINT_ERR(L"Usage:\r\n\t%s path\\to\\boot.wim\r\n", argv[0]);
        goto END;
    }

    if (FAILED(DismInitialize(DismLogErrorsWarnings, NULL, NULL)))
    {
        PRINT_ERR(L"Failed to initialize Dism. HRESULT = 0x%08lX\r\n", GetLastError());
        goto END;
    }
    srand((unsigned int)timeGetTime());
    WCHAR szWimFile[NT_MAX_PATH + 1] = { 0 };
    GetFullPathNameW(argv[1], NT_MAX_PATH, szWimFile, NULL);
    UINT nEnabled = EnableReFS(szWimFile);
    if (nEnabled != 0)
    {
        PRINT_OUT(L"Done! %d image%s enabled.\r\n", nEnabled, nEnabled == 1 ? L"": L"s");
    }
    else
    {
        PRINT_ERR(L"No suitable image found in %s.\r\n", szWimFile);
    }
    DismShutdown();
    Cleanup();
END:
    if (!IsRunningFromTerminal())
    {
        PRINT_OUT(L"Press any key to exit...");
        getchar();
    }
    return 0;
}