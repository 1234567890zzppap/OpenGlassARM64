#include "pch.h"
#include "MainFrame.hpp"
#include "WrappingLayout.hpp"

namespace OpenGlass
{
	void MainFrame::StartSymbolDownload()
	{
		if (m_symbolDownloadRunning || !m_isAdmin)
		{
			return;
		}

		m_closeWhenSymbolDownloadStops = false;
		m_symbolDownloadRunning = true;
		m_btnDownloadSymbols->Enable(false);
		m_btnCancelSymbolDownload->Enable(true);
		m_dpSymbolCacheDirectory->Enable(false);
		std::wstring symbolDirectory = m_dpSymbolCacheDirectory->GetPath().ToStdWstring();
		if (symbolDirectory.empty())
		{
			symbolDirectory = GetSymbolCacheDirectory();
			m_dpSymbolCacheDirectory->SetPath(symbolDirectory);
		}
		UpdateSymbolDownloadResult(wxART_INFORMATION, wxEmptyString);
		UpdateSymbolDownloadProgress(SymbolDownloadProgress{
			0,
			true,
			L"Connecting to Microsoft Symbol Server...",
			L"Preparing symbol download."
		});

		m_symbolDownloadThread = std::jthread([this, symbolDirectory = std::move(symbolDirectory)](std::stop_token stopToken)
		{
			const auto progressCallback = [this](const SymbolDownloadProgress& progress)
			{
				CallAfter([this, progress]
				{
					UpdateSymbolDownloadProgress(progress);
				});
			};

			const SymbolDownloadOutcome outcome = DownloadSymbols(symbolDirectory, stopToken, progressCallback);
			CallAfter([this, outcome]
			{
				FinishSymbolDownload(outcome);
			});
		});
	}

	void MainFrame::RefreshDiagnosticsLayout()
	{
		wxWindow* parent = nullptr;
		if (m_pnlSymbolDownloadResult)
		{
			parent = m_pnlSymbolDownloadResult->GetParent();
		}
		else if (m_lblSymbolDownloadDetail)
		{
			parent = m_lblSymbolDownloadDetail->GetParent();
		}

		if (parent)
		{
			// Assign widths first, including a result panel that has just become visible.
			parent->Layout();
			if (m_pnlSymbolDownloadResult) m_pnlSymbolDownloadResult->Layout();
			WrapStaticTextToParentWidth(m_lblSymbolDownloadDetail, m_symbolDownloadDetailText);
			WrapStaticTextToParentWidth(m_lblSymbolDownloadResult, m_symbolDownloadResultText);
			WrapStaticTextToParentWidth(m_lblDwmCrashDumpStatus, m_dwmCrashDumpStatusText);
			parent->Layout();
			if (wxScrolledWindow* scrolled = wxDynamicCast(parent, wxScrolledWindow))
			{
				scrolled->FitInside();
			}
		}
	}

	void MainFrame::RefreshTransparencyDiagnostics()
	{
		if (!m_lblWindowsTransparencyStatus || !m_lblOpaqueBlendStatus || !m_lblEnergySaverStatus || !m_lblDisableOnBatteryStatus || !m_bmpEffectiveTransparencyWarning || !m_lblEffectiveTransparencyStatus)
		{
			return;
		}

		TransparencyDiagnostics diagnostics;
		const HRESULT result = QueryTransparencyDiagnostics(m_targetUserSid.ToStdWstring(), diagnostics);
		if (FAILED(result))
		{
			const wxString error = wxString::Format(L"Unavailable (HRESULT 0x%08lX)", static_cast<unsigned long>(result));
			m_lblWindowsTransparencyStatus->SetLabel(error);
			m_lblOpaqueBlendStatus->SetLabel(error);
			m_lblEnergySaverStatus->SetLabel(error);
			m_lblDisableOnBatteryStatus->SetLabel(error);
			m_bmpEffectiveTransparencyWarning->Hide();
			m_lblEffectiveTransparencyStatus->SetLabel(error);
			RefreshDiagnosticsLayout();
			return;
		}

		auto sourceName = [](DwmSettings::ValueSource source)
		{
			switch (source)
			{
			case DwmSettings::ValueSource::User: return L"HKCU";
			case DwmSettings::ValueSource::Machine: return L"HKLM";
			default: return L"default";
			}
		};

		m_lblWindowsTransparencyStatus->SetLabel(diagnostics.windowsTransparencyEnabled ? L"On" : L"Off (opaque)");
		m_lblOpaqueBlendStatus->SetLabel(wxString::Format(
			L"%lu (%ls, %ls)",
			static_cast<unsigned long>(diagnostics.colorizationOpaqueBlend),
			sourceName(diagnostics.colorizationOpaqueBlendSource),
			diagnostics.colorizationOpaqueBlend ? L"opaque" : L"transparent"
		));
		m_lblEnergySaverStatus->SetLabel(diagnostics.powerSaverActive ? L"On" : L"Off");
		m_lblDisableOnBatteryStatus->SetLabel(wxString::Format(
			L"%ls (%ls)",
			diagnostics.disableGlassOnBattery ? L"On" : L"Off",
			sourceName(diagnostics.disableGlassOnBatterySource)
		));

		const bool isOpaque = DwmSettings::IsTransparencyDisabled(
			diagnostics.colorizationOpaqueBlend != 0,
			diagnostics.windowsTransparencyEnabled,
			diagnostics.powerSaverActive,
			diagnostics.disableGlassOnBattery
		);
		wxString resultText = L"Transparent";
		if (diagnostics.colorizationOpaqueBlend)
		{
			resultText = L"Opaque: Opaque blend";
		}
		else if (diagnostics.powerSaverActive && diagnostics.disableGlassOnBattery)
		{
			resultText = L"Opaque: Power saver";
		}
		else if (!diagnostics.windowsTransparencyEnabled)
		{
			resultText = L"Opaque: Windows setting";
		}
		m_bmpEffectiveTransparencyWarning->Show(isOpaque);
		m_lblEffectiveTransparencyStatus->SetLabel(resultText);
		RefreshDiagnosticsLayout();
	}

	void MainFrame::UpdateSymbolDownloadProgress(const SymbolDownloadProgress& progress)
	{
		if (m_gaugeSymbolDownload)
		{
			if (progress.indeterminate)
			{
				m_gaugeSymbolDownload->Pulse();
			}
			else
			{
				m_gaugeSymbolDownload->SetValue(std::clamp(progress.percent, 0, 100));
			}
		}

		bool textChanged{};
		if (m_lblSymbolDownloadPhase)
		{
			if (m_lblSymbolDownloadPhase->GetLabelText() != progress.phase)
			{
				m_lblSymbolDownloadPhase->SetLabel(progress.phase);
				textChanged = true;
			}
		}
		if (m_lblSymbolDownloadDetail)
		{
			if (m_symbolDownloadDetailText != progress.detail)
			{
				m_symbolDownloadDetailText = progress.detail;
				textChanged = true;
			}
		}
		if (textChanged) RefreshDiagnosticsLayout();
	}

	void MainFrame::UpdateSymbolDownloadResult(wxArtID iconId, const wxString& details)
	{
		if (!m_pnlSymbolDownloadResult || !m_bmpSymbolDownloadResult || !m_lblSymbolDownloadResult)
		{
			return;
		}

		if (details.empty())
		{
			m_symbolDownloadResultText.clear();
			m_lblSymbolDownloadResult->SetLabel(wxEmptyString);
			m_pnlSymbolDownloadResult->Hide();
			RefreshDiagnosticsLayout();
			return;
		}

		m_bmpSymbolDownloadResult->SetBitmap(
			wxArtProvider::GetBitmapBundle(iconId, wxART_MESSAGE_BOX, wxSize(16, 16))
		);

		m_symbolDownloadResultText = details;
		m_pnlSymbolDownloadResult->Show();
		RefreshDiagnosticsLayout();
	}

	void MainFrame::FinishSymbolDownload(const SymbolDownloadOutcome& outcome)
	{
		m_symbolDownloadRunning = false;
		m_btnDownloadSymbols->Enable(m_isAdmin);
		m_btnCancelSymbolDownload->Enable(false);
		m_dpSymbolCacheDirectory->Enable(m_isAdmin);

		switch (outcome.result)
		{
		case SymbolDownloadResult::Success:
			UpdateSymbolDownloadProgress(SymbolDownloadProgress{
				100,
				false,
				L"Symbols downloaded successfully.",
				std::format(L"The symbol cache has been updated:\n{}", outcome.symbolDirectory)
			});
			UpdateSymbolDownloadResult(wxART_INFORMATION, wxEmptyString);
			break;
		case SymbolDownloadResult::Cancelled:
			UpdateSymbolDownloadProgress(SymbolDownloadProgress{
				m_gaugeSymbolDownload ? m_gaugeSymbolDownload->GetValue() : 0,
				false,
				L"Symbol download cancelled.",
				L"No further network requests will be started."
			});
			UpdateSymbolDownloadResult(wxART_WARNING, outcome.details);
			break;
		case SymbolDownloadResult::Failed:
		default:
			UpdateSymbolDownloadProgress(SymbolDownloadProgress{
				m_gaugeSymbolDownload ? m_gaugeSymbolDownload->GetValue() : 0,
				false,
				L"Symbol download failed.",
				outcome.summary
			});
			UpdateSymbolDownloadResult(wxART_ERROR, outcome.details);
			break;
		}

		const bool closePending = m_closeWhenSymbolDownloadStops;
		m_closeWhenSymbolDownloadStops = false;
		if (closePending)
		{
			Close(true);
			return;
		}
	}

	void MainFrame::RefreshDwmCrashDumpConfiguration()
	{
		DwmCrashDumpConfiguration configuration;
		const HRESULT result = QueryDwmCrashDumpConfiguration(configuration);
		if (FAILED(result))
		{
			m_dwmCrashDumpStatusText = wxString::Format(
				L"Unable to read the dwm.exe WER configuration (HRESULT 0x%08lX).",
				static_cast<unsigned long>(result)
			);
			m_btnEnableDwmCrashDumps->Enable(m_isAdmin);
			m_btnDisableDwmCrashDumps->Enable(m_isAdmin && configuration.enabled);
			RefreshDiagnosticsLayout();
			return;
		}

		m_btnEnableDwmCrashDumps->Enable(m_isAdmin);
		m_btnDisableDwmCrashDumps->Enable(m_isAdmin && configuration.enabled);
		if (!configuration.enabled)
		{
			m_dwmCrashDumpStatusText = L"Disabled. No per-application WER LocalDumps configuration exists for dwm.exe. System-wide WER settings, if present, may still apply.";
			RefreshDiagnosticsLayout();
			return;
		}

		if (!configuration.dumpFolder.empty())
		{
			m_dpDwmCrashDumpFolder->SetPath(configuration.dumpFolder);
		}

		const wxString folder = configuration.dumpFolder.empty()
			? wxString{ L"Windows default" }
			: wxString{ configuration.dumpFolder };
		if (configuration.dumpType == 2 && configuration.dumpCount == 1 && !configuration.dumpFolder.empty())
		{
			m_dwmCrashDumpStatusText = wxString::Format(
				L"Enabled for dwm.exe: full dump, keep 1, folder: %s",
				folder
			);
		}
		else
		{
			m_dwmCrashDumpStatusText = wxString::Format(
				L"Enabled with custom settings for dwm.exe: DumpType=%lu, DumpCount=%lu, folder: %s. Click Enable full dumps to apply the recommended OpenGlass settings.",
				configuration.dumpType,
				configuration.dumpCount,
				folder
			);
		}
		RefreshDiagnosticsLayout();
	}

	void MainFrame::SetDwmCrashDumpsEnabled(bool enabled)
	{
		if (!m_isAdmin)
		{
			return;
		}

		HRESULT result{};
		if (enabled)
		{
			std::wstring requestedFolder = m_dpDwmCrashDumpFolder->GetPath().ToStdWstring();
			if (requestedFolder.empty())
			{
				requestedFolder = GetDefaultDwmCrashDumpFolder();
			}

			std::wstring configuredFolder;
			result = EnableDwmCrashDumps(requestedFolder, configuredFolder);
			if (SUCCEEDED(result))
			{
				m_dpDwmCrashDumpFolder->SetPath(configuredFolder);
			}
		}
		else
		{
			result = DisableDwmCrashDumps();
		}

		if (FAILED(result))
		{
			wxMessageBox(
				wxString::Format(
					enabled
						? L"Failed to enable WER crash dumps (HRESULT 0x%08lX)."
						: L"Failed to disable WER crash dumps (HRESULT 0x%08lX).",
					static_cast<unsigned long>(result)
				),
				L"WER crash dumps",
				wxOK | wxICON_ERROR,
				this
			);
		}
		RefreshDwmCrashDumpConfiguration();
	}
}
