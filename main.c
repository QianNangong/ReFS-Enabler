#include <windows.h>
#include <dismapi.h>

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

PWSTR WINAPI GetTempMountPath(void)
{
    static WCHAR szTempMountPath[NT_MAX_PATH + 1] = { 0 };
    if (szTempMountPath[0] != L'\0')
    {
        return szTempMountPath;
    }
    WCHAR szSysTempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, szSysTempPath);
    WCHAR szTempBuf[9] = { 0 };
    UINT nCount = 8;
    GenerateRandomString(szTempBuf, &nCount);
    snwprintf(szTempMountPath, NT_MAX_PATH, L"\\\\?\\%s\\%s", szSysTempPath, szTempBuf);
    if (!CreateDirectoryW(szTempMountPath, NULL))
    {
        szTempMountPath[0] = '\0';
        return GetTempMountPath();
    }
    return szTempMountPath;
}

PWSTR WINAPI MountImage(PCWSTR pWimFile, UINT nIndex)
{
    if (FAILED(DismMountImage(pWimFile, GetTempMountPath(), nIndex, NULL, DismImageIndex, DISM_MOUNT_READWRITE, NULL, NULL, NULL)))
    {
        return NULL;
    }
    return GetTempMountPath();
}

BOOL WINAPI EnableReFSInRegistry(PCWSTR pMountPath)
{
    HKEY hSystemRootKey = INVALID_HANDLE_VALUE;
    HKEY hFeatureKey = INVALID_HANDLE_VALUE;
    WCHAR szRegistryKeyFile[NT_MAX_PATH + 1] = { 0 };
    snwprintf(szRegistryKeyFile, NT_MAX_PATH, L"%s\\Windows\\System32\\config\\SYSTEM");
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

BOOL WINAPI UnmountImage(PCWSTR pMountPath)
{
    return SUCCEEDED(DismUnmountImage(pMountPath, DISM_COMMIT_IMAGE, NULL, NULL, NULL));
}

UINT WINAPI EnableReFS(PCWSTR pWimFile)
{
    UINT nImgCount = 0;
    DismImageInfo* pImgInfo = NULL;
    if (FAILED(DismGetImageInfo(pWimFile, &pImgInfo, &nImgCount)))
    {
        PRINT_ERR(L"Failed to open wim file %s. HRESULT = 0x%08lX", pWimFile, GetLastError());
        return 0;
    }
    if (nImgCount == 0)
    {
        PRINT_ERR(L"No suitable image found in %s.", pWimFile);
        return 0;
    }
    UINT nEnabled = 0;
    for (UINT i = 0; i < nImgCount; ++i)
    {
        BOOL bSuccess = TRUE;
        DismImageInfo info = pImgInfo[i];
        if (info.MajorVersion < 10 || info.Build < 25281 || !lstrcmpW(L"WindowsPE", info.EditionId) || !lstrcmpW(L"WindowsPE", info.InstallationType))
        {
            continue;
        }
        PRINT_OUT(L"Enable ReFS Installation on image \"%s\" #%u...\r\n", info.ImageName, info.ImageIndex);
        PWSTR pszMountPath = MountImage(pWimFile, i);
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
    snwprintf(szWimFile, NT_MAX_PATH, L"\\\\?\\%s", argv[1]);
    UINT nEnabled = EnableReFS(szWimFile);
    if (nEnabled != 0)
    {
        PRINT_OUT(L"Done! %d image%s enabled.\r\n", nEnabled, nEnabled == 1 ? L"": L"s");
    }
    else
    {
        PRINT_ERR(L"No suitable image found in %s.\r\n", argv[1]);
    }
    DismShutdown();
END:
    if (!IsRunningFromTerminal())
    {
        PRINT_OUT(L"Press any key to exit...");
        getchar();
    }
    return 0;
}