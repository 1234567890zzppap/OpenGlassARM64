#pragma once

#include <Windows.h>
#include <cstdarg>
#include <cstdio>

namespace OpenGlass::StartupTrace
{
	// DWM runs without a debugger and not every termination produces a WER dump, so the
	// startup sequence records how far it got. The trace stays bounded: one append per
	// milestone, and the first writable path wins. Tracing must never change behavior.
	inline void WriteV(PCSTR format, va_list arguments) noexcept
	{
		SYSTEMTIME localTime{};
		GetLocalTime(&localTime);

		CHAR body[512]{};
		if (_vsnprintf_s(body, _TRUNCATE, format, arguments) <= 0)
		{
			return;
		}

		CHAR line[640]{};
		const auto length = _snprintf_s(
			line,
			_TRUNCATE,
			"[%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu] %s\r\n",
			localTime.wYear,
			localTime.wMonth,
			localTime.wDay,
			localTime.wHour,
			localTime.wMinute,
			localTime.wSecond,
			localTime.wMilliseconds,
			body
		);
		if (length <= 0)
		{
			return;
		}

		static constexpr PCWSTR tracePaths[]
		{
			// The installer grants the Window Manager account write access to the symbol cache,
			// so that directory is the most reliable place for a trace written from dwm.exe.
			L"C:\\ProgramData\\OpenGlass\\symbols\\startup-trace.log",
			L"C:\\ProgramData\\OpenGlass\\startup-trace.log",
			L"C:\\Windows\\Temp\\OpenGlass-startup-trace.log",
		};
		for (const auto path : tracePaths)
		{
			const HANDLE file = CreateFileW(
				path,
				FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr
			);
			if (file == INVALID_HANDLE_VALUE)
			{
				continue;
			}
			DWORD written{};
			WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
			CloseHandle(file);
			return;
		}
	}

	inline void Write(PCSTR format, ...) noexcept
	{
		va_list arguments;
		va_start(arguments, format);
		WriteV(format, arguments);
		va_end(arguments);
	}
}
