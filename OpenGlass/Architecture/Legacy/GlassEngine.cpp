#include "pch.h"
#include "StartupTrace.hpp"
#include "Shared.hpp"
#include "GlassEngine.hpp"
#include "CaptionTextHandler.hpp"
#include "CaptionMetricsTweaker.hpp"
#include "ButtonGlowHandler.hpp"
#include "GlassKernel.hpp"
#include "AccentOverrider.hpp"
#include "GlassFrameHandler.hpp"
#include "GlassFrameDemodernizer.hpp"
#include "GlassFrameEnhancer.hpp"
#include "GlassReflectionHandler.hpp"
#include "GlassRenderer.hpp"
#include "GlassIntegrity.hpp"
#include "CustomThemeAtlasLoader.hpp"
#include "GlassService.hpp"
#include "SystemSettings.hpp"

using namespace OpenGlass;
namespace OpenGlass::GlassEngine
{
	wil::unique_hkey g_dwmKey{ nullptr };
	wil::unique_hkey g_machineDwmKey{ nullptr };
	wil::unique_hkey g_personalizeKey{ nullptr };
	HWND g_notificationWindow{ nullptr };
}

HKEY GlassEngine::GetDwmKey()
{
	return g_dwmKey.get();
}

HKEY GlassEngine::GetMachineDwmKey()
{
	return g_machineDwmKey.get();
}

HKEY GlassEngine::GetPersonalizeKey()
{
	return g_personalizeKey.get();
}

void GlassEngine::LoadRegistry(bool redrawNow)
{
	UnloadRegistry();

	GlassService::RequestBuffer content{ GlassService::RequestType::OpenUserRegistry };
	const auto hr = GlassService::SendRequest(content);
	if (SUCCEEDED(hr))
	{
		if (content.dwmKey)
		{
			g_dwmKey.reset(content.dwmKey);
		}
		if (content.personalizeKey)
		{
			g_personalizeKey.reset(content.personalizeKey);
		}
		Update(UpdateType::All, redrawNow);
	}
}
void GlassEngine::UnloadRegistry()
{
	g_dwmKey.reset();
	LOG_IF_FAILED(wil::reg::open_unique_key_nothrow(HKEY_LOCAL_MACHINE, DwmSettings::g_keyPath, g_machineDwmKey));
	LOG_IF_FAILED(wil::reg::open_unique_key_nothrow(HKEY_LOCAL_MACHINE, Util::g_personalizeKeyPath, g_personalizeKey));
}

void GlassEngine::SetDwmNotificationWindow(HWND hWnd)
{
	g_notificationWindow = hWnd;
}

HWND GlassEngine::GetDwmNotificationWindow()
{
	return g_notificationWindow;
}

void GlassEngine::Update(UpdateType type, bool redrawNow)
{
	{
		const auto lock = wil::EnterCriticalSection(uDWM::CDesktopManager::s_csDwmInstance);
		GlassKernel::Update(type);
		GlassIntegrity::Update(type);
		GlassRenderer::Update(type);

		CustomThemeAtlasLoader::Update(type);
		CaptionTextHandler::Update(type);
		CaptionMetricsTweaker::Update(type);
		ButtonGlowHandler::Update(type);
		AccentOverrider::Update(type);
		GlassFrameHandler::Update(type);
		GlassFrameDemodernizer::Update(type);
		GlassFrameEnhancer::Update(type);
		GlassReflectionHandler::Update(type);
		if (redrawNow)
		{
			GlassKernel::RedrawAllTopLevelWindow(false);
		}
	}
	LOG_IF_FAILED(DwmFlush());
}

void GlassEngine::Startup()
{
	Shared::g_disabledHooks = GetDwordFromRegistry(L"DisabledHooks", 0);

	// wait for compositor thread to be idle before starting up,
	// to avoid racing with it and causing potential deadlock or other issues
	LOG_IF_FAILED(DwmFlush());

	StartupTrace::Write("engine: hook rundown opened");
	HookHelper::GetHookRundown().Open();
	{
		HookHelper::HookTransaction transaction{ HookHelper::HookMode::Install };
		StartupTrace::Write("engine: GlassKernel::Startup");
		GlassKernel::Startup();
		StartupTrace::Write("engine: GlassIntegrity::Startup");
		GlassIntegrity::Startup();
		StartupTrace::Write("engine: GlassRenderer::Startup");
		GlassRenderer::Startup();
		{
			const auto lock = wil::EnterCriticalSection(uDWM::CDesktopManager::s_csDwmInstance);
			StartupTrace::Write("engine: CustomThemeAtlasLoader::Startup");
			CustomThemeAtlasLoader::Startup();
			StartupTrace::Write("engine: CaptionTextHandler::Startup");
			CaptionTextHandler::Startup();
			StartupTrace::Write("engine: CaptionMetricsTweaker::Startup");
			CaptionMetricsTweaker::Startup();
			StartupTrace::Write("engine: ButtonGlowHandler::Startup");
			ButtonGlowHandler::Startup();
			StartupTrace::Write("engine: AccentOverrider::Startup");
			AccentOverrider::Startup();
			StartupTrace::Write("engine: GlassFrameHandler::Startup");
			GlassFrameHandler::Startup();
			StartupTrace::Write("engine: GlassFrameEnhancer::Startup");
			GlassFrameEnhancer::Startup();
			StartupTrace::Write("engine: GlassFrameDemodernizer::Startup");
			GlassFrameDemodernizer::Startup();
			StartupTrace::Write("engine: GlassReflectionHandler::Startup");
			GlassReflectionHandler::Startup();
			StartupTrace::Write("engine: committing the hook transaction");
			StartupTrace::Write("shutdown: committing the remove transaction");
			transaction.Commit();
			StartupTrace::Write("shutdown: remove transaction committed");
			StartupTrace::Write("engine: hook transaction committed");
			StartupTrace::Write("engine: redrawing all top-level windows");
			GlassKernel::RedrawAllTopLevelWindow(true);
			StartupTrace::Write("engine: startup complete");
		}
	}
}
void GlassEngine::Activate()
{
	CustomThemeAtlasLoader::Activate();
}
void GlassEngine::Shutdown()
{
	StartupTrace::Write("shutdown: begin");
	HookHelper::GetHookRundown().BeginShutdown();
	LOG_IF_FAILED(DwmFlush());

	{
		HookHelper::HookTransaction transaction{ HookHelper::HookMode::Remove };
		StartupTrace::Write("shutdown: removing hooks");
		GlassReflectionHandler::Shutdown();
		GlassFrameDemodernizer::Shutdown();
		GlassFrameEnhancer::Shutdown();
		GlassFrameHandler::Shutdown();
		AccentOverrider::Shutdown();
		ButtonGlowHandler::Shutdown();
		CaptionMetricsTweaker::Shutdown();
		CaptionTextHandler::Shutdown();
		CustomThemeAtlasLoader::Shutdown();
		GlassRenderer::Shutdown();
		GlassIntegrity::Shutdown();
		GlassKernel::Shutdown();
		{
			const auto lock = wil::EnterCriticalSection(uDWM::CDesktopManager::s_csDwmInstance);
			StartupTrace::Write("shutdown: committing the remove transaction");
			transaction.Commit();
			StartupTrace::Write("shutdown: remove transaction committed");
		}
	}

	StartupTrace::Write("shutdown: waiting for the hook rundown");
	HookHelper::GetHookRundown().WaitForDrain(std::chrono::seconds{ 1 });
	StartupTrace::Write("shutdown: hook rundown drained");

	{
		const auto lock = wil::EnterCriticalSection(uDWM::CDesktopManager::s_csDwmInstance);
		GlassReflectionHandler::Cleanup();
		GlassFrameDemodernizer::Cleanup();
		GlassFrameEnhancer::Cleanup();
		GlassFrameHandler::Cleanup();
		AccentOverrider::Cleanup();
		ButtonGlowHandler::Cleanup();
		CaptionMetricsTweaker::Cleanup();
		CaptionTextHandler::Cleanup();
	}

	CustomThemeAtlasLoader::Deactivate();

	{
		const auto lock = wil::EnterCriticalSection(uDWM::CDesktopManager::s_csDwmInstance);
		StartupTrace::Write("shutdown: redrawing all top-level windows");
		GlassKernel::RedrawAllTopLevelWindow(true);
		StartupTrace::Write("shutdown: redraw done");
		GlassRenderer::Cleanup();
		GlassIntegrity::Cleanup();
		GlassKernel::Cleanup();
	}
}
