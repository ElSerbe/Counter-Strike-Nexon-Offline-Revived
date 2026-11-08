// nartool: reads files out of the game's .nar archives through the game's own FileSystem_Nar.dll
// (VFileSystem009, the GoldSrc file system interface), so no decryption code is needed here.
//
// Build (x86, VS Build Tools):  cl /nologo /O2 /EHsc nartool.cpp
// Usage:
//   nartool <Bin dir> <game root> <output dir> <file> [file...]
//   nartool <Bin dir> <game root> <output dir> @listfile        (one relative path per line)
//   nartool <Bin dir> <game root> --serve                       (stays open, requests on stdin: see Serve)
// Build with its version information (antivirus programs trust a described program more than a bare one):
//   rc /nologo nartool.rc  &&  cl /nologo /O2 /EHsc nartool.cpp nartool.res
// Files are game-relative paths such as resource/res/popup_userresearch.res.
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <fstream>

typedef void* FileHandle_t;

// GoldSrc public/FileSystem.h, VFileSystem009 (only the part we call; order matters)
class IBaseInterface
{
public:
	virtual ~IBaseInterface() {}
};

class IFileSystem : public IBaseInterface
{
public:
	virtual void Mount() = 0;
	virtual void Unmount() = 0;
	virtual void RemoveAllSearchPaths() = 0;
	virtual void AddSearchPath(const char* pPath, const char* pathID) = 0;
	virtual bool RemoveSearchPath(const char* pPath) = 0;
	virtual void RemoveFile(const char* pRelativePath, const char* pathID) = 0;
	virtual void CreateDirHierarchy(const char* path, const char* pathID) = 0;
	virtual bool FileExists(const char* pFileName) = 0;
	virtual bool IsDirectory(const char* pFileName) = 0;
	virtual FileHandle_t Open(const char* pFileName, const char* pOptions, const char* pathID) = 0;
	virtual void Close(FileHandle_t file) = 0;
	virtual void Seek(FileHandle_t file, int pos, int seekType) = 0;
	virtual unsigned int Tell(FileHandle_t file) = 0;
	virtual unsigned int Size(FileHandle_t file) = 0;
	virtual unsigned int SizeByName(const char* pFileName) = 0;
	virtual long GetFileTime(const char* pFileName) = 0;
	virtual void FileTimeToString(char* pStrip, int maxChars, long fileTime) = 0;
	virtual bool IsOk(FileHandle_t file) = 0;
	virtual void Flush(FileHandle_t file) = 0;
	virtual bool EndOfFile(FileHandle_t file) = 0;
	virtual int Read(void* pOutput, int size, FileHandle_t file) = 0;
};

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);

static bool g_crashed = false;

template <class F> static bool Guard(const char* what, F f)
{
	__try
	{
		f();
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		fprintf(stderr, "[crash] %s (0x%08lX)\n", what, GetExceptionCode());
		g_crashed = true;
		return false;
	}
}

static bool g_quiet = false;         // --serve: no line per file, only the batch's answer
static UINT g_pathCodePage = CP_ACP; // the output folders' encoding: the command line's, or UTF-8 from stdin

static std::wstring Widen(const std::string& s, UINT cp)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), NULL, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

// the folders above a file, with wide paths (a Windows user name with accents is in %TEMP%)
static void MakeDirs(const std::wstring& path)
{
	for (size_t i = 0; i < path.size(); i++)
		if (path[i] == L'/' || path[i] == L'\\')
			CreateDirectoryW(path.substr(0, i).c_str(), NULL);
}

// FileSystem_Nar's vtable is GoldSrc's plus a few CSO methods: these offsets are the ones hw.dll uses
// (Open +0x30, Close +0x34, Size +0x44, Read +0x5c).
typedef FileHandle_t(__thiscall* OpenFn)(void*, const char*, const char*, const char*);
typedef void(__thiscall* CloseFn)(void*, FileHandle_t);
typedef unsigned int(__thiscall* SizeFn)(void*, FileHandle_t);
typedef int(__thiscall* ReadFn)(void*, void*, int, FileHandle_t);

static bool Extract(IFileSystem* fs, const std::string& name, const std::string& outDir)
{
	void** vt = *(void***)fs;
	FileHandle_t h = NULL;
	Guard("Open", [&] { h = ((OpenFn)vt[0x30 / 4])(fs, name.c_str(), "rb", "GAME"); });
	if (!h)
	{
		if (!g_quiet)
			printf("[missing] %s\n", name.c_str());
		return false;
	}
	unsigned int size = 0;
	Guard("Size", [&] { size = ((SizeFn)vt[0x44 / 4])(fs, h); });
	std::vector<char> buf(size + 1);
	int got = 0;
	if (size)
		Guard("Read", [&] { got = ((ReadFn)vt[0x5c / 4])(fs, buf.data(), (int)size, h); });
	Guard("Close", [&] { ((CloseFn)vt[0x34 / 4])(fs, h); });

	std::wstring out = Widen(outDir, g_pathCodePage) + L"/" + Widen(name, g_pathCodePage);
	MakeDirs(out);
	FILE* f = _wfopen(out.c_str(), L"wb");
	if (!f)
	{
		fprintf(stderr, "can't write %s\n", name.c_str());
		return false;
	}
	if (got > 0)
		fwrite(buf.data(), 1, got, f);
	fclose(f);
	if (!g_quiet)
		printf("[ok] %s (%d bytes)\n", name.c_str(), got);
	return got == (int)size;
}

// --serve: the archives are opened once and the export scripts ask for files on stdin, so the program starts once
// per export instead of once per group of files (the antivirus checks every start of a program it doesn't know).
// Request (UTF-8 lines):  get<TAB><output dir><TAB><count>, then <count> lines with one game path each
// Answer:                 done<TAB><extracted><TAB><count>
// "quit" or the end of stdin closes it.
static int Serve(IFileSystem* fs)
{
	g_quiet = true;
	g_pathCodePage = CP_UTF8;
	static char buf[8192];
	auto readLine = [&](std::string& out) -> bool
	{
		if (!fgets(buf, sizeof(buf), stdin))
			return false;
		out = buf;
		while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
			out.pop_back();
		return true;
	};
	std::string line;
	while (readLine(line))
	{
		if (line == "quit")
			break;
		size_t tab = line.compare(0, 4, "get\t") == 0 ? line.find('\t', 4) : std::string::npos;
		if (tab == std::string::npos)
		{
			printf("error\tunknown request\n");
			fflush(stdout);
			continue;
		}
		std::string outDir = line.substr(4, tab - 4);
		int count = atoi(line.c_str() + tab + 1), ok = 0;
		for (int i = 0; i < count; i++)
		{
			std::string name;
			if (!readLine(name))
				return 0;
			if (!name.empty())
				ok += Extract(fs, name, outDir);
		}
		printf("done\t%d\t%d\n", ok, count);
		fflush(stdout);
	}
	return 0;
}

int main(int argc, char** argv)
{
	bool serve = argc == 4 && std::string(argv[3]) == "--serve";
	if (argc < 5 && !serve)
	{
		fprintf(stderr, "usage: nartool <Bin dir> <game root> <output dir> <file|@list>...\n"
		                "       nartool <Bin dir> <game root> --serve\n");
		return 2;
	}
	std::string bin = argv[1], root = argv[2], outDir = argv[3];

	SetDllDirectoryA(bin.c_str());
	SetCurrentDirectoryA(bin.c_str());
	HMODULE dll = LoadLibraryA((bin + "\\FileSystem_Nar.dll").c_str());
	if (!dll)
	{
		fprintf(stderr, "can't load FileSystem_Nar.dll (%lu)\n", GetLastError());
		return 1;
	}
	CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(dll, "CreateInterface");
	IFileSystem* fs = createInterface ? (IFileSystem*)createInterface("VFileSystem009", NULL) : NULL;
	if (!fs)
	{
		fprintf(stderr, "no VFileSystem009 interface\n");
		return 1;
	}

	Guard("Mount", [&] { fs->Mount(); });

	// CSO-specific methods after the GoldSrc ones, in the order hw.dll calls them when it sets up the file system
	typedef void(__thiscall* Str1Fn)(void*, const char*);
	typedef void(__thiscall* Str2Fn)(void*, const char*, const char*);
	void** vt = *(void***)fs;
	std::string dataDir = root + "/Data";
	Guard("SetDataDir (+0xe8)", [&] { ((Str1Fn)vt[0xe8 / 4])(fs, dataDir.c_str()); });
	Guard("Packer (+0xe4)", [&] { ((Str1Fn)vt[0xe4 / 4])(fs, "Packer"); });

	const char* order = getenv("NAR_ORDER") ? getenv("NAR_ORDER") : "nar-dir";
	// NAR_ONLY=fstrike mounts just that archive, to read one version of a file that several archives contain (any
	// archive of Data, e.g. cstrike_na_es for the Spanish texts)
	const char* only = getenv("NAR_ONLY");
	std::vector<const char*> games = { "cstrike_na_en", "cstrike_na", "cstrike", "dstrike", "estrike", "fstrike", "valve" };
	if (only)
		games = { only };
	for (const char* g : games)
	{
		std::string nar = root + "/Data/" + g + ".nar";
		std::string dir = std::string("/") + g + "/";
		if (std::string(order) == "dir-nar")
			Guard("AddNar (+0xe0)", [&] { ((Str2Fn)vt[0xe0 / 4])(fs, dir.c_str(), nar.c_str()); });
		else
			Guard("AddNar (+0xe0)", [&] { ((Str2Fn)vt[0xe0 / 4])(fs, nar.c_str(), dir.c_str()); });
	}
	// Data/Packer/*.pak (newer content): hw.dll registers them as virtual folders with +0xf4
	// ("/lstrike/common/", "/lstrike/locale_na/", ...) and searches them like game folders.
	const char* packs[] = { "/lstrike/common/", "/lstrike/locale/", "/lstrike/locale_na/", "/lstrike/locale_na_en/" };
	for (const char* p : packs)
		if (!only)
			Guard("AddPack (+0xf4)", [&] { ((Str1Fn)vt[0xf4 / 4])(fs, p); });

	for (const char* g : games)
	{
		std::string dir = root + "/" + g;
		Guard("AddSearchPath", [&] { fs->AddSearchPath(dir.c_str(), "GAME"); });
	}
	const char* packDirs[] = { "lstrike/common", "lstrike/locale_na_en", "lstrike/locale_na", "lstrike/locale" };
	for (const char* p : packDirs)
	{
		std::string dir = root + "/" + p;
		Guard("AddSearchPath(pack)", [&] { fs->AddSearchPath(dir.c_str(), "GAME"); });
	}

	if (serve)
		return Serve(fs);

	// nartool <Bin> <root> <out> --list <wildcard>...: prints matching files (archives and packs included)
	if (std::string(argv[4]) == "--list")
	{
		typedef const char*(__thiscall* FindFirstFn)(void*, const char*, int*, const char*);
		typedef const char*(__thiscall* FindNextFn)(void*, int);
		typedef void(__thiscall* FindCloseFn)(void*, int);
		for (int i = 5; i < argc; i++)
		{
			int handle = -1;
			const char* name = NULL;
			Guard("FindFirst", [&] { name = ((FindFirstFn)vt[0x74 / 4])(fs, argv[i], &handle, getenv("NAR_PATHID")); });
			while (name)
			{
				printf("%s\n", name);
				Guard("FindNext", [&] { name = ((FindNextFn)vt[0x78 / 4])(fs, handle); });
			}
			Guard("FindClose", [&] { ((FindCloseFn)vt[0x80 / 4])(fs, handle); });
		}
		return 0;
	}

	// nartool <Bin> <root> <out> --memscan <probe file> <needle>...: opens the probe file (so the pack indexes get
	// loaded), then prints every string in this process's memory containing a needle (case-insensitive).
	// The packs have no readable file list, but FileSystem_Nar keeps their names in memory once mounted.
	if (std::string(argv[4]) == "--memscan" && argc > 6)
	{
		void* h = NULL;
		Guard("Open", [&] { h = ((OpenFn)vt[0x30 / 4])(fs, argv[5], "rb", "GAME"); });
		if (h)
			Guard("Close", [&] { ((CloseFn)vt[0x34 / 4])(fs, h); });
		std::vector<std::string> seen;
		MEMORY_BASIC_INFORMATION mbi;
		for (char* p = NULL; VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi); p = (char*)mbi.BaseAddress + mbi.RegionSize)
		{
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
				!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
				continue;
			const char* base = (const char*)mbi.BaseAddress;
			size_t size = mbi.RegionSize;
			for (int n = 6; n < argc; n++)
			{
				size_t len = strlen(argv[n]);
				for (size_t i = 0; i + len <= size; i++)
				{
					if (_strnicmp(base + i, argv[n], len) != 0)
						continue;
					size_t s = i, e = i + len;
					while (s > 0 && base[s - 1] >= 32 && base[s - 1] < 127 && i - s < 200)
						s--;
					while (e < size && base[e] >= 32 && base[e] < 127 && e - i < 200)
						e++;
					std::string str(base + s, e - s);
					bool dup = false;
					for (auto& x : seen)
						dup |= x == str;
					if (!dup && str != argv[n])
					{
						seen.push_back(str);
						printf("%s\n", str.c_str());
					}
					i = e;
				}
			}
		}
		return 0;
	}

	std::vector<std::string> files;
	for (int i = 4; i < argc; i++)
	{
		if (argv[i][0] == '@')
		{
			std::ifstream list(argv[i] + 1);
			std::string line;
			while (std::getline(list, line))
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
					line.pop_back();
				if (!line.empty())
					files.push_back(line);
			}
		}
		else
		{
			files.push_back(argv[i]);
		}
	}

	int ok = 0;
	for (auto& f : files)
		ok += Extract(fs, f, outDir);
	printf("%d / %d extracted%s\n", ok, (int)files.size(), g_crashed ? " (some calls crashed)" : "");
	return 0;
}
