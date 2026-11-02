#include "pch.h"
#include "GlassFrameIntegrity.hpp"
#include "uDwmProjection.hpp"
#include "WindowFrameBounds.hpp"

using namespace OpenGlass;

namespace OpenGlass::GlassFrameIntegrity
{
	RECT* MyCTopLevelWindow_GetActualWindowRect(uDWM::CTopLevelWindow* This, RECT* rect, char relative, char respectMaximizedClip, bool excludeShadowMargins);
	HRESULT MyCTopLevelWindow_UpdatePinnedParts(uDWM::CTopLevelWindow* This);
	bool MyCSecondaryWindowRepresentation_CalculateWindowMetrics(uDWM::CSecondaryWindowRepresentation* This, SIZE* size, MARGINS* margins);
	void MyCProjectionBorderVisual_UpdateRectFromWindow(uDWM::CProjectionBorderVisual* This, uDWM::CWindowData* data);
	void MyCProjectionBorderVisualShim_UpdateRectFromWindow(uDWM::CProjectionBorderVisualShim* This, uDWM::CWindowData* data);

	Projection::Detour<uDWM::Symbol_CTopLevelWindow_GetActualWindowRect, &MyCTopLevelWindow_GetActualWindowRect> g_CTopLevelWindow_GetActualWindowRect_Org{};
	Projection::Detour<uDWM::Symbol_CTopLevelWindow_UpdatePinnedParts, &MyCTopLevelWindow_UpdatePinnedParts> g_CTopLevelWindow_UpdatePinnedParts_Org{};
	Projection::Detour<uDWM::Symbol_CSecondaryWindowRepresentation_CalculateWindowMetrics, &MyCSecondaryWindowRepresentation_CalculateWindowMetrics> g_CSecondaryWindowRepresentation_CalculateWindowMetrics_Org{};
	Projection::Detour<uDWM::Symbol_CProjectionBorderVisual_UpdateRectFromWindow, &MyCProjectionBorderVisual_UpdateRectFromWindow> g_CProjectionBorderVisual_UpdateRectFromWindow_Org{};
	Projection::Detour<uDWM::Symbol_CProjectionBorderVisualShim_UpdateRectFromWindow, &MyCProjectionBorderVisualShim_UpdateRectFromWindow> g_CProjectionBorderVisualShim_UpdateRectFromWindow_Org{};

	decltype(uDWM::Symbol_CProjectionBorderVisual_UpdateRect.get()) g_CProjectionBorderVisual_UpdateRect{};
	decltype(uDWM::Symbol_CProjectionBorderVisualShim_UpdateRect.get()) g_CProjectionBorderVisualShim_UpdateRect{};
	decltype(uDWM::Symbol_CWindowData_NotifySWROfMarginOrSizeChange.get()) g_CWindowData_NotifySWROfMarginOrSizeChange{};
	bool g_useProjectionBorderShim{};

	bool GetAtlasFrameRect(uDWM::CTopLevelWindow* window, RECT* rect, bool relative)
	{
		const auto left = window->GetNCAreaAtlasImage1();
		const auto right = window->GetNCAreaAtlasImage2();
		const auto bottom = window->GetNCAreaAtlasImage3();
		if (!left || !right || !bottom)
		{
			return false;
		}
		// Request native outside margins without entering its modern-border shortcut.
		// This retains its actual maximized-clip flag, not an appearance override.
		g_CTopLevelWindow_GetActualWindowRect_Org(window, rect, relative, true, false);
		auto bounds = window->GetData()->GetWindowRect();
		if (relative)
		{
			OffsetRect(&bounds, -bounds.left, -bounds.top);
		}
		*rect = WindowFrameBounds::ExcludeAtlasShadow(
			bounds, *rect, window->GetFrameInsideMargins(),
			left->GetSize(), right->GetSize(), bottom->GetSize()
		);
		return true;
	}

	void UpdateThumbnailMargins(const uDWM::CWindowData* data, MARGINS* margins)
	{
		const auto window = data->GetWindow();
		RECT rect;
		if (window && !window->HasMaximizedClip() && !window->GetCloneFrameMargins() && GetAtlasFrameRect(window, &rect, true))
		{
			const auto& bounds = window->GetData()->GetWindowRect();
			*margins = { rect.left, bounds.right - bounds.left - rect.right,
				rect.top, bounds.bottom - bounds.top - rect.bottom };
		}
	}

	void RefreshWindow(uDWM::CTopLevelWindow* window)
	{
		const auto data = window->GetData();
		// A cloned tree can borrow the live owner's data; never notify that owner here.
		if (!data || data->GetWindow() != window)
		{
			return;
		}
		if (const auto border = data->GetProjectionBorder(); border)
		{
			if (g_useProjectionBorderShim)
			{
				MyCProjectionBorderVisualShim_UpdateRectFromWindow(reinterpret_cast<uDWM::CProjectionBorderVisualShim*>(border), data);
			}
			else
			{
				MyCProjectionBorderVisual_UpdateRectFromWindow(reinterpret_cast<uDWM::CProjectionBorderVisual*>(border), data);
			}
			// This sibling may already have been visited in the current frame.
			border->RenderRecursive();
		}
		g_CWindowData_NotifySWROfMarginOrSizeChange(data, true, false);
	}

	void ApplyHooks()
	{
		HookHelper::ApplyInlineHooks(
			std::initializer_list<HookHelper::DetourInfo>
			{
				{ &g_CTopLevelWindow_UpdatePinnedParts_Org },
				{ &g_CProjectionBorderVisual_UpdateRectFromWindow_Org, !g_useProjectionBorderShim },
				{ &g_CProjectionBorderVisualShim_UpdateRectFromWindow_Org, g_useProjectionBorderShim },
				{ &g_CTopLevelWindow_GetActualWindowRect_Org },
				{ &g_CSecondaryWindowRepresentation_CalculateWindowMetrics_Org }
			}
		);
	}
}

RECT* GlassFrameIntegrity::MyCTopLevelWindow_GetActualWindowRect(
	uDWM::CTopLevelWindow* This, RECT* rect, char relative, char respectMaximizedClip, bool excludeShadowMargins)
{
	if (respectMaximizedClip && excludeShadowMargins && GetAtlasFrameRect(This, rect, relative != 0))
	{
		return rect;
	}
	return g_CTopLevelWindow_GetActualWindowRect_Org(This, rect, relative, respectMaximizedClip, excludeShadowMargins);
}

bool GlassFrameIntegrity::MyCSecondaryWindowRepresentation_CalculateWindowMetrics(
	uDWM::CSecondaryWindowRepresentation* This, SIZE* size, MARGINS* margins)
{
	const auto result = g_CSecondaryWindowRepresentation_CalculateWindowMetrics_Org(This, size, margins);
	// Kinds 1 and 2 share the original window owner. Only change the full-window
	// crop; keep native snapshot dimensions and client/explicit-source metrics.
	if (result && (This->GetKind() == 1 || This->GetKind() == 2) && !(This->GetFlags() & 0x84))
	{
		UpdateThumbnailMargins(This->GetData(), margins);
	}
	return result;
}

void GlassFrameIntegrity::MyCProjectionBorderVisual_UpdateRectFromWindow(
	uDWM::CProjectionBorderVisual* This, uDWM::CWindowData* data)
{
	g_CProjectionBorderVisual_UpdateRectFromWindow_Org(This, data);
	if (const auto window = data->GetWindow(); window)
	{
		RECT rect;
		window->GetActualWindowRect(&rect, false, true, true);
		g_CProjectionBorderVisual_UpdateRect(This, rect);
	}
}

void GlassFrameIntegrity::MyCProjectionBorderVisualShim_UpdateRectFromWindow(
	uDWM::CProjectionBorderVisualShim* This, uDWM::CWindowData* data)
{
	// Retain native capture/projection selection, rounded corners and DPI updates.
	g_CProjectionBorderVisualShim_UpdateRectFromWindow_Org(This, data);
	if (const auto window = data->GetWindow(); window)
	{
		RECT rect;
		window->GetActualWindowRect(&rect, false, true, true);
		g_CProjectionBorderVisualShim_UpdateRect(This, rect);
	}
}

HRESULT GlassFrameIntegrity::MyCTopLevelWindow_UpdatePinnedParts(uDWM::CTopLevelWindow* This)
{
	const auto hr = g_CTopLevelWindow_UpdatePinnedParts_Org(This);
	if (SUCCEEDED(hr))
	{
		// UpdateWindowVisuals supplies intrinsic bitmap sizes; UpdatePinnedParts
		// finishes their realized dimensions. Notify only after that layout completes.
		RefreshWindow(This);
	}
	return hr;
}

void GlassFrameIntegrity::Update([[maybe_unused]] GlassEngine::UpdateType type)
{
}

void GlassFrameIntegrity::Startup()
{
	g_useProjectionBorderShim = uDWM::Symbol_CProjectionBorderVisualShim_UpdateRectFromWindow.try_get() != nullptr;
	g_CProjectionBorderVisual_UpdateRect = uDWM::Symbol_CProjectionBorderVisual_UpdateRect.get();
	if (g_useProjectionBorderShim)
	{
		g_CProjectionBorderVisualShim_UpdateRect = uDWM::Symbol_CProjectionBorderVisualShim_UpdateRect.get();
	}
	g_CWindowData_NotifySWROfMarginOrSizeChange = uDWM::Symbol_CWindowData_NotifySWROfMarginOrSizeChange.get();
	ApplyHooks();
}

void GlassFrameIntegrity::Shutdown()
{
	ApplyHooks();
}

void GlassFrameIntegrity::Cleanup()
{
}
