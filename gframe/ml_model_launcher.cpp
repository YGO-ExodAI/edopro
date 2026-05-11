#include "ml_model_launcher.h"
#include "config.h"

#if EDOPRO_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include "fmt.h"
#include "game.h"
#include "bufferio.h"
#endif

namespace ygo {

#if EDOPRO_WINDOWS

namespace {

constexpr const wchar_t* DEFAULT_SRC_DIR = L"C:\\Users\\Joe\\Documents\\ExodAI\\src";
constexpr const wchar_t* DEFAULT_WINDBOT_DIR = L"C:\\Users\\Joe\\Documents\\ExodAI\\src\\deploy\\windbot-bin";
constexpr const wchar_t* DEFAULT_PYTHON = L"python";
constexpr const wchar_t* SERVE_MODEL_MUTEX = L"Global\\ExodAI_serve_model_singleton";

// Process group bookkeeping. The Job Object kills its members when its
// last handle is closed — so EDOPro exit/crash auto-cleans the children.
// ShutdownMLModelBot() additionally calls TerminateJobObject for explicit
// teardown on surrender / leave-game.
HANDLE g_job = nullptr;
HANDLE g_serveModelMutex = nullptr;
std::mutex g_lifecycleMtx;

HANDLE EnsureJob() {
	if(g_job)
		return g_job;
	g_job = CreateJobObjectW(nullptr, nullptr);
	if(!g_job)
		return nullptr;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
	info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
	return g_job;
}

std::wstring GetEnvOrDefault(const wchar_t* var, const wchar_t* def) {
	wchar_t buf[2048];
	DWORD n = GetEnvironmentVariableW(var, buf, 2048);
	if(n > 0 && n < 2048)
		return std::wstring(buf, n);
	return def;
}

// Show a Win32 file-open dialog for picking a .pt checkpoint.
// Returns the chosen path, or empty string if the user cancelled.
std::wstring PickModelFile(const std::wstring& initialDir) {
	wchar_t fileBuf[MAX_PATH] = {};
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = nullptr;
	ofn.lpstrFilter = L"PyTorch checkpoint (*.pt)\0*.pt\0All files (*.*)\0*.*\0\0";
	ofn.lpstrFile = fileBuf;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
	ofn.lpstrTitle = L"Select ExodAI model checkpoint";
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if(GetOpenFileNameW(&ofn))
		return fileBuf;
	return {};
}

void PostLogLine(const std::wstring& line) {
	if(!mainGame) return;
	std::lock_guard<epro::mutex> lock(mainGame->gMutex);
	mainGame->AddLog(line, 0);
}

void PipeReaderLoop(HANDLE readEnd, std::wstring prefix) {
	char buf[4096];
	std::string acc;
	DWORD got = 0;
	while(ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
		acc.append(buf, got);
		size_t pos;
		while((pos = acc.find('\n')) != std::string::npos) {
			std::string line = acc.substr(0, pos);
			acc.erase(0, pos + 1);
			if(!line.empty() && line.back() == '\r')
				line.pop_back();
			if(line.empty())
				continue;
			PostLogLine(prefix + BufferIO::DecodeUTF8(line));
		}
	}
	if(!acc.empty())
		PostLogLine(prefix + BufferIO::DecodeUTF8(acc));
	CloseHandle(readEnd);
}

bool SpawnWithCapture(const wchar_t* exePath, std::wstring cmdLine, const wchar_t* workingDir,
                      std::wstring logPrefix, DWORD& errOut) {
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE rd = nullptr, wr = nullptr;
	if(!CreatePipe(&rd, &wr, &sa, 0)) {
		errOut = GetLastError();
		return false;
	}
	// Read end stays in the parent only.
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{ sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = nullptr;
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(
		exePath,
		cmdLine.empty() ? nullptr : &cmdLine[0],
		nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED,
		nullptr,
		workingDir,
		&si, &pi);
	// The child holds its own copy of the write end; parent must close.
	CloseHandle(wr);
	if(!ok) {
		errOut = GetLastError();
		CloseHandle(rd);
		return false;
	}
	HANDLE job = EnsureJob();
	if(job)
		AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	std::thread(PipeReaderLoop, rd, std::move(logPrefix)).detach();
	return true;
}

// Returns true if a serve_model.py instance was spawned by *this* call,
// false if one already appeared to be running (or if spawn failed — see err).
// Holds the singleton mutex in g_serveModelMutex so ShutdownMLModelBot can
// release it for a fresh launch.
bool MaybeSpawnServeModel(const std::wstring& srcDir, const std::wstring& modelPath, std::wstring& errMsg) {
	if(g_serveModelMutex)
		return false; // we already own the slot
	HANDLE mtx = CreateMutexW(nullptr, TRUE, SERVE_MODEL_MUTEX);
	if(mtx == nullptr) {
		errMsg = L"Failed to create serve_model singleton mutex";
		return false;
	}
	if(GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(mtx);
		return false; // a separate EDOPro/process already owns it
	}

	// Prefer the project's venv python (has fastapi/torch/etc.) over
	// whatever bare `python` is on EDOPro's PATH. Override with EXODAI_PYTHON
	// if you want a different interpreter.
	std::wstring py = GetEnvOrDefault(L"EXODAI_PYTHON", L"");
	if(py.empty()) {
		std::filesystem::path venvPy = std::filesystem::path(srcDir).parent_path() / L".venv" / L"Scripts" / L"python.exe";
		std::error_code ec;
		py = std::filesystem::exists(venvPy, ec) ? venvPy.wstring() : std::wstring{ DEFAULT_PYTHON };
	}
	// `python -u` disables Python's own stdio buffering — without it,
	// serve_model.py's startup logs sit in the kernel pipe buffer until
	// it's full or the process exits, so we can't see model-load progress
	// or crashes in real time.
	std::wstring cmd;
	if(modelPath.empty())
		cmd = epro::format(L"\"{}\" -u \"{}\\serve_model.py\"", py, srcDir);
	else
		cmd = epro::format(L"\"{}\" -u \"{}\\serve_model.py\" --checkpoint \"{}\"", py, srcDir, modelPath);
	DWORD err = 0;
	if(!SpawnWithCapture(nullptr, cmd, srcDir.c_str(), L"[serve] ", err)) {
		errMsg = epro::format(L"Failed to start serve_model.py (CreateProcess error {})", err);
		CloseHandle(mtx);
		return false;
	}
	g_serveModelMutex = mtx;
	return true;
}

} // namespace

MLModelLaunchResult LaunchMLModelBot(int port, const std::wstring& pass) {
	std::lock_guard<std::mutex> lock(g_lifecycleMtx);
	MLModelLaunchResult res{};
	auto srcDir = GetEnvOrDefault(L"EXODAI_SRC_DIR", DEFAULT_SRC_DIR);
	auto windbotDir = GetEnvOrDefault(L"EXODAI_WINDBOT_DIR", DEFAULT_WINDBOT_DIR);

	// Show file-picker so Joe can choose which checkpoint to load.
	// EXODAI_MODELS_DIR overrides the default initial directory.
	auto modelsDir = srcDir + L"\\models";
	auto initialDir = GetEnvOrDefault(L"EXODAI_MODELS_DIR", modelsDir.c_str());
	auto modelPath = PickModelFile(initialDir);
	if(modelPath.empty()) {
		res.cancelled = true;
		return res;
	}
	res.modelName = std::filesystem::path(modelPath).stem().wstring();

	std::wstring serveErr;
	MaybeSpawnServeModel(srcDir, modelPath, serveErr);
	if(!serveErr.empty()) {
		res.errorMessage = serveErr;
		return res;
	}

	auto windbotExe = windbotDir + L"\\WindBot.exe";
	auto botName = epro::format(L"ExodAI-{}", res.modelName);
	auto windbotArgs = epro::format(
		L"WindBot.exe Host=127.0.0.1 HostInfo=\"{}\" Port={} Version={} Deck=ExodAI Name=\"{}\" Chat=true Hand=1",
		pass, port, static_cast<uint32_t>(CLIENT_VERSION), botName);
	DWORD err = 0;
	if(!SpawnWithCapture(windbotExe.c_str(), windbotArgs, windbotDir.c_str(), L"[WindBot] ", err)) {
		res.errorMessage = epro::format(
			L"Failed to launch WindBot.exe at {} (CreateProcess error {})", windbotExe, err);
		return res;
	}

	res.ok = true;
	return res;
}

void ShutdownMLModelBot() {
	std::lock_guard<std::mutex> lock(g_lifecycleMtx);
	if(g_job) {
		TerminateJobObject(g_job, 0);
		CloseHandle(g_job);
		g_job = nullptr;
	}
	if(g_serveModelMutex) {
		CloseHandle(g_serveModelMutex);
		g_serveModelMutex = nullptr;
	}
}

#else // !EDOPRO_WINDOWS

MLModelLaunchResult LaunchMLModelBot(int /*port*/, const std::wstring& /*pass*/) {
	MLModelLaunchResult res{};
	res.errorMessage = L"ML Model launcher is only supported on Windows";
	return res;
}

void ShutdownMLModelBot() {}

#endif

} // namespace ygo
