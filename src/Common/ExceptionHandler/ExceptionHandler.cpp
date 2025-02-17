#include "Common/precompiled.h"
#include "Cafe/CafeSystem.h"

#if BOOST_OS_LINUX
#include <signal.h>
#include <execinfo.h>
#endif

#if BOOST_OS_WINDOWS
#include <Windows.h>
#include <Dbghelp.h>
#include <shellapi.h>

#include "Config/ActiveSettings.h"
#include "Config/CemuConfig.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/PPCState.h"

#endif

extern uint32 currentBaseApplicationHash;
extern uint32 currentUpdatedApplicationHash;

#if BOOST_OS_WINDOWS

LONG handleException_SINGLE_STEP(PEXCEPTION_POINTERS pExceptionInfo)
{

	return EXCEPTION_CONTINUE_SEARCH;
}

void crashlog_writeHeader(const char* header)
{
	cemuLog_writePlainToLog("-----------------------------------------\n");

	cemuLog_writePlainToLog("   ");
	cemuLog_writePlainToLog(header);
	cemuLog_writePlainToLog("\n");

	cemuLog_writePlainToLog("-----------------------------------------\n");
}

bool crashLogCreated = false;
bool IsCemuhookLoaded();
#include <boost/algorithm/string.hpp>
BOOL CALLBACK MyMiniDumpCallback(PVOID pParam, const PMINIDUMP_CALLBACK_INPUT pInput, PMINIDUMP_CALLBACK_OUTPUT pOutput)
{
	if (!pInput || !pOutput)
		return FALSE;

	switch (pInput->CallbackType)
	{
	case IncludeModuleCallback:
	case IncludeThreadCallback:
	case ThreadCallback:
	case ThreadExCallback:
		return TRUE;

	case ModuleCallback:

		if (!(pOutput->ModuleWriteFlags & ModuleReferencedByMemory))
			pOutput->ModuleWriteFlags &= ~ModuleWriteModule;

		return TRUE;
	}

	return FALSE;

}

bool CreateMiniDump(CrashDump dump, EXCEPTION_POINTERS* pep)
{
	if (dump == CrashDump::Disabled)
		return true;

	fs::path p = ActiveSettings::GetPath("crashdump");

	std::error_code ec;
	fs::create_directories(p, ec);
	if (ec)
		return false;

	const auto now = std::chrono::system_clock::now();
	const auto temp_time = std::chrono::system_clock::to_time_t(now);
	const auto& time = *std::gmtime(&temp_time);

	p /= fmt::format("crash_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}.dmp", 1900 + time.tm_year, time.tm_mon + 1, time.tm_mday, time.tm_year, time.tm_hour, time.tm_min, time.tm_sec);

	const auto hFile = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	MINIDUMP_EXCEPTION_INFORMATION mdei;
	mdei.ThreadId = GetCurrentThreadId();
	mdei.ExceptionPointers = pep;
	mdei.ClientPointers = FALSE;

	MINIDUMP_CALLBACK_INFORMATION mci;
	mci.CallbackRoutine = (MINIDUMP_CALLBACK_ROUTINE)MyMiniDumpCallback;
	mci.CallbackParam = nullptr;

	MINIDUMP_TYPE mdt;
	if (dump == CrashDump::Full)
	{
		mdt = (MINIDUMP_TYPE)(MiniDumpWithPrivateReadWriteMemory |
			MiniDumpWithDataSegs |
			MiniDumpWithHandleData |
			MiniDumpWithFullMemoryInfo |
			MiniDumpWithThreadInfo |
			MiniDumpWithUnloadedModules);
	}
	else
	{
		mdt = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
	}

	const auto result = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, mdt, &mdei, nullptr, &mci);
	CloseHandle(hFile);
	return result != FALSE;
}

void DebugLogStackTrace(OSThread_t* thread, MPTR sp);

void DumpThreadStackTrace()
{
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, NULL, TRUE);

	char dumpLine[1024 * 4];
	void* stack[100];

	const unsigned short frames = CaptureStackBackTrace(0, 40, stack, NULL);
	SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
	symbol->MaxNameLen = 255;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

	crashlog_writeHeader("Stack trace");
	for (unsigned int i = 0; i < frames; i++)
	{
		DWORD64 stackTraceOffset = (DWORD64)stack[i];
		SymFromAddr(process, stackTraceOffset, 0, symbol);
		sprintf(dumpLine, "0x%016I64x ", (uint64)(size_t)stack[i]);
		cemuLog_writePlainToLog(dumpLine);
		// module name
		HMODULE stackModule;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)stackTraceOffset, &stackModule))
		{
			char moduleName[512];
			moduleName[0] = '\0';
			GetModuleFileNameA(stackModule, moduleName, 512);
			sint32 moduleNameStartIndex = std::max((sint32)0, (sint32)strlen(moduleName) - 1);
			while (moduleNameStartIndex > 0)
			{
				if (moduleName[moduleNameStartIndex] == '\\' || moduleName[moduleNameStartIndex] == '/')
				{
					moduleNameStartIndex++;
					break;
				}
				moduleNameStartIndex--;
			}

			DWORD64 moduleAddress = (DWORD64)GetModuleHandleA(moduleName);
			sint32 relativeOffset = 0;
			if (moduleAddress != 0)
				relativeOffset = stackTraceOffset - moduleAddress;

			sprintf(dumpLine, "+0x%08x %-16s", relativeOffset, moduleName + moduleNameStartIndex);
			cemuLog_writePlainToLog(dumpLine);
		}
		else
		{
			sprintf(dumpLine, "+0x00000000 %-16s", "NULL");
			cemuLog_writePlainToLog(dumpLine);
		}
		// function name
		sprintf(dumpLine, " %s\n", symbol->Name);
		cemuLog_writePlainToLog(dumpLine);
	}

	free(symbol);
}

void createCrashlog(EXCEPTION_POINTERS* e, PCONTEXT context)
{
	if (crashLogCreated)
		return; // only create one crashlog
	crashLogCreated = true;

	cemuLog_createLogFile(true);

	const auto crash_dump = GetConfig().crash_dump.GetValue();
	const auto dump_written = CreateMiniDump(crash_dump, e);
	if (!dump_written)
		cemuLog_writeLineToLog(fmt::format("couldn't write minidump {:#x}", GetLastError()), false, true);

	char dumpLine[1024 * 4];

	// info about Cemu version
	sprintf(dumpLine, "\nCrashlog for Cemu %d.%d%s\n", EMULATOR_VERSION_LEAD, EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_SUFFIX);
	cemuLog_writePlainToLog(dumpLine);

	SYSTEMTIME sysTime;
	GetSystemTime(&sysTime);
	sprintf(dumpLine, "Date: %02d-%02d-%04d %02d:%02d:%02d\n\n", (sint32)sysTime.wDay, (sint32)sysTime.wMonth, (sint32)sysTime.wYear, (sint32)sysTime.wHour, (sint32)sysTime.wMinute, (sint32)sysTime.wSecond);
	cemuLog_writePlainToLog(dumpLine);

	DumpThreadStackTrace();
	// info about exception
	if (e->ExceptionRecord)
	{
		HMODULE exceptionModule;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(e->ExceptionRecord->ExceptionAddress), &exceptionModule))
		{
			char moduleName[512];
			moduleName[0] = '\0';
			GetModuleFileNameA(exceptionModule, moduleName, 512);
			sint32 moduleNameStartIndex = std::max((sint32)0, (sint32)strlen(moduleName) - 1);
			while (moduleNameStartIndex > 0)
			{
				if (moduleName[moduleNameStartIndex] == '\\' || moduleName[moduleNameStartIndex] == '/')
				{
					moduleNameStartIndex++;
					break;
				}
				moduleNameStartIndex--;
			}
			sprintf(dumpLine, "Exception 0x%08x at 0x%I64x(+0x%I64x) in module %s\n", (uint32)e->ExceptionRecord->ExceptionCode, (uint64)e->ExceptionRecord->ExceptionAddress, (uint64)e->ExceptionRecord->ExceptionAddress - (uint64)exceptionModule, moduleName + moduleNameStartIndex);
			cemuLog_writePlainToLog(dumpLine);
		}
		else
		{
			sprintf(dumpLine, "Exception 0x%08x at 0x%I64x\n", (uint32)e->ExceptionRecord->ExceptionCode, (uint64)e->ExceptionRecord->ExceptionAddress);
			cemuLog_writePlainToLog(dumpLine);
		}

	}
	sprintf(dumpLine, "cemu.exe at 0x%I64x\n", (uint64)GetModuleHandle(NULL));
	cemuLog_writePlainToLog(dumpLine);
	// register info
	sprintf(dumpLine, "\n");
	cemuLog_writePlainToLog(dumpLine);
	sprintf(dumpLine, "RAX=%016I64x RBX=%016I64x RCX=%016I64x RDX=%016I64x\n", context->Rax, context->Rbx, context->Rcx, context->Rdx);
	cemuLog_writePlainToLog(dumpLine);
	sprintf(dumpLine, "RSP=%016I64x RBP=%016I64x RDI=%016I64x RSI=%016I64x\n", context->Rsp, context->Rbp, context->Rdi, context->Rsi);
	cemuLog_writePlainToLog(dumpLine);
	sprintf(dumpLine, "R8 =%016I64x R9 =%016I64x R10=%016I64x R11=%016I64x\n", context->R8, context->R9, context->R10, context->R11);
	cemuLog_writePlainToLog(dumpLine);
	sprintf(dumpLine, "R12=%016I64x R13=%016I64x R14=%016I64x R15=%016I64x\n", context->R12, context->R13, context->R14, context->R15);
	cemuLog_writePlainToLog(dumpLine);
	// info about game
	cemuLog_writePlainToLog("\n");
	crashlog_writeHeader("Game info");
	if (CafeSystem::IsTitleRunning())
	{
		cemuLog_writePlainToLog("Game: ");
		cemuLog_writePlainToLog(CafeSystem::GetForegroundTitleName().c_str());
		cemuLog_writePlainToLog("\n");
		// title id
		sprintf(dumpLine, "TitleId: %I64x\n", CafeSystem::GetForegroundTitleId());
		cemuLog_writePlainToLog(dumpLine);
		// rpx hash
		sprintf(dumpLine, "RPXHash: %x\n", currentBaseApplicationHash);
		cemuLog_writePlainToLog(dumpLine);
	}
	else
	{
		cemuLog_writePlainToLog("Not running\n");
	}
	// info about active PPC instance:
	cemuLog_writePlainToLog("\n");
	crashlog_writeHeader("Active PPC instance");
	if (ppcInterpreterCurrentInstance)
	{		
		OSThread_t* currentThread = coreinit::OSGetCurrentThread();
		uint32 threadPtr = memory_getVirtualOffsetFromPointer(coreinit::OSGetCurrentThread());
		sprintf(dumpLine, "IP 0x%08x LR 0x%08x Thread 0x%08x\n", ppcInterpreterCurrentInstance->instructionPointer, ppcInterpreterCurrentInstance->spr.LR, threadPtr);
		cemuLog_writePlainToLog(dumpLine);

		// GPR info
		sprintf(dumpLine, "\n");
		cemuLog_writePlainToLog(dumpLine);
		auto gprs = ppcInterpreterCurrentInstance->gpr;
		sprintf(dumpLine, "r0 =%08x r1 =%08x r2 =%08x r3 =%08x r4 =%08x r5 =%08x r6 =%08x r7 =%08x\n", gprs[0], gprs[1], gprs[2], gprs[3], gprs[4], gprs[5], gprs[6], gprs[7]);
		cemuLog_writePlainToLog(dumpLine);
		sprintf(dumpLine, "r8 =%08x r9 =%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x\n", gprs[8], gprs[9], gprs[10], gprs[11], gprs[12], gprs[13], gprs[14], gprs[15]);
		cemuLog_writePlainToLog(dumpLine);
		sprintf(dumpLine, "r16=%08x r17=%08x r18=%08x r19=%08x r20=%08x r21=%08x r22=%08x r23=%08x\n", gprs[16], gprs[17], gprs[18], gprs[19], gprs[20], gprs[21], gprs[22], gprs[23]);
		cemuLog_writePlainToLog(dumpLine);
		sprintf(dumpLine, "r24=%08x r25=%08x r26=%08x r27=%08x r28=%08x r29=%08x r30=%08x r31=%08x\n", gprs[24], gprs[25], gprs[26], gprs[27], gprs[28], gprs[29], gprs[30], gprs[31]);
		cemuLog_writePlainToLog(dumpLine);

		// write line to log
		cemuLog_writePlainToLog(dumpLine);

		// stack trace
		MPTR currentStackVAddr = ppcInterpreterCurrentInstance->gpr[1];
		cemuLog_writePlainToLog("\n");
		crashlog_writeHeader("PPC stack trace");
		DebugLogStackTrace(currentThread, currentStackVAddr);

		// stack dump
		cemuLog_writePlainToLog("\n");
		crashlog_writeHeader("PPC stack dump");
		for (uint32 i = 0; i < 16; i++)
		{
			MPTR lineAddr = currentStackVAddr + i * 8 * 4;
			if (memory_isAddressRangeAccessible(lineAddr, 8 * 4))
			{
				sprintf(dumpLine, "[0x%08x] %08x %08x %08x %08x - %08x %08x %08x %08x\n", lineAddr, memory_readU32(lineAddr + 0), memory_readU32(lineAddr + 4), memory_readU32(lineAddr + 8), memory_readU32(lineAddr + 12), memory_readU32(lineAddr + 16), memory_readU32(lineAddr + 20), memory_readU32(lineAddr + 24), memory_readU32(lineAddr + 28));
				cemuLog_writePlainToLog(dumpLine);
			}
			else
			{
				sprintf(dumpLine, "[0x%08x] ?\n", lineAddr);
				cemuLog_writePlainToLog(dumpLine);
			}
		}
	}
	else
	{
		cemuLog_writePlainToLog("Not active\n");
	}

	// PPC thread log
	cemuLog_writePlainToLog("\n");
	crashlog_writeHeader("PPC threads");
	if (activeThreadCount == 0)
	{
		cemuLog_writePlainToLog("None active\n");
	}
	for (sint32 i = 0; i < activeThreadCount; i++)
	{
		MPTR threadItrMPTR = activeThread[i];
		OSThread_t* threadItrBE = (OSThread_t*)memory_getPointerFromVirtualOffset(threadItrMPTR);

		// get thread state
		OSThread_t::THREAD_STATE threadState = threadItrBE->state;
		const char* threadStateStr = "UNDEFINED";
		if (threadItrBE->suspendCounter != 0)
			threadStateStr = "SUSPENDED";
		else if (threadState == OSThread_t::THREAD_STATE::STATE_NONE)
			threadStateStr = "NONE";
		else if (threadState == OSThread_t::THREAD_STATE::STATE_READY)
			threadStateStr = "READY";
		else if (threadState == OSThread_t::THREAD_STATE::STATE_RUNNING)
			threadStateStr = "RUNNING";
		else if (threadState == OSThread_t::THREAD_STATE::STATE_WAITING)
			threadStateStr = "WAITING";
		else if (threadState == OSThread_t::THREAD_STATE::STATE_MORIBUND)
			threadStateStr = "MORIBUND";
		// generate log line
		uint8 affinity = threadItrBE->attr;
		sint32 effectivePriority = threadItrBE->effectivePriority;
		const char* threadName = "NULL";
		if (!threadItrBE->threadName.IsNull())
			threadName = threadItrBE->threadName.GetPtr();
		sprintf(dumpLine, "%08x Ent %08x IP %08x LR %08x %-9s Aff %d%d%d Pri %2d Name %s\n", threadItrMPTR, _swapEndianU32(threadItrBE->entrypoint), threadItrBE->context.srr0, _swapEndianU32(threadItrBE->context.lr), threadStateStr, (affinity >> 0) & 1, (affinity >> 1) & 1, (affinity >> 2) & 1, effectivePriority, threadName);
		// write line to log
		cemuLog_writePlainToLog(dumpLine);
	}

	cemuLog_waitForFlush();

	// save log with the dump
	if (dump_written && crash_dump != CrashDump::Disabled)
	{
		const auto now = std::chrono::system_clock::now();
		const auto temp_time = std::chrono::system_clock::to_time_t(now);
		const auto& time = *std::gmtime(&temp_time);

		fs::path p = ActiveSettings::GetPath("crashdump");
		p /= fmt::format("log_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}.txt", 1900 + time.tm_year, time.tm_mon + 1, time.tm_mday, time.tm_year, time.tm_hour, time.tm_min, time.tm_sec);

		std::error_code ec;
		fs::copy_file(ActiveSettings::GetPath("log.txt"), p, ec);
	}

	if (IsCemuhookLoaded())
		TerminateProcess(GetCurrentProcess(), 0); // abort();
	else
		exit(0);

	return;
}

bool logCrashlog;

int crashlogThread(void* exceptionInfoRawPtr)
{
	PEXCEPTION_POINTERS pExceptionInfo = (PEXCEPTION_POINTERS)exceptionInfoRawPtr;
	createCrashlog(pExceptionInfo, pExceptionInfo->ContextRecord);
	logCrashlog = true;
	return 0;
}

void debugger_handleSingleStepException(uint32 drMask);

LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
	if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP)
	{
		LONG r = handleException_SINGLE_STEP(pExceptionInfo);
		if (r != EXCEPTION_CONTINUE_SEARCH)
			return r;
		debugger_handleSingleStepException(pExceptionInfo->ContextRecord->Dr6 & 0xF);
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI cemu_unhandledExceptionFilter(EXCEPTION_POINTERS* pExceptionInfo)
{
	createCrashlog(pExceptionInfo, pExceptionInfo->ContextRecord);
	return EXCEPTION_NONCONTINUABLE_EXCEPTION;
}

void ExceptionHandler_init()
{
	SetUnhandledExceptionFilter(cemu_unhandledExceptionFilter);
	AddVectoredExceptionHandler(1, VectoredExceptionHandler);
	SetErrorMode(SEM_FAILCRITICALERRORS);
}
#else

void handler_SIGSEGV(int sig)
{
    printf("SIGSEGV!\n");

    void *array[32];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 32);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

void ExceptionHandler_init()
{
    signal(SIGSEGV, handler_SIGSEGV);
}

#endif