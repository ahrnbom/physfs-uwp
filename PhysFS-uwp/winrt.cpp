/*
* Windows RT support routines for PhysicsFS.
*
* Please see the file LICENSE.txt in the source's root directory.
*
*  This file written by Ryan C. Gordon, and made sane by Gregory S. Read, and modified for Windows RT by Martin Ahrnbom.
*/

#define __PHYSICSFS_INTERNAL__
#include "physfs_platforms.h"

#ifdef PHYSFS_PLATFORM_WINRT

/* Forcibly disable UNICODE, since we manage this ourselves. */
#ifdef UNICODE
#undef UNICODE
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#include "physfs_internal.h"

#define LOWORDER_UINT64(pos) ((PHYSFS_uint32) (pos & 0xFFFFFFFF))
#define HIGHORDER_UINT64(pos) ((PHYSFS_uint32) ((pos >> 32) & 0xFFFFFFFF))

/*
* Users without the platform SDK don't have this defined.  The original docs
*  for SetFilePointer() just said to compare with 0xFFFFFFFF, so this should
*  work as desired.
*/
#define PHYSFS_INVALID_SET_FILE_POINTER  0xFFFFFFFF

/* just in case... */
#define PHYSFS_INVALID_FILE_ATTRIBUTES   0xFFFFFFFF

/* Not defined before the Vista SDK. */
#define PHYSFS_IO_REPARSE_TAG_SYMLINK    0xA000000C


#define UTF8_TO_UNICODE_STACK_MACRO(w_assignto, str) { \
    if (str == NULL) \
        w_assignto = NULL; \
    else { \
        const PHYSFS_uint64 len = (PHYSFS_uint64) ((strlen(str) + 1) * 2); \
        w_assignto = (WCHAR *) __PHYSFS_smallAlloc(len); \
        if (w_assignto != NULL) \
            PHYSFS_utf8ToUcs2(str, (PHYSFS_uint16 *) w_assignto, len); \
    } \
} \

static PHYSFS_uint64 wStrLen(const WCHAR *wstr)
{
	PHYSFS_uint64 len = 0;
	while (*(wstr++))
		len++;
	return(len);
} /* wStrLen */

static char *unicodeToUtf8Heap(const WCHAR *w_str)
{
	char *retval = NULL;
	if (w_str != NULL)
	{
		void *ptr = NULL;
		const PHYSFS_uint64 len = (wStrLen(w_str) * 4) + 1;
		retval = (char*)allocator.Malloc(len);
		BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
		PHYSFS_utf8FromUcs2((const PHYSFS_uint16 *)w_str, retval, len);
		ptr = allocator.Realloc(retval, strlen(retval) + 1); /* shrink. */
		if (ptr != NULL)
			retval = (char *)ptr;
	} /* if */
	return(retval);
} /* unicodeToUtf8Heap */


static char *codepageToUtf8Heap(const char *cpstr)
{
	char *retval = NULL;
	if (cpstr != NULL)
	{
		const int len = (int)(strlen(cpstr) + 1);
		WCHAR *wbuf = (WCHAR *)__PHYSFS_smallAlloc(len * sizeof(WCHAR));
		BAIL_IF_MACRO(wbuf == NULL, ERR_OUT_OF_MEMORY, NULL);
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, cpstr, len, wbuf, len);
		retval = (char *)allocator.Malloc(len * 4);
		if (retval == NULL)
			__PHYSFS_setError(ERR_OUT_OF_MEMORY);
		else
			PHYSFS_utf8FromUcs2((const PHYSFS_uint16*)wbuf, retval, len * 4);
		__PHYSFS_smallFree(wbuf);
	} /* if */
	return(retval);
} /* codepageToUtf8Heap */


typedef struct
{
	HANDLE handle;
	int readonly;
} WinApiFile;


static char *userDir = NULL;
static int osHasUnicode = 0;


/* pointers for APIs that may not exist on some Windows versions... */
static HANDLE libKernel32 = NULL;
static HANDLE libUserEnv = NULL;
static HANDLE libAdvApi32 = NULL;
static DWORD(WINAPI *pGetModuleFileNameW)(HMODULE, LPWCH, DWORD);
static BOOL(WINAPI *pGetUserProfileDirectoryW)(HANDLE, LPWSTR, LPDWORD);
static BOOL(WINAPI *pGetUserNameW)(LPWSTR, LPDWORD);
static DWORD(WINAPI *pGetFileAttributesW)(LPCWSTR);
static HANDLE(WINAPI *pFindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
static BOOL(WINAPI *pFindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
static DWORD(WINAPI *pGetCurrentDirectoryW)(DWORD, LPWSTR);
static BOOL(WINAPI *pDeleteFileW)(LPCWSTR);
static BOOL(WINAPI *pRemoveDirectoryW)(LPCWSTR);
static BOOL(WINAPI *pCreateDirectoryW)(LPCWSTR, LPSECURITY_ATTRIBUTES);
static BOOL(WINAPI *pGetFileAttributesExA)
(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static BOOL(WINAPI *pGetFileAttributesExW)
(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static DWORD(WINAPI *pFormatMessageW)
(DWORD, LPCVOID, DWORD, DWORD, LPWSTR, DWORD, va_list *);
static HANDLE(WINAPI *pCreateFileW)
(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static DWORD WINAPI fallbackFormatMessageW(DWORD dwFlags, LPCVOID lpSource,
	DWORD dwMessageId, DWORD dwLangId,
	LPWSTR lpBuf, DWORD nSize,
	va_list *Arguments)
{
	char *cpbuf = (char *)__PHYSFS_smallAlloc(nSize);
	DWORD retval = FormatMessageA(dwFlags, lpSource, dwMessageId, dwLangId,
		cpbuf, nSize, Arguments);
	if (retval > 0)
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, cpbuf, retval, lpBuf, nSize);
	__PHYSFS_smallFree(cpbuf);
	return(retval);
} /* fallbackFormatMessageW */

static DWORD WINAPI fallbackGetModuleFileNameW(HMODULE hMod, LPWCH lpBuf,
	DWORD nSize)
{
	char *cpbuf = (char *)__PHYSFS_smallAlloc(nSize);
	DWORD retval = GetModuleFileNameA(hMod, cpbuf, nSize);
	if (retval > 0)
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, cpbuf, retval, lpBuf, nSize);
	__PHYSFS_smallFree(cpbuf);
	return(retval);
} /* fallbackGetModuleFileNameW */

static DWORD WINAPI fallbackGetFileAttributesW(LPCWSTR fname)
{
	DWORD retval = 0;
	const int buflen = (int)(wStrLen(fname) + 1);
	char *cpstr = (char *)__PHYSFS_smallAlloc(buflen);
	WideCharToMultiByte(CP_ACP, 0, fname, buflen, cpstr, buflen, NULL, NULL);
	//retval = GetFileAttributesA(cpstr);
	const BOOL res = GetFileAttributesExA(cpstr, GetFileExInfoStandard, &retval);
	__PHYSFS_smallFree(cpstr);

	if (!res) 
	{
		return 0;
	}

	return(retval);
} /* fallbackGetFileAttributesW */

static DWORD WINAPI fallbackGetCurrentDirectoryW(DWORD buflen, LPWSTR buf)
{
	const wchar_t* path = Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data();
	wchar_t path2[1024];
	wcscpy_s(path2, path);
	wcscat_s(path2, L"\\");
	buf = path2;

	int i;
	for (i = 0; buf[i] != '\0'; ++i);

	return i;
} /* fallbackGetCurrentDirectoryW */

static BOOL WINAPI fallbackRemoveDirectoryW(LPCWSTR dname)
{
	BOOL retval = 0;
	const int buflen = (int)(wStrLen(dname) + 1);
	char *cpstr = (char *)__PHYSFS_smallAlloc(buflen);
	WideCharToMultiByte(CP_ACP, 0, dname, buflen, cpstr, buflen, NULL, NULL);
	retval = RemoveDirectoryA(cpstr);
	__PHYSFS_smallFree(cpstr);
	return(retval);
} /* fallbackRemoveDirectoryW */

static BOOL WINAPI fallbackCreateDirectoryW(LPCWSTR dname,
	LPSECURITY_ATTRIBUTES attr)
{
	BOOL retval = 0;
	const int buflen = (int)(wStrLen(dname) + 1);
	char *cpstr = (char *)__PHYSFS_smallAlloc(buflen);
	WideCharToMultiByte(CP_ACP, 0, dname, buflen, cpstr, buflen, NULL, NULL);
	retval = CreateDirectoryA(cpstr, attr);
	__PHYSFS_smallFree(cpstr);
	return(retval);
} /* fallbackCreateDirectoryW */

static BOOL WINAPI fallbackDeleteFileW(LPCWSTR fname)
{
	BOOL retval = 0;
	const int buflen = (int)(wStrLen(fname) + 1);
	char *cpstr = (char *)__PHYSFS_smallAlloc(buflen);
	WideCharToMultiByte(CP_ACP, 0, fname, buflen, cpstr, buflen, NULL, NULL);
	retval = DeleteFileA(cpstr);
	__PHYSFS_smallFree(cpstr);
	return(retval);
} /* fallbackDeleteFileW */

static HANDLE WINAPI fallbackCreateFileW(LPCWSTR fname,
	DWORD dwDesiredAccess, DWORD dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttrs,
	DWORD dwCreationDisposition,
	DWORD dwFlagsAndAttrs, HANDLE hTemplFile)
{
	HANDLE retval;
	const int buflen = (int)(wStrLen(fname) + 1);
	char *cpstr = (char *)__PHYSFS_smallAlloc(buflen);
	WideCharToMultiByte(CP_ACP, 0, fname, buflen, cpstr, buflen, NULL, NULL);
	//retval = CreateFileA(cpstr, dwDesiredAccess, dwShareMode, lpSecurityAttrs, dwCreationDisposition, dwFlagsAndAttrs, hTemplFile);
	retval = CreateFile2(fname, dwDesiredAccess, dwShareMode, dwCreationDisposition, NULL);
	__PHYSFS_smallFree(cpstr);
	return(retval);
} /* fallbackCreateFileW */

#if (PHYSFS_MINIMUM_GCC_VERSION(3,3))
typedef FARPROC __attribute__((__may_alias__)) PHYSFS_FARPROC;
#else
typedef FARPROC PHYSFS_FARPROC;
#endif


static void symLookup(HMODULE dll, PHYSFS_FARPROC *addr, const char *sym,
	int reallyLook, PHYSFS_FARPROC fallback)
{
	PHYSFS_FARPROC proc;
	proc = (PHYSFS_FARPROC)((reallyLook) ? GetProcAddress(dll, sym) : NULL);
	if (proc == NULL)
		proc = fallback;  /* may also be NULL. */
	*addr = proc;
} /* symLookup */


static int findApiSymbols(void)
{
	HMODULE dll = NULL;

#define LOOKUP_NOFALLBACK(x, reallyLook) \
        symLookup(dll, (PHYSFS_FARPROC *) &p##x, #x, reallyLook, NULL)

#define LOOKUP(x, reallyLook) \
        symLookup(dll, (PHYSFS_FARPROC *) &p##x, #x, \
                  reallyLook, (PHYSFS_FARPROC) fallback##x)

	/* Apparently Win9x HAS the Unicode entry points, they just don't WORK. */
	/*  ...so don't look them up unless we're on NT+. (see osHasUnicode.) */

	libUserEnv = LoadPackagedLibrary(L"userenv.dll", 0);
	dll = (HMODULE)libUserEnv;
	if (dll != NULL)
		LOOKUP_NOFALLBACK(GetUserProfileDirectoryW, osHasUnicode);

	/* !!! FIXME: what do they call advapi32.dll on Win64? */
	libAdvApi32 = LoadPackagedLibrary(L"advapi32.dll", 0);
	dll = (HMODULE)libAdvApi32;
	//if (dll != NULL)
	//	LOOKUP(GetUserNameW, osHasUnicode);

	/* !!! FIXME: what do they call kernel32.dll on Win64? */
	libKernel32 = LoadPackagedLibrary(L"kernel32.dll", 0);
	dll = (HMODULE)libKernel32;
	if (dll != NULL)
	{
		LOOKUP_NOFALLBACK(GetFileAttributesExA, 1);
		LOOKUP_NOFALLBACK(GetFileAttributesExW, osHasUnicode);
		LOOKUP_NOFALLBACK(FindFirstFileW, osHasUnicode);
		LOOKUP_NOFALLBACK(FindNextFileW, osHasUnicode);
		LOOKUP(GetModuleFileNameW, osHasUnicode);
		LOOKUP(FormatMessageW, osHasUnicode);
		LOOKUP(GetFileAttributesW, osHasUnicode);
		LOOKUP(GetCurrentDirectoryW, osHasUnicode);
		LOOKUP(CreateDirectoryW, osHasUnicode);
		LOOKUP(RemoveDirectoryW, osHasUnicode);
		LOOKUP(CreateFileW, osHasUnicode);
		LOOKUP(DeleteFileW, osHasUnicode);
	} /* if */

#undef LOOKUP_NOFALLBACK
#undef LOOKUP

	return(1);
} /* findApiSymbols */


const char *__PHYSFS_platformDirSeparator = "\\";


/*
* Figure out what the last failing Windows API call was, and
*  generate a human-readable string for the error message.
*
* The return value is a static buffer that is overwritten with
*  each call to this function.
*/
static const char *winApiStrError(void)
{
	static char utf8buf[255];
	WCHAR msgbuf[255];
	WCHAR *ptr;
	DWORD rc = pFormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		msgbuf, __PHYSFS_ARRAYLEN(msgbuf),
		NULL);

	if (rc == 0)
		msgbuf[0] = '\0';  /* oh well. */

						   /* chop off newlines. */
	for (ptr = msgbuf; *ptr; ptr++)
	{
		if ((*ptr == '\n') || (*ptr == '\r'))
		{
			*ptr = '\0';
			break;
		} /* if */
	} /* for */

	  /* may truncate, but oh well. */
	PHYSFS_utf8FromUcs2((PHYSFS_uint16 *)msgbuf, utf8buf, sizeof(utf8buf));
	return((const char *)utf8buf);
} /* winApiStrError */


static char *getExePath(void)
{
	const wchar_t* path = Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data();
	wchar_t path2[1024];
	wcscpy_s(path2, path);
	wcscat_s(path2, L"\\");
	return unicodeToUtf8Heap(path2);
} /* getExePath */


  /*
  * Try to make use of GetUserProfileDirectoryW(), which isn't available on
  *  some common variants of Win32. If we can't use this, we just punt and
  *  use the physfs base dir for the user dir, too.
  *
  * On success, module-scope variable (userDir) will have a pointer to
  *  a malloc()'d string of the user's profile dir, and a non-zero value is
  *  returned. If we can't determine the profile dir, (userDir) will
  *  be NULL, and zero is returned.
  */
static int determineUserDir(void)
{
	if (userDir != NULL)
		return(1);  /* already good to go. */

	const wchar_t* path = Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data();
	wchar_t path2[1024];
	wcscpy_s(path2, path);
	wcscat_s(path2, L"\\");

	userDir = unicodeToUtf8Heap(path2);

	if (userDir == NULL)  /* couldn't get profile for some reason. */
	{
		/* Might just be a non-NT system; resort to the basedir. */
		userDir = getExePath();
		BAIL_IF_MACRO(userDir == NULL, NULL, 0); /* STILL failed?! */
	} /* if */

	return(1);  /* We made it: hit the showers. */
} /* determineUserDir */

void __PHYSFS_platformDetectAvailableCDs(PHYSFS_StringCallback cb, void *data)
{
	
} /* __PHYSFS_platformDetectAvailableCDs */


char *__PHYSFS_platformCalcBaseDir(const char *argv0)
{
	if ((argv0 != NULL) && (strchr(argv0, '\\') != NULL))
		return(NULL); /* default behaviour can handle this. */

	return(getExePath());
} /* __PHYSFS_platformCalcBaseDir */


char *__PHYSFS_platformGetUserName(void)
{
	DWORD bufsize = 0;
	char *retval = NULL;

	if (pGetUserNameW(NULL, &bufsize) == 0)  /* This SHOULD fail. */
	{
		LPWSTR wbuf = (LPWSTR)__PHYSFS_smallAlloc(bufsize * sizeof(WCHAR));
		BAIL_IF_MACRO(wbuf == NULL, ERR_OUT_OF_MEMORY, NULL);
		if (pGetUserNameW(wbuf, &bufsize) == 0)  /* ?! */
			__PHYSFS_setError(winApiStrError());
		else
			retval = unicodeToUtf8Heap(wbuf);
		__PHYSFS_smallFree(wbuf);
	} /* if */

	return(retval);
} /* __PHYSFS_platformGetUserName */


char *__PHYSFS_platformGetUserDir(void)
{
	char *retval = (char *)allocator.Malloc(strlen(userDir) + 1);
	BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);
	strcpy(retval, userDir); /* calculated at init time. */
	return(retval);
} /* __PHYSFS_platformGetUserDir */


void *__PHYSFS_platformGetThreadID(void)
{
	return((void *)((size_t)GetCurrentThreadId()));
} /* __PHYSFS_platformGetThreadID */


static int doPlatformExists(LPWSTR wpath)
{
	WIN32_FILE_ATTRIBUTE_DATA a;

	// Returns non-zero if successful
	BOOL retval = GetFileAttributesExW(wpath, GetFileExInfoStandard, &a);

	return(retval);
} /* doPlatformExists */


int __PHYSFS_platformExists(const char *fname)
{
	int retval = 0;
	LPWSTR wpath;
	UTF8_TO_UNICODE_STACK_MACRO(wpath, fname);
	BAIL_IF_MACRO(wpath == NULL, ERR_OUT_OF_MEMORY, 0);
	retval = doPlatformExists(wpath);
	__PHYSFS_smallFree(wpath);
	return(retval);
} /* __PHYSFS_platformExists */


static int isSymlinkAttrs(const DWORD attr, const DWORD tag)
{
	return ((attr & FILE_ATTRIBUTE_REPARSE_POINT) &&
		(tag == PHYSFS_IO_REPARSE_TAG_SYMLINK));
} /* isSymlinkAttrs */


int __PHYSFS_platformIsSymLink(const char *fname)
{
	/* !!! FIXME:
	* Windows Vista can have NTFS symlinks. Can older Windows releases have
	*  them when talking to a network file server? What happens when you
	*  mount a NTFS partition on XP that was plugged into a Vista install
	*  that made a symlink?
	*/

	int retval = 0;
	LPWSTR wpath;
	HANDLE dir;
	WIN32_FIND_DATAW entw;

	/* no unicode entry points? Probably no symlinks. */
	BAIL_IF_MACRO(pFindFirstFileW == NULL, NULL, 0);

	UTF8_TO_UNICODE_STACK_MACRO(wpath, fname);
	BAIL_IF_MACRO(wpath == NULL, ERR_OUT_OF_MEMORY, 0);

	/* !!! FIXME: filter wildcard chars? */
	dir = pFindFirstFileW(wpath, &entw);
	if (dir != INVALID_HANDLE_VALUE)
	{
		retval = isSymlinkAttrs(entw.dwFileAttributes, entw.dwReserved0);
		FindClose(dir);
	} /* if */

	__PHYSFS_smallFree(wpath);
	return(retval);
} /* __PHYSFS_platformIsSymlink */


int __PHYSFS_platformIsDirectory(const char *fname)
{
	int retval = 0;
	LPWSTR wpath;
	UTF8_TO_UNICODE_STACK_MACRO(wpath, fname);
	BAIL_IF_MACRO(wpath == NULL, ERR_OUT_OF_MEMORY, 0);
	//retval = ((pGetFileAttributesW(wpath) & FILE_ATTRIBUTE_DIRECTORY) != 0);
	WIN32_FILE_ATTRIBUTE_DATA file_info;
	const BOOL res = GetFileAttributesExW(wpath, GetFileExInfoStandard, &file_info);
	if (res) {
		retval = ((file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
	}

	__PHYSFS_smallFree(wpath);
	return(retval);
} /* __PHYSFS_platformIsDirectory */


char *__PHYSFS_platformCvtToDependent(const char *prepend,
	const char *dirName,
	const char *append)
{
	int len = ((prepend) ? strlen(prepend) : 0) +
		((append) ? strlen(append) : 0) +
		strlen(dirName) + 1;
	char *retval = (char *)allocator.Malloc(len);
	char *p;

	BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);

	if (prepend)
		strcpy(retval, prepend);
	else
		retval[0] = '\0';

	strcat(retval, dirName);

	if (append)
		strcat(retval, append);

	for (p = strchr(retval, '/'); p != NULL; p = strchr(p + 1, '/'))
		*p = '\\';

	return(retval);
} /* __PHYSFS_platformCvtToDependent */


void __PHYSFS_platformEnumerateFiles(const char *dirname,
	int omitSymLinks,
	PHYSFS_EnumFilesCallback callback,
	const char *origdir,
	void *callbackdata)
{
	const int unicode = (pFindFirstFileW != NULL) && (pFindNextFileW != NULL);
	HANDLE dir = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATA ent;
	WIN32_FIND_DATAW entw;
	size_t len = strlen(dirname);
	char *searchPath = NULL;
	WCHAR *wSearchPath = NULL;
	char *utf8 = NULL;

	/* Allocate a new string for path, maybe '\\', "*", and NULL terminator */
	searchPath = (char *)__PHYSFS_smallAlloc(len + 3);
	if (searchPath == NULL)
		return;

	/* Copy current dirname */
	strcpy(searchPath, dirname);

	/* if there's no '\\' at the end of the path, stick one in there. */
	if (searchPath[len - 1] != '\\')
	{
		searchPath[len++] = '\\';
		searchPath[len] = '\0';
	} /* if */

	  /* Append the "*" to the end of the string */
	strcat(searchPath, "*");

	UTF8_TO_UNICODE_STACK_MACRO(wSearchPath, searchPath);
	if (wSearchPath == NULL)
		return;  /* oh well. */

	if (unicode)
		dir = pFindFirstFileW(wSearchPath, &entw);
	else
	{
		const int len = (int)(wStrLen(wSearchPath) + 1);
		char *cp = (char *)__PHYSFS_smallAlloc(len);
		if (cp != NULL)
		{
			WideCharToMultiByte(CP_ACP, 0, wSearchPath, len, cp, len, 0, 0);
			//dir = FindFirstFileA(cp, &ent);
			dir = FindFirstFileExA(cp, FindExInfoStandard, &ent, FindExSearchNameMatch, NULL, 0);
			__PHYSFS_smallFree(cp);
		} /* if */
	} /* else */

	__PHYSFS_smallFree(wSearchPath);
	__PHYSFS_smallFree(searchPath);
	if (dir == INVALID_HANDLE_VALUE)
		return;

	if (unicode)
	{
		do
		{
			const DWORD attr = entw.dwFileAttributes;
			const DWORD tag = entw.dwReserved0;
			const WCHAR *fn = entw.cFileName;
			if ((fn[0] == '.') && (fn[1] == '\0'))
				continue;
			if ((fn[0] == '.') && (fn[1] == '.') && (fn[2] == '\0'))
				continue;
			if ((omitSymLinks) && (isSymlinkAttrs(attr, tag)))
				continue;

			utf8 = unicodeToUtf8Heap(fn);
			if (utf8 != NULL)
			{
				callback(callbackdata, origdir, utf8);
				allocator.Free(utf8);
			} /* if */
		} while (pFindNextFileW(dir, &entw) != 0);
	} /* if */

	else  /* ANSI fallback. */
	{
		do
		{
			const DWORD attr = ent.dwFileAttributes;
			const DWORD tag = ent.dwReserved0;
			const char *fn = ent.cFileName;
			if ((fn[0] == '.') && (fn[1] == '\0'))
				continue;
			if ((fn[0] == '.') && (fn[1] == '.') && (fn[2] == '\0'))
				continue;
			if ((omitSymLinks) && (isSymlinkAttrs(attr, tag)))
				continue;

			utf8 = codepageToUtf8Heap(fn);
			if (utf8 != NULL)
			{
				callback(callbackdata, origdir, utf8);
				allocator.Free(utf8);
			} /* if */
		} while (FindNextFileA(dir, &ent) != 0);
	} /* else */

	FindClose(dir);
} /* __PHYSFS_platformEnumerateFiles */


char *__PHYSFS_platformCurrentDir(void)
{
	const wchar_t* path = Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data();
	wchar_t path2[1024];
	wcscpy_s(path2, path);
	wcscat_s(path2, L"\\");
	return unicodeToUtf8Heap(path2);
} /* __PHYSFS_platformCurrentDir */


  /* this could probably use a cleanup. */
char *__PHYSFS_platformRealPath(const char *path)
{
	/* !!! FIXME: this should return NULL if (path) doesn't exist? */
	/* !!! FIXME: Need to handle symlinks in Vista... */
	/* !!! FIXME: try GetFullPathName() instead? */
	/* this function should be UTF-8 clean. */
	char *retval = NULL;
	char *p = NULL;

	BAIL_IF_MACRO(path == NULL, ERR_INVALID_ARGUMENT, NULL);
	BAIL_IF_MACRO(*path == '\0', ERR_INVALID_ARGUMENT, NULL);

	retval = (char *)allocator.Malloc(MAX_PATH);
	BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);

	/*
	* If in \\server\path format, it's already an absolute path.
	*  We'll need to check for "." and ".." dirs, though, just in case.
	*/
	if ((path[0] == '\\') && (path[1] == '\\'))
		strcpy(retval, path);

	else
	{
		char *currentDir = __PHYSFS_platformCurrentDir();
		if (currentDir == NULL)
		{
			allocator.Free(retval);
			BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
		} /* if */

		if (path[1] == ':')   /* drive letter specified? */
		{
			/*
			* Apparently, "D:mypath" is the same as "D:\\mypath" if
			*  D: is not the current drive. However, if D: is the
			*  current drive, then "D:mypath" is a relative path. Ugh.
			*/
			if (path[2] == '\\')  /* maybe an absolute path? */
				strcpy(retval, path);
			else  /* definitely an absolute path. */
			{
				if (path[0] == currentDir[0]) /* current drive; relative. */
				{
					strcpy(retval, currentDir);
					strcat(retval, path + 2);
				} /* if */

				else  /* not current drive; absolute. */
				{
					retval[0] = path[0];
					retval[1] = ':';
					retval[2] = '\\';
					strcpy(retval + 3, path + 2);
				} /* else */
			} /* else */
		} /* if */

		else  /* no drive letter specified. */
		{
			if (path[0] == '\\')  /* absolute path. */
			{
				retval[0] = currentDir[0];
				retval[1] = ':';
				strcpy(retval + 2, path);
			} /* if */
			else
			{
				strcpy(retval, currentDir);
				strcat(retval, path);
			} /* else */
		} /* else */

		allocator.Free(currentDir);
	} /* else */

	  /* (whew.) Ok, now take out "." and ".." path entries... */

	p = retval;
	while ((p = strstr(p, "\\.")) != NULL)
	{
		/* it's a "." entry that doesn't end the string. */
		if (p[2] == '\\')
			memmove(p + 1, p + 3, strlen(p + 3) + 1);

		/* it's a "." entry that ends the string. */
		else if (p[2] == '\0')
			p[0] = '\0';

		/* it's a ".." entry. */
		else if (p[2] == '.')
		{
			char *prevEntry = p - 1;
			while ((prevEntry != retval) && (*prevEntry != '\\'))
				prevEntry--;

			if (prevEntry == retval)  /* make it look like a "." entry. */
				memmove(p + 1, p + 2, strlen(p + 2) + 1);
			else
			{
				if (p[3] != '\0') /* doesn't end string. */
					*prevEntry = '\0';
				else /* ends string. */
					memmove(prevEntry + 1, p + 4, strlen(p + 4) + 1);

				p = prevEntry;
			} /* else */
		} /* else if */

		else
		{
			p++;  /* look past current char. */
		} /* else */
	} /* while */

	  /* shrink the retval's memory block if possible... */
	p = (char *)allocator.Realloc(retval, strlen(retval) + 1);
	if (p != NULL)
		retval = p;

	return(retval);
} /* __PHYSFS_platformRealPath */


int __PHYSFS_platformMkDir(const char *path)
{
	WCHAR *wpath;
	DWORD rc;
	UTF8_TO_UNICODE_STACK_MACRO(wpath, path);
	rc = pCreateDirectoryW(wpath, NULL);
	__PHYSFS_smallFree(wpath);
	BAIL_IF_MACRO(rc == 0, winApiStrError(), 0);
	return(1);
} /* __PHYSFS_platformMkDir */


  /*
  * Get OS info and save the important parts.
  *
  * Returns non-zero if successful, otherwise it returns zero on failure.
  */
static int getOSInfo(void)
{
	osHasUnicode = 1;
	return(1);
} /* getOSInfo */


int __PHYSFS_platformInit(void)
{
	BAIL_IF_MACRO(!getOSInfo(), NULL, 0);
	BAIL_IF_MACRO(!findApiSymbols(), NULL, 0);
	BAIL_IF_MACRO(!determineUserDir(), NULL, 0);
	return(1);  /* It's all good */
} /* __PHYSFS_platformInit */


int __PHYSFS_platformDeinit(void)
{
	HANDLE *libs[] = { &libKernel32, &libUserEnv, &libAdvApi32, NULL };
	int i;

	allocator.Free(userDir);
	userDir = NULL;

	for (i = 0; libs[i] != NULL; i++)
	{
		const HANDLE lib = *(libs[i]);
		if (lib)
			FreeLibrary((HMODULE)lib);
		*(libs[i]) = NULL;
	} /* for */

	return(1); /* It's all good */
} /* __PHYSFS_platformDeinit */


static void *doOpen(const char *fname, DWORD mode, DWORD creation, int rdonly)
{
	HANDLE fileHandle;
	WinApiFile *retval;
	WCHAR *wfname;

	UTF8_TO_UNICODE_STACK_MACRO(wfname, fname);
	BAIL_IF_MACRO(wfname == NULL, ERR_OUT_OF_MEMORY, NULL);
	/*fileHandle = pCreateFileW(wfname, mode, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);*/
	fileHandle = CreateFile2(wfname, mode, FILE_SHARE_READ | FILE_SHARE_WRITE, creation, NULL);
	__PHYSFS_smallFree(wfname);

	BAIL_IF_MACRO
		(
			fileHandle == INVALID_HANDLE_VALUE,
			winApiStrError(), NULL
			);

	retval = (WinApiFile *)allocator.Malloc(sizeof(WinApiFile));
	if (retval == NULL)
	{
		CloseHandle(fileHandle);
		BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
	} /* if */

	retval->readonly = rdonly;
	retval->handle = fileHandle;
	return(retval);
} /* doOpen */


void *__PHYSFS_platformOpenRead(const char *filename)
{
	return(doOpen(filename, GENERIC_READ, OPEN_EXISTING, 1));
} /* __PHYSFS_platformOpenRead */


void *__PHYSFS_platformOpenWrite(const char *filename)
{
	return(doOpen(filename, GENERIC_WRITE, CREATE_ALWAYS, 0));
} /* __PHYSFS_platformOpenWrite */


void *__PHYSFS_platformOpenAppend(const char *filename)
{
	void *retval = doOpen(filename, GENERIC_WRITE, OPEN_ALWAYS, 0);
	if (retval != NULL)
	{
		HANDLE h = ((WinApiFile *)retval)->handle;
		//DWORD rc = SetFilePointer(h, 0, NULL, FILE_END);
		const LARGE_INTEGER zero = { 0 };
		DWORD rc = SetFilePointerEx(h, zero, NULL, FILE_END);
		if (rc == PHYSFS_INVALID_SET_FILE_POINTER)
		{
			const char *err = winApiStrError();
			CloseHandle(h);
			allocator.Free(retval);
			BAIL_MACRO(err, NULL);
		} /* if */
	} /* if */

	return retval;

} /* __PHYSFS_platformOpenAppend */


PHYSFS_sint64 __PHYSFS_platformRead(void *opaque, void *buffer,
	PHYSFS_uint32 size, PHYSFS_uint32 count)
{
	HANDLE Handle = ((WinApiFile *)opaque)->handle;
	DWORD CountOfBytesRead;
	PHYSFS_sint64 retval;

	/* Read data from the file */
	/* !!! FIXME: uint32 might be a greater # than DWORD */
	if (!ReadFile(Handle, buffer, count * size, &CountOfBytesRead, NULL))
	{
		BAIL_MACRO(winApiStrError(), -1);
	} /* if */
	else
	{
		/* Return the number of "objects" read. */
		/* !!! FIXME: What if not the right amount of bytes was read to make an object? */
		retval = CountOfBytesRead / size;
	} /* else */

	return(retval);
} /* __PHYSFS_platformRead */


PHYSFS_sint64 __PHYSFS_platformWrite(void *opaque, const void *buffer,
	PHYSFS_uint32 size, PHYSFS_uint32 count)
{
	HANDLE Handle = ((WinApiFile *)opaque)->handle;
	DWORD CountOfBytesWritten;
	PHYSFS_sint64 retval;

	/* Read data from the file */
	/* !!! FIXME: uint32 might be a greater # than DWORD */
	if (!WriteFile(Handle, buffer, count * size, &CountOfBytesWritten, NULL))
	{
		BAIL_MACRO(winApiStrError(), -1);
	} /* if */
	else
	{
		/* Return the number of "objects" read. */
		/* !!! FIXME: What if not the right number of bytes was written? */
		retval = CountOfBytesWritten / size;
	} /* else */

	return(retval);
} /* __PHYSFS_platformWrite */


int __PHYSFS_platformSeek(void *opaque, PHYSFS_uint64 pos)
{
	HANDLE Handle = ((WinApiFile *)opaque)->handle;
	BOOL rc;

	LARGE_INTEGER li;
	li.LowPart = LOWORDER_UINT64(pos);
	li.HighPart = HIGHORDER_UINT64(pos);

	rc = SetFilePointerEx(Handle, li, NULL, FILE_BEGIN);

	if (!rc && (GetLastError() != NO_ERROR))
	{
		return 0;
	} /* if */

	return 1;  /* No error occured */
} /* __PHYSFS_platformSeek */


PHYSFS_sint64 __PHYSFS_platformTell(void *opaque)
{
	HANDLE Handle = ((WinApiFile *)opaque)->handle;
	PHYSFS_sint64 retval;
	BOOL rc;

	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	LARGE_INTEGER out;

	rc = SetFilePointerEx(Handle, zero, &out, FILE_CURRENT);
	if (!rc)
	{
		return 0;
	} /* if */
	else
	{
		retval = out.QuadPart;
		assert(retval >= 0);
	} /* else */

	return retval;
} /* __PHYSFS_platformTell */


PHYSFS_sint64 __PHYSFS_platformFileLength(void *opaque)
{
	HANDLE Handle = ((WinApiFile *)opaque)->handle;
	PHYSFS_sint64 retval;

	FILE_STANDARD_INFO file_info = { 0 };
	const BOOL res = GetFileInformationByHandleEx(Handle, FileStandardInfo, &file_info, sizeof(file_info));
	if (res) {
		retval = file_info.EndOfFile.QuadPart;
		assert(retval >= 0);
	}

	return retval;
} /* __PHYSFS_platformFileLength */


int __PHYSFS_platformEOF(void *opaque)
{
	const PHYSFS_sint64 FileLength = __PHYSFS_platformFileLength(opaque);
	PHYSFS_sint64 FilePosition;
	int retval = 0;

	if (FileLength == 0)
		return 1;  /* we're definitely at EOF. */

				   /* Get the current position in the file */
	if ((FilePosition = __PHYSFS_platformTell(opaque)) != -1)
	{
		/* Non-zero if EOF is equal to the file length */
		retval = (FilePosition == FileLength);
	} /* if */

	return(retval);
} /* __PHYSFS_platformEOF */


int __PHYSFS_platformFlush(void *opaque)
{
	WinApiFile *fh = ((WinApiFile *)opaque);
	if (!fh->readonly)
		BAIL_IF_MACRO(!FlushFileBuffers(fh->handle), winApiStrError(), 0);

	return(1);
} /* __PHYSFS_platformFlush */


int __PHYSFS_platformClose(void *opaque)
{
	HANDLE Handle = ((WinApiFile *)opaque)->handle;
	BAIL_IF_MACRO(!CloseHandle(Handle), winApiStrError(), 0);
	allocator.Free(opaque);
	return(1);
} /* __PHYSFS_platformClose */


static int doPlatformDelete(LPWSTR wpath)
{
	/* If filename is a folder */
	int isdir = 0;
	//if (pGetFileAttributesW(wpath) & FILE_ATTRIBUTE_DIRECTORY)
	WIN32_FILE_ATTRIBUTE_DATA file_info;
	const BOOL res = GetFileAttributesExW(wpath, GetFileExInfoStandard, &file_info);
	if (res) {
		isdir = (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	if (isdir)
	{
		BAIL_IF_MACRO(!pRemoveDirectoryW(wpath), winApiStrError(), 0);
	} /* if */
	else
	{
		BAIL_IF_MACRO(!pDeleteFileW(wpath), winApiStrError(), 0);
	} /* else */

	return(1);   /* if you made it here, it worked. */
} /* doPlatformDelete */


int __PHYSFS_platformDelete(const char *path)
{
	int retval = 0;
	LPWSTR wpath;
	UTF8_TO_UNICODE_STACK_MACRO(wpath, path);
	BAIL_IF_MACRO(wpath == NULL, ERR_OUT_OF_MEMORY, 0);
	retval = doPlatformDelete(wpath);
	__PHYSFS_smallFree(wpath);
	return(retval);
} /* __PHYSFS_platformDelete */


  /*
  * !!! FIXME: why aren't we using Critical Sections instead of Mutexes?
  * !!! FIXME:  mutexes on Windows are for cross-process sync. CritSects are
  * !!! FIXME:  mutexes for threads in a single process and are faster.
  */
void *__PHYSFS_platformCreateMutex(void)
{
	return((void *)CreateMutex(NULL, FALSE, NULL));
} /* __PHYSFS_platformCreateMutex */


void __PHYSFS_platformDestroyMutex(void *mutex)
{
	CloseHandle((HANDLE)mutex);
} /* __PHYSFS_platformDestroyMutex */


int __PHYSFS_platformGrabMutex(void *mutex)
{
	return(WaitForSingleObject((HANDLE)mutex, INFINITE) != WAIT_FAILED);
} /* __PHYSFS_platformGrabMutex */


void __PHYSFS_platformReleaseMutex(void *mutex)
{
	ReleaseMutex((HANDLE)mutex);
} /* __PHYSFS_platformReleaseMutex */


static PHYSFS_sint64 FileTimeToPhysfsTime(const FILETIME *ft)
{
	SYSTEMTIME st_utc;
	SYSTEMTIME st_localtz;
	TIME_ZONE_INFORMATION tzi;
	DWORD tzid;
	PHYSFS_sint64 retval;
	struct tm tm;

	BAIL_IF_MACRO(!FileTimeToSystemTime(ft, &st_utc), winApiStrError(), -1);
	tzid = GetTimeZoneInformation(&tzi);
	BAIL_IF_MACRO(tzid == TIME_ZONE_ID_INVALID, winApiStrError(), -1);

	/* (This API is unsupported and fails on non-NT systems. */
	if (!SystemTimeToTzSpecificLocalTime(&tzi, &st_utc, &st_localtz))
	{
		/* do it by hand. Grumble... */
		ULARGE_INTEGER ui64;
		FILETIME new_ft;
		ui64.LowPart = ft->dwLowDateTime;
		ui64.HighPart = ft->dwHighDateTime;

		if (tzid == TIME_ZONE_ID_STANDARD)
			tzi.Bias += tzi.StandardBias;
		else if (tzid == TIME_ZONE_ID_DAYLIGHT)
			tzi.Bias += tzi.DaylightBias;

		/* convert from minutes to 100-nanosecond increments... */
		ui64.QuadPart -= (((LONGLONG)tzi.Bias) * (600000000));

		/* Move it back into a FILETIME structure... */
		new_ft.dwLowDateTime = ui64.LowPart;
		new_ft.dwHighDateTime = ui64.HighPart;

		/* Convert to something human-readable... */
		if (!FileTimeToSystemTime(&new_ft, &st_localtz))
			BAIL_MACRO(winApiStrError(), -1);
	} /* if */

	  /* Convert to a format that mktime() can grok... */
	tm.tm_sec = st_localtz.wSecond;
	tm.tm_min = st_localtz.wMinute;
	tm.tm_hour = st_localtz.wHour;
	tm.tm_mday = st_localtz.wDay;
	tm.tm_mon = st_localtz.wMonth - 1;
	tm.tm_year = st_localtz.wYear - 1900;
	tm.tm_wday = -1 /*st_localtz.wDayOfWeek*/;
	tm.tm_yday = -1;
	tm.tm_isdst = -1;

	/* Convert to a format PhysicsFS can grok... */
	retval = (PHYSFS_sint64)mktime(&tm);
	BAIL_IF_MACRO(retval == -1, strerror(errno), -1);
	return(retval);
} /* FileTimeToPhysfsTime */


PHYSFS_sint64 __PHYSFS_platformGetLastModTime(const char *fname)
{
	PHYSFS_sint64 retval = -1;
	WIN32_FILE_ATTRIBUTE_DATA attr;
	int rc = 0;

	memset(&attr, '\0', sizeof(attr));

	/* GetFileAttributesEx didn't show up until Win98 and NT4. */
	if ((pGetFileAttributesExW != NULL) || (pGetFileAttributesExA != NULL))
	{
		WCHAR *wstr;
		UTF8_TO_UNICODE_STACK_MACRO(wstr, fname);
		if (wstr != NULL) /* if NULL, maybe the fallback will work. */
		{
			if (pGetFileAttributesExW != NULL)  /* NT/XP/Vista/etc system. */
				rc = pGetFileAttributesExW(wstr, GetFileExInfoStandard, &attr);
			else  /* Win98/ME system */
			{
				const int len = (int)(wStrLen(wstr) + 1);
				char *cp = (char *)__PHYSFS_smallAlloc(len);
				if (cp != NULL)
				{
					WideCharToMultiByte(CP_ACP, 0, wstr, len, cp, len, 0, 0);
					rc = pGetFileAttributesExA(cp, GetFileExInfoStandard, &attr);
					__PHYSFS_smallFree(cp);
				} /* if */
			} /* else */
			__PHYSFS_smallFree(wstr);
		} /* if */
	} /* if */

	if (rc)  /* had API entry point and it worked. */
	{
		/* 0 return value indicates an error or not supported */
		if ((attr.ftLastWriteTime.dwHighDateTime != 0) ||
			(attr.ftLastWriteTime.dwLowDateTime != 0))
		{
			retval = FileTimeToPhysfsTime(&attr.ftLastWriteTime);
		} /* if */
	} /* if */

	  /* GetFileTime() has been in the Win32 API since the start. */
	if (retval == -1)  /* try a fallback... */
	{
		FILETIME ft;
		BOOL rc;
		const char *err;
		WinApiFile *f = (WinApiFile *)__PHYSFS_platformOpenRead(fname);
		BAIL_IF_MACRO(f == NULL, NULL, -1)
			rc = GetFileTime(f->handle, NULL, NULL, &ft);
		err = winApiStrError();
		CloseHandle(f->handle);
		allocator.Free(f);
		BAIL_IF_MACRO(!rc, err, -1);
		retval = FileTimeToPhysfsTime(&ft);
	} /* if */

	return(retval);
} /* __PHYSFS_platformGetLastModTime */


  /* !!! FIXME: Don't use C runtime for allocators? */
int __PHYSFS_platformSetDefaultAllocator(PHYSFS_Allocator *a)
{
	return(0);  /* just use malloc() and friends. */
} /* __PHYSFS_platformSetDefaultAllocator */

#endif /* PHYSFS_PLATFORM_WINRT */

  /* end of windows.c ... */


