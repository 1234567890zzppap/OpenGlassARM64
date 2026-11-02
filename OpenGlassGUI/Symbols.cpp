#include "pch.h"
#include "ApplicationPaths.hpp"
#include "PeCodeViewIdentity.hpp"
#include "Symbols.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <wil/cppwinrt.h>

#include <chrono>
#include <filesystem>
#include <roapi.h>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")

namespace OpenGlass
{
	namespace
	{
		auto WaitForDownload(const auto& operation, const std::stop_token& stopToken)
		{
			// A read racing Cancel can reach Canceled without delivering Completed.
			// Cancellation must wake the worker independently of that notification.
			const wil::shared_event finished{ wil::EventOptions::ManualReset };
			const std::stop_callback cancel{ stopToken, [&operation, &finished]
			{
				try
				{
					operation.Cancel();
				}
				catch (...)
				{
					LOG_CAUGHT_EXCEPTION();
				}
				finished.SetEvent();
			} };
			// The provider may retain its callback after cancellation or timeout.
			operation.Completed([finished](const auto&, auto) { finished.SetEvent(); });
			const bool signaled = finished.wait(SymbolDownloadTimeoutSeconds * 1000);
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stopToken.stop_requested());
			if (!signaled)
			{
				operation.Cancel();
				THROW_HR(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled)
			{
				throw winrt::hresult_canceled{};
			}
			return operation.GetResults();
		}

		void DownloadSymbolFile(
			const winrt::Windows::Web::Http::HttpClient& client,
			const std::stop_token& stopToken,
			const winrt::hstring& url,
			const std::filesystem::path& destination,
			int basePercent,
			const SymbolDownloadProgressCallback& progressCallback
		)
		{
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stopToken.stop_requested());

			const winrt::Windows::Foundation::Uri uri{ url };
			const winrt::Windows::Web::Http::HttpRequestMessage request{ winrt::Windows::Web::Http::HttpMethod::Get(), uri };
			const auto response = WaitForDownload(client.SendRequestAsync(
				request, winrt::Windows::Web::Http::HttpCompletionOption::ResponseHeadersRead
			), stopToken);
			response.EnsureSuccessStatusCode();
			const auto content = response.Content();
			const auto length = content.Headers().ContentLength();
			const uint64_t totalBytes = length ? length.Value() : 0;

			// A cancelled or failed refresh must leave the existing symbol cache usable.
			// Keep the staging file beside its destination so publication stays on one volume.
			GUID stagingId{};
			THROW_IF_FAILED(CoCreateGuid(&stagingId));
			WCHAR stagingName[39]{};
			StringFromGUID2(stagingId, stagingName, ARRAYSIZE(stagingName));
			auto targetPath = std::filesystem::absolute(destination).wstring();
			if (!targetPath.starts_with(L"\\\\?\\"))
			{
				// A longer staging name must not shorten the supported cache directory path.
				targetPath = targetPath.starts_with(L"\\\\")
					? L"\\\\?\\UNC\\" + targetPath.substr(2) : L"\\\\?\\" + targetPath;
			}
			const auto stagingPath = std::filesystem::path{ targetPath }.parent_path() / stagingName;
			wil::unique_hfile file{ CreateFileW(
				stagingPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr
			) };
			THROW_LAST_ERROR_IF(!file);

			bool completed = false;
			const auto cleanup = wil::scope_exit([&]
			{
				file.reset();
				if (!completed)
				{
					std::error_code cleanupError;
					std::filesystem::remove(stagingPath, cleanupError);
				}
			});

			const auto stream = WaitForDownload(content.ReadAsInputStreamAsync(), stopToken);
			winrt::Windows::Storage::Streams::Buffer buffer{ 128 * 1024 };
			const auto fileName = destination.filename().wstring();
			auto nextProgress = std::chrono::steady_clock::time_point::min();

			uint64_t downloaded = 0;
			for (;;)
			{
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stopToken.stop_requested());

				const auto read = WaitForDownload(stream.ReadAsync(
					buffer, buffer.Capacity(), winrt::Windows::Storage::Streams::InputStreamOptions::Partial
				), stopToken);
				if (read.Length() == 0)
				{
					break;
				}

				DWORD written = 0;
				THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), read.data(), read.Length(), &written, nullptr));
				downloaded += written;

				// Each callback queues a UI update and rewraps status text. Network chunk
				// boundaries are not useful UI events; keep formatting and delivery at 10 Hz.
				const auto now = std::chrono::steady_clock::now();
				if (progressCallback && now >= nextProgress)
				{
					const auto percent = totalBytes
						? std::clamp(static_cast<int>(100.0 * downloaded / totalBytes), 0, 100) : 0;
					std::wstring detail = std::format(L"{} - {} bytes", fileName, downloaded);
					if (totalBytes)
					{
						detail += std::format(L" / {} bytes ({}%)", totalBytes, percent);
					}
					progressCallback(SymbolDownloadProgress{
						basePercent + percent / 2, totalBytes == 0,
						totalBytes ? std::format(L"Downloading {}", fileName) : L"Connecting to Microsoft Symbol Server...",
						std::move(detail)
					});
					nextProgress = now + std::chrono::milliseconds{ 100 };
				}
			}
			// A stream can report EOF without rejecting a truncated Content-Length body.
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_HANDLE_EOF), length && downloaded != totalBytes);
			THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(file.get()));
			THROW_IF_WIN32_BOOL_FALSE(CloseHandle(file.release()));
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stopToken.stop_requested());
			THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(stagingPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING));
			completed = true;
		}

		std::wstring GetSystemModulePath(PCWSTR moduleName)
		{
			WCHAR systemPath[MAX_PATH]{};
			const UINT length = GetSystemDirectoryW(systemPath, ARRAYSIZE(systemPath));
			if (length == 0 || length >= ARRAYSIZE(systemPath))
			{
				return {};
			}

			return (std::filesystem::path{ systemPath } / moduleName).wstring();
		}

		std::wstring FormatHResult(HRESULT hr)
		{
			return std::format(L"0x{:08X}", static_cast<unsigned long>(hr));
		}

		std::wstring DescribeDownloadFailure(const std::wstring& itemName, HRESULT hr)
		{
			if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
			{
				return std::format(
					L"Timed out after {} seconds while downloading {}.",
					SymbolDownloadTimeoutSeconds,
					itemName
				);
			}

			return std::format(L"Failed to download {} ({}).", itemName, FormatHResult(hr));
		}
	}

	std::wstring GetSymbolCacheDirectory()
	{
		return ApplicationPaths::GetProgramDataSubdirectory(L"symbols").wstring();
	}

	SymbolDownloadOutcome DownloadSymbols(std::wstring symbolDirectory, const std::stop_token& stopToken, const SymbolDownloadProgressCallback& progressCallback)
	{
		SymbolDownloadOutcome outcome{};
		std::wstring downloading{ L"symbols" };
		try
		{
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stopToken.stop_requested());
			outcome.symbolDirectory = symbolDirectory.empty() ? GetSymbolCacheDirectory() : std::move(symbolDirectory);

			const HRESULT roInitResult = RoInitialize(RO_INIT_MULTITHREADED);
			const auto roScope = wil::scope_exit([roInitResult]
			{
				if (SUCCEEDED(roInitResult))
				{
					RoUninitialize();
				}
			});
			if (FAILED(roInitResult) && roInitResult != RPC_E_CHANGED_MODE)
			{
				outcome.hr = roInitResult;
				outcome.summary = std::format(L"Failed to initialize the Windows Runtime ({}).", FormatHResult(roInitResult));
				return outcome;
			}

			std::error_code directoryError;
			std::filesystem::create_directories(outcome.symbolDirectory, directoryError);
			if (directoryError)
			{
				outcome.hr = HRESULT_FROM_WIN32(directoryError.value());
				outcome.summary = std::format(L"Failed to create the symbols directory:\n{}", outcome.symbolDirectory);
				return outcome;
			}

			const winrt::Windows::Web::Http::HttpClient client;
			constexpr LPCWSTR symbolServerBase = L"https://msdl.microsoft.com/download/symbols/";
			struct ModuleSpec
			{
				LPCWSTR name;
				int basePercent;
			};
			constexpr ModuleSpec modules[]
			{
				{ L"uDWM.dll", 0 },
				{ L"dwmcore.dll", 50 },
			};

			for (const auto& module : modules)
			{
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stopToken.stop_requested());
				outcome.details.clear();
				const auto modulePath = GetSystemModulePath(module.name);
				if (modulePath.empty())
				{
					outcome.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
					outcome.summary = std::format(L"Failed to locate {} in the system directory.", module.name);
					return outcome;
				}

				PeCodeViewIdentity identity{};
				const HRESULT hr = ReadFilePeCodeViewIdentity(modulePath.c_str(), identity);
				if (FAILED(hr))
				{
					outcome.hr = hr;
					outcome.summary = std::format(L"Failed to prepare symbol download for {} ({}).", module.name, FormatHResult(hr));
					outcome.details = std::format(L"Module: {}\nPath: {}", module.name, modulePath);
					return outcome;
				}

				const auto url = BuildSymbolServerUrl(symbolServerBase, identity);
				downloading = identity.pdbName;
				outcome.details = std::format(
					L"Module: {}\nPath: {}\nPDB: {}\nURL: {}", module.name, modulePath, identity.pdbName, url
				);
				if (progressCallback)
				{
					progressCallback(SymbolDownloadProgress{
						module.basePercent, true, L"Connecting to Microsoft Symbol Server...",
						std::format(L"Requesting {} from Microsoft Symbol Server", identity.pdbName)
					});
				}

				DownloadSymbolFile(
					client, stopToken, url.c_str(),
					std::filesystem::path{ outcome.symbolDirectory } / identity.pdbName,
					module.basePercent, progressCallback
				);
				if (progressCallback)
				{
					progressCallback(SymbolDownloadProgress{
						module.basePercent + 50, false,
						std::format(L"Finished {}", identity.pdbName), L"Local symbol cache updated."
					});
				}
			}

			outcome.result = SymbolDownloadResult::Success;
			outcome.summary = L"Required symbols downloaded successfully.";
			outcome.details.clear();
		}
		catch (...)
		{
			// This is the worker entry point: activation, paths and callbacks can throw
			// as well as network operations, and none may escape the GUI's jthread.
			outcome.hr = stopToken.stop_requested() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : wil::ResultFromCaughtException();
			outcome.result = outcome.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)
				? SymbolDownloadResult::Cancelled : SymbolDownloadResult::Failed;
			outcome.summary = outcome.result == SymbolDownloadResult::Cancelled
				? L"Symbol download cancelled." : DescribeDownloadFailure(downloading, outcome.hr);
		}
		return outcome;
	}
}
