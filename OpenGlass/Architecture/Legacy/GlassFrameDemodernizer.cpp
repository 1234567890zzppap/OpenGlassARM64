#include "pch.h"
#include "Shared.hpp"
#include "GlassEngine.hpp"
#include "GlassFrameDemodernizer.hpp"
#include "DetourChains.hpp"
#include "StartupTrace.hpp"

using namespace OpenGlass;

namespace OpenGlass::GlassFrameDemodernizer
{
	extern "C" bool WINAPI MySetMargin(
		MARGINS* dstMargins,
		int cxLeftWidth,
		int cxRightWidth,
		int cyTopHeight,
		int cyBottomHeight,
		const MARGINS* srcMargins
	);
	extern "C" bool WINAPI MySetMargin_Impl(
		MARGINS* dstMargins,
		int cxLeftWidth,
		int cxRightWidth,
		int cyTopHeight,
		int cyBottomHeight,
		const MARGINS* srcMargins
	);

	// uDWM exposes CTopLevelWindow::UpdateWindowVisuals through the ARM64 code-address anchor that
	// the forced high-contrast patch already uses; the projected pointer type is only declared for
	// x64, so the detached detour keeps its own storage here.
#if defined(_M_ARM64)
	HRESULT MyCTopLevelWindow_UpdateWindowVisuals(uDWM::CTopLevelWindow* This);
	using CTopLevelWindow_UpdateWindowVisuals_t = HRESULT (*)(uDWM::CTopLevelWindow*);
	CTopLevelWindow_UpdateWindowVisuals_t g_CTopLevelWindow_UpdateWindowVisuals_Org{ nullptr };
#endif

	DetourChains::TopLevelWindowValidateVisualDemodernizerNode g_CTopLevelWindow_ValidateVisual_Org{};
	DetourChains::TopLevelWindowUpdateNCAreaBackgroundDemodernizerNode g_CTopLevelWindow_UpdateNCAreaBackground_Org{};
	Projection::CustomDispatchDetour<uDWM::Symbol_SetMargin, &MySetMargin> g_SetMargin_Org{};

	UCHAR g_callCDesktopManager_IsHighContrastMode_Instructions[]
	{
		// call ???
		0xE8, 0x00, 0x00, 0x00, 0x00,
		// test al, al
		0x84, 0xC0
	};
	UCHAR g_callCDesktopManager_IsHighContrastMode_replacedInstruction[]
	{
		// move al, 0x01
		0xB0, 0x01,
		// nop
		// nop
		// nop
		0x90, 0x90, 0x90,
		// test al, al
		0x84, 0xC0
	};
	UCHAR g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_Instructions[]
	{
		// lea eax, [???-12h]
		0x8D, 0x00, 0xEE,
		// cmp eax, 3
		0x83, 0xF8, 0x03,
		// ja short loc_xxxxxxxx
		0x77,
	};
	UCHAR g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_replacedInstructions[]
	{
		// lea eax, [???-12h]
		0x8D, 0x00, 0xEE,
		// nop
		// nop
		// nop
		0x90, 0x90, 0x90,
		// jmp
		0xEB,
	};
	UCHAR g_callCTopLevelWindow_IsShadowNCAreaPart_Instructions[]
	{
		// call ???
		0xE8, 0x00, 0x00, 0x00, 0x00,
		// test al, al
		0x84, 0xC0
	};
	UCHAR g_callCTopLevelWindow_IsShadowNCAreaPart_replacedInstructions[]
	{
		// move al, 0x00
		0xB0, 0x00,
		// nop
		// nop
		// nop
		0x90, 0x90, 0x90,
		// test al, al
		0x84, 0xC0
	};
	std::unordered_map<UCHAR*, std::vector<UCHAR>> g_instructionsToReplace{};
	std::unordered_map<UCHAR*, std::vector<UCHAR>> g_instructionsBackup{};
	std::vector<HookHelper::InstructionPatch> g_instructionPatches{};
	std::vector<std::byte> g_atlasedImageNullSentinel{};
	bool g_systemBackdrop{ false };
}

HRESULT GlassFrameDemodernizer::MyCTopLevelWindow_ValidateVisual(uDWM::CTopLevelWindow* This)
{
	auto data = This->GetData();
	if (!data)
	{
		return g_CTopLevelWindow_ValidateVisual_Org(This);
	}

	auto& systemBackdropType = data->GetSystemBackdropType();
	auto& extendedFrameMargins = data->GetExtendedFrameMargins();
	auto& nonclientAttribute = data->GetNonClientAttributeReference();
	auto& borderUpdatesSuppressed = This->GetIsBorderUpdatesSuppressed();
	const auto disableModernFrames = Shared::g_disableModernBorders;
	const auto old_borderUpdatesSuppressed = borderUpdatesSuppressed;
	const auto old_systemBackdropType = systemBackdropType;
	const auto old_extendedFrameMargins = extendedFrameMargins;
	const auto old_nonclientAttribute = nonclientAttribute;

	g_systemBackdrop = (uDWM::g_versionInfo.build == os::build_w11_21h2 && old_systemBackdropType) || (uDWM::g_versionInfo.build > os::build_w11_21h2 && old_systemBackdropType >= DWMSBT_MAINWINDOW);
	// DemodernizerForcedCalls bit4 keeps the backdrop suppression together with the
	// frame-extension rewrite it implies. Clearing the bit leaves both window fields untouched,
	// so the native non-client geometry survives the call unchanged.
	static const auto s_forcedCalls = GlassEngine::GetDwordFromRegistry(L"DemodernizerForcedCalls", 23);
	const auto suppressBackdrop = (s_forcedCalls & 16) != 0;
	const auto extendFrame = g_systemBackdrop && suppressBackdrop;
	if (suppressBackdrop)
	{
		systemBackdropType = (uDWM::g_versionInfo.build == os::build_w11_21h2 ? DWMSBT_AUTO : DWMSBT_NONE);
	}
	{
		static std::atomic<unsigned> s_logged{ 0 };
		const auto sample = s_logged.fetch_add(1, std::memory_order_relaxed);
		if (sample < 12 || (sample % 64) == 0)
		{
			StartupTrace::Write(
				"frame: validate window=%p backdrop=%u systemBackdrop=%d extendFrame=%d margins=%d,%d,%d,%d disableModernFrames=%d mask=%#010x",
				static_cast<void*>(This),
				static_cast<unsigned>(old_systemBackdropType),
				g_systemBackdrop ? 1 : 0,
				extendFrame ? 1 : 0,
				old_extendedFrameMargins.cxLeftWidth,
				old_extendedFrameMargins.cxRightWidth,
				old_extendedFrameMargins.cyTopHeight,
				old_extendedFrameMargins.cyBottomHeight,
				disableModernFrames ? 1 : 0,
				s_forcedCalls
			);
		}
	}
	if (extendFrame)
	{
		// known issue: 
		// set cyTopHeight to any non zero value will cause compatibility issue with Outlook (new)
		// Outlook (new) has drawn its own titlebar buttons with native titlebar buttons hidden,
		// this will make the native titlebar buttons visible
		// 
		extendedFrameMargins.cyTopHeight = 0x7FFFFFFF;
		extendedFrameMargins.cxLeftWidth = 0x7FFFFFFF;
		extendedFrameMargins.cxRightWidth = 0x7FFFFFFF;
		extendedFrameMargins.cyBottomHeight = 0x7FFFFFFF;
		nonclientAttribute |= 8;
	}
	
	if (disableModernFrames)
	{
		borderUpdatesSuppressed = true;
		if (const auto windowBorder = This->GetWindowBorder(); windowBorder)
		{
			RETURN_IF_FAILED(windowBorder->EnableBorder(false));
		}
	}

	const auto scope = wil::scope_exit([&, old_borderUpdatesSuppressed, old_systemBackdropType, old_extendedFrameMargins, old_nonclientAttribute, extendFrame]
	{
		if (disableModernFrames)
		{
			borderUpdatesSuppressed = old_borderUpdatesSuppressed;
		}
		if (extendFrame)
		{
			extendedFrameMargins = old_extendedFrameMargins;
			nonclientAttribute = old_nonclientAttribute;
		}
		g_systemBackdrop = false;
		systemBackdropType = old_systemBackdropType;
	});

	return g_CTopLevelWindow_ValidateVisual_Org(This);
}

#if defined(_M_ARM64)
namespace
{
	bool HasNonClientGeometry(uDWM::CTopLevelWindow* window);
}

HRESULT GlassFrameDemodernizer::MyCTopLevelWindow_UpdateWindowVisuals(uDWM::CTopLevelWindow* This)
{
	const auto data = This->GetData();
	// DemodernizerForcedCalls bit3 keeps the legacy backdrop type visible to the visual update
	// so CTopLevelWindow::CalculateBackgroundType stops reporting a modern background and the
	// later background update rebuilds CLegacyNonClientBackground instead of clearing it.
	if (!data || !Shared::g_disableModernBorders || !HasNonClientGeometry(This))
	{
		return g_CTopLevelWindow_UpdateWindowVisuals_Org(This);
	}

	auto& systemBackdropType = data->GetSystemBackdropType();
	const auto oldSystemBackdropType = systemBackdropType;
	systemBackdropType = DWMSBT_NONE;
	const auto scope = wil::scope_exit([&systemBackdropType, oldSystemBackdropType]
	{
		systemBackdropType = oldSystemBackdropType;
	});

	{
		static std::atomic<unsigned> s_logged{ 0 };
		const auto sample = s_logged.fetch_add(1, std::memory_order_relaxed);
		if (sample < 12 || (sample % 64) == 0)
		{
			StartupTrace::Write(
				"frame: update-visuals window=%p backdrop=%u forced=%u",
				static_cast<void*>(This),
				static_cast<unsigned>(oldSystemBackdropType),
				static_cast<unsigned>(systemBackdropType)
			);
		}
	}

	return g_CTopLevelWindow_UpdateWindowVisuals_Org(This);
}
#endif

#if defined(_M_ARM64)
namespace
{
	// The x64 builds force the inlined CDesktopManager::IsHighContrastMode result inside
	// CTopLevelWindow::UpdateWindowVisuals by rewriting instruction bytes that they locate
	// by scanning for x86 call and test sequences. ARM64 uDWM never calls that predicate:
	// it reads the flag inline twice, once to clear a rectangle and skip the modern
	// background block, and once to capture a bool that the later border and background
	// code consumes. Only the captured-bool read is forced here. Faking the flag for the
	// whole call also reaches GetRoundedBorderColor and the border brush setup, which paints
	// the window border black, and forcing the rect-clear read removes the background fill
	// again; both were reproduced on the 22631 ARM64 build.
	UCHAR g_arm64ForcedHighContrast_BoolCapture_Instructions[]
	{
		// ldrb w9,[x8,#0x1A]
		0x09, 0x69, 0x40, 0x39,
		// cbz w9, +0xC  (capture zero instead of one)
		0x69, 0x00, 0x00, 0x34
	};
	UCHAR g_arm64ForcedHighContrast_BoolCapture_Replacement[]
	{
		// ldrb w9,[x8,#0x1A]
		0x09, 0x69, 0x40, 0x39,
		// nop; fall through so the captured bool stays one
		0x1F, 0x20, 0x03, 0xD5
	};
	// x64 builds patch a second site, the inlined CTopLevelWindow::IsShadowNCAreaPart range
	// test, so the forced high-contrast branch never takes the shadow-part path. ARM64 uDWM
	// exports no symbol for that predicate, but UpdateWindowVisuals keeps the identical
	// source expression inline next to the captured bool: (part - 0x12) <= 3 followed by a
	// conditional branch to the shared path. Forcing the branch to always leave the range
	// reproduces the x64 outcome; the condition word is read at run time so only its opcode
	// class has to be recognised.
	UCHAR g_arm64ForcedHighContrast_ShadowPartTest_Instructions[]
	{
		// sub w8,w22,#0x12
		0xC8, 0x4A, 0x00, 0x51,
		// cmp w8,#3
		0x1F, 0x0D, 0x00, 0x71
	};
	std::vector<HookHelper::InstructionPatch> g_arm64ForcedHighContrastPatches{};

	struct ForcedHighContrastPatchTemplate
	{
		const UCHAR* expected;
		size_t expectedSize;
		const UCHAR* replacement;
		size_t replacementSize;
	};

	const UCHAR* FindUniqueInstructionSequence(
		const UCHAR* code,
		size_t searchLength,
		const UCHAR* expected,
		size_t expectedSize,
		PCSTR description
	)
	{
		const UCHAR* match = nullptr;
		for (size_t offset = 0; offset + expectedSize <= searchLength; offset++)
		{
			if (memcmp(code + offset, expected, expectedSize) != 0)
			{
				continue;
			}
			FAIL_FAST_IF_FAILED_MSG(
				match == nullptr ? S_OK : E_UNEXPECTED,
				"%hs is ambiguous",
				description
			);
			match = code + offset;
		}
		FAIL_FAST_IF_FAILED_MSG(match ? S_OK : E_NOINTERFACE, "%hs was not found", description);
		return match;
	}

	// Replaces one "bl <predicate>" with "mov w0, #imm" so the forced result reaches the caller
	// without touching the consumer that follows. Neither predicate takes its result in a
	// register that survives the call, and no caller can rely on flags across a call.
	void PrepareForcedCallPatch(
		const UCHAR* code,
		size_t searchLength,
		PVOID target,
		DWORD forcedValue,
		PCSTR description
	)
	{
		if (!target)
		{
			// A missing predicate only costs the forced decision, so record it and keep DWM alive
			// instead of turning a new build family's unknown shape into a startup failure.
			StartupTrace::Write("patch: %hs target is unresolved, skipping", description);
			return;
		}
		const auto targetAddress = reinterpret_cast<ULONG_PTR>(target);

		UCHAR* match = nullptr;
		size_t matches = 0;
		for (size_t offset = 0; offset + sizeof(UINT) <= searchLength; offset += sizeof(UINT))
		{
			UINT word{};
			memcpy(&word, code + offset, sizeof(word));
			if ((word & 0xFC000000u) != 0x94000000u)
			{
				continue;
			}
			const auto raw = static_cast<LONG>(word & 0x03FFFFFFu);
			const auto signedOffset = (raw & 0x02000000) ? raw - 0x04000000 : raw;
			const auto site = reinterpret_cast<ULONG_PTR>(code + offset);
			if (site + static_cast<ULONG_PTR>(static_cast<LONGLONG>(signedOffset) * 4) != targetAddress)
			{
				continue;
			}
			match = const_cast<UCHAR*>(code + offset);
			matches++;
		}
		if (matches != 1)
		{
			StartupTrace::Write(
				"patch: %hs matched %zu site(s), skipping",
				description,
				matches
			);
			return;
		}
		StartupTrace::Write("patch: %hs at %p -> mov w0,#%u", description, match, forcedValue);

		const auto forcedWord = 0x52800000u | ((forcedValue & 1u) << 5);
		const auto forcedBytes = reinterpret_cast<const UCHAR*>(&forcedWord);
		auto& patch = g_arm64ForcedHighContrastPatches.emplace_back();
		patch.Prepare(
			match,
			std::vector<UCHAR>(match, match + sizeof(UINT)),
			std::vector<UCHAR>(forcedBytes, forcedBytes + sizeof(forcedWord))
		);
	}

	void PrepareForcedHighContrastPatches()
	{
		const auto visuals = static_cast<const UCHAR*>(
			uDWM::Symbol_CTopLevelWindow_UpdateWindowVisuals_ForcedHighContrast.get()
		);
		constexpr size_t searchLength = 0x2000;

		if (uDWM::g_versionInfo.build >= os::build_w11_24h2)
		{
			// 26100 ARM64 has exactly one "bl CDesktopManager::IsHighContrastMode" in
			// UpdateWindowVisuals (0x354a4) and it is the decision that also skips
			// CWindowBorder::SetBorderParameters (0x35514) and CWindowBorder::EnableBorder(1)
			// (0x35550). Forcing it to 1 therefore freezes the border geometry: the window keeps
			// resizing while its border keeps the old rectangle, which the user sees as dragging
			// the top edge and the bottom edge moving instead. The x64 patch survives only because
			// its scan keeps the second match, inside GetBorderRect, rather than this decision;
			// that anchor does not exist on ARM64. Forcing IsShadowNCAreaPart alone cannot replace
			// it either: 0x355a4 skips the whole GetWindowFramePart path while w25 is zero.
			StartupTrace::Write(
				"patch: 24H2 forced predicate calls are not installed on ARM64 in %p (they would stop the window border from tracking resizes)",
				visuals
			);
			return;
		}

		const ForcedHighContrastPatchTemplate templates[]
		{
			{
				g_arm64ForcedHighContrast_BoolCapture_Instructions,
				sizeof(g_arm64ForcedHighContrast_BoolCapture_Instructions),
				g_arm64ForcedHighContrast_BoolCapture_Replacement,
				sizeof(g_arm64ForcedHighContrast_BoolCapture_Replacement)
			},
		};

		for (const auto& patchTemplate : templates)
		{
			const auto match = FindUniqueInstructionSequence(
				visuals,
				searchLength,
				patchTemplate.expected,
				patchTemplate.expectedSize,
				"the forced high-contrast read"
			);

			auto& patch = g_arm64ForcedHighContrastPatches.emplace_back();
			patch.Prepare(
				const_cast<UCHAR*>(match),
				std::vector<UCHAR>(patchTemplate.expected, patchTemplate.expected + patchTemplate.expectedSize),
				std::vector<UCHAR>(patchTemplate.replacement, patchTemplate.replacement + patchTemplate.replacementSize)
			);
		}

		const auto shadowTest = FindUniqueInstructionSequence(
			visuals,
			searchLength,
			g_arm64ForcedHighContrast_ShadowPartTest_Instructions,
			sizeof(g_arm64ForcedHighContrast_ShadowPartTest_Instructions),
			"the forced high-contrast shadow-part test"
		);
		const auto shadowTestSize = sizeof(g_arm64ForcedHighContrast_ShadowPartTest_Instructions);
		UINT branch{};
		memcpy(&branch, shadowTest + shadowTestSize, sizeof(branch));
		// Only B.cond leaves the range on "hi". B.cond stores a 19-bit word offset at bits
		// [23:5] while B stores a 26-bit one at bits [25:0], so the offset has to be
		// re-encoded rather than copied; the resulting branch must reach the same target.
		FAIL_FAST_IF_FAILED_MSG(
			(branch & 0xFF000010u) == 0x54000000u && (branch & 0xFu) == 0x8u
				? S_OK
				: E_UNEXPECTED,
			"the forced high-contrast shadow-part test does not branch with b.hi"
		);
		const auto conditionalOffset = static_cast<std::int32_t>((branch >> 5) & 0x7FFFFu);
		const auto signedOffset = (conditionalOffset & 0x40000) ? conditionalOffset - 0x80000 : conditionalOffset;
		const auto forcedBranch = 0x14000000u | (static_cast<std::uint32_t>(signedOffset) & 0x03FFFFFFu);
		FAIL_FAST_IF_FAILED_MSG(
			((forcedBranch & 0x03FFFFFFu) - ((forcedBranch & 0x02000000u) ? 0x04000000u : 0u)) ==
				static_cast<std::uint32_t>(signedOffset)
				? S_OK
				: E_UNEXPECTED,
			"the forced high-contrast shadow-part branch does not keep its target"
		);

		std::vector<UCHAR> shadowTestExpected(
			g_arm64ForcedHighContrast_ShadowPartTest_Instructions,
			g_arm64ForcedHighContrast_ShadowPartTest_Instructions + shadowTestSize
		);
		auto shadowTestReplacement = shadowTestExpected;
		const auto branchBytes = reinterpret_cast<const UCHAR*>(&forcedBranch);
		shadowTestReplacement.insert(shadowTestReplacement.end(), branchBytes, branchBytes + sizeof(forcedBranch));
		shadowTestExpected.insert(
			shadowTestExpected.end(),
			shadowTest + shadowTestSize,
			shadowTest + shadowTestSize + sizeof(branch)
		);

		auto& shadowPatch = g_arm64ForcedHighContrastPatches.emplace_back();
		shadowPatch.Prepare(
			const_cast<UCHAR*>(shadowTest),
			std::move(shadowTestExpected),
			std::move(shadowTestReplacement)
		);
	}

	// InstructionPatch toggles between the captured and the replacement bytes on every Apply,
	// so the same call installs the patches during Startup and removes them during Shutdown.
	void ApplyForcedHighContrastPatches()
	{
		for (auto& patch : g_arm64ForcedHighContrastPatches)
		{
			HookHelper::GetCurrentHookTransaction().Apply(patch);
		}
	}

	// uDWM itself refuses the rebuild path when CTopLevelWindow::GetFrameInsideMargins is entirely
	// zero, which is exactly what the desktop and other frameless top-level windows carry. Those
	// windows must keep their native backdrop type and non-client attribute: changing them clears
	// the desktop background and makes the whole session unusable.
	bool HasNonClientGeometry(uDWM::CTopLevelWindow* window)
	{
		const auto data = window->GetData();
		if (!data)
		{
			return false;
		}
		const auto hwnd = data->GetHwnd();
		if (!hwnd || hwnd == GetDesktopWindow())
		{
			return false;
		}
		const auto& margins = window->GetFrameInsideMargins();
		return
			margins.cxLeftWidth != 0 ||
			margins.cxRightWidth != 0 ||
			margins.cyTopHeight != 0 ||
			margins.cyBottomHeight != 0;
	}

	// uDWM 26100 chooses between "clear the legacy non-client background" and "rebuild it" with
	// bit 3 of CWindowData::GetNonClientAttribute: ldr x0,[x19,#0x110]; cbnz x0 -> ClearAll; ret
	// is the clear path, while the rebuild path needs that bit set and then runs
	// UpdateNCAreaGeometry/SetBorderColor/SetCaptionColor on the same object. Forcing only the
	// backdrop type leaves the bit clear, so the object exists but always ends up empty and the
	// window paints no non-client background at all. Raising the bit here keeps uDWM's own
	// create/attach/ownership path; it is restored when this call returns.
	void PrepareLegacyNonClientBackground(
		uDWM::CTopLevelWindow* window,
		uDWM::CWindowData*& overriddenData,
		BYTE& previousAttribute
	)
	{
		overriddenData = nullptr;
		if (uDWM::g_versionInfo.build < os::build_w11_24h2)
		{
			return;
		}
		static const auto s_enabled =
			(GlassEngine::GetDwordFromRegistry(L"DemodernizerForcedCalls", 23) & 8) != 0;
		// Raising the non-client attribute only makes sense while the modern border is switched
		// off. With DisableModernBorders=0 the border still paints from the real frame geometry,
		// so the half-state shows up as visuals that no longer line up with what the mouse hits.
		if (!s_enabled || !Shared::g_disableModernBorders)
		{
			return;
		}

		const auto data = window->GetData();
		const auto hwnd = data ? data->GetHwnd() : nullptr;
		const auto& margins = window->GetFrameInsideMargins();
		const auto hasFrame =
			margins.cxLeftWidth != 0 ||
			margins.cxRightWidth != 0 ||
			margins.cyTopHeight != 0 ||
			margins.cyBottomHeight != 0;
		const auto eligible = data && hwnd && hwnd != GetDesktopWindow() && hasFrame;

		// One probe line per window keeps the whole decision visible: which window, whether the
		// desktop guard or the frame-inside margins excluded it, and what uDWM would have seen.
		{
			static std::atomic<unsigned> s_logged{ 0 };
			const auto sample = s_logged.fetch_add(1, std::memory_order_relaxed);
			if (sample < 24 || (sample % 64) == 0)
			{
				StartupTrace::Write(
					"frame: legacy-background probe window=%p hwnd=%p margins=%d,%d,%d,%d visual=%p attribute=%#04x eligible=%d",
					static_cast<void*>(window),
					static_cast<void*>(hwnd),
					margins.cxLeftWidth,
					margins.cxRightWidth,
					margins.cyTopHeight,
					margins.cyBottomHeight,
					static_cast<void*>(window->GetLegacyVisual()),
					static_cast<unsigned>(data ? data->GetNonClientAttributeReference() : BYTE{}),
					eligible ? 1 : 0
				);
			}
		}
		if (!eligible)
		{
			return;
		}

		auto& attribute = data->GetNonClientAttributeReference();
		previousAttribute = attribute;
		attribute |= 8;
		overriddenData = data;

		static std::atomic<unsigned> s_applied{ 0 };
		const auto appliedSample = s_applied.fetch_add(1, std::memory_order_relaxed);
		if (appliedSample < 12 || (appliedSample % 64) == 0)
		{
			StartupTrace::Write(
				"frame: legacy-background apply window=%p attribute=%#04x->%#04x visual=%p",
				static_cast<void*>(window),
				static_cast<unsigned>(previousAttribute),
				static_cast<unsigned>(attribute),
				static_cast<void*>(window->GetLegacyVisual())
			);
		}
	}
}
#endif

HRESULT GlassFrameDemodernizer::MyCTopLevelWindow_UpdateNCAreaBackground(uDWM::CTopLevelWindow* This)
{
#if defined(_M_ARM64)
	uDWM::CWindowData* legacyData{};
	BYTE previousNonClientAttribute{};
	const auto legacyBackgroundScope = wil::scope_exit([&legacyData, &previousNonClientAttribute]
	{
		if (legacyData)
		{
			legacyData->GetNonClientAttributeReference() = previousNonClientAttribute;
		}
	});
	PrepareLegacyNonClientBackground(This, legacyData, previousNonClientAttribute);
#endif

	uDWM::CAtlasedImage const** atlasedImagePtrs[3]{};

	auto& highContrastMode = uDWM::CDesktopManager::GetInstance()->GetIsHighContrastMode();
	const auto old_highContrastMode = highContrastMode;

	// windows 11 rtm lacks null pointer check when high contrast mode is enabled
	// so we manually add it here
	if (
		uDWM::g_versionInfo.build == os::build_w11_21h2 && 
		uDWM::g_versionInfo.revision == os::revision_21h2_rtm_0 &&
		(
			atlasedImagePtrs[0] = &This->GetNCAreaAtlasImage1(),
			atlasedImagePtrs[1] = &This->GetNCAreaAtlasImage2(),
			atlasedImagePtrs[2] = &This->GetNCAreaAtlasImage3(),

			std::any_of(
				std::begin(atlasedImagePtrs),
				std::end(atlasedImagePtrs),
				[](auto atlasedImagePtr) { return *atlasedImagePtr == nullptr; }
			)
		)
	)
	{
		const auto atlasedImageNullSentinel =
			reinterpret_cast<uDWM::CAtlasedImage const*>(g_atlasedImageNullSentinel.data());

		for (auto& atlasedImagePtr : atlasedImagePtrs)
		{
			if (*atlasedImagePtr == nullptr)
			{
				*atlasedImagePtr = atlasedImageNullSentinel;
			}
			else
			{
				atlasedImagePtr = nullptr;
			}
		}
	}
	else
	{
		memset(atlasedImagePtrs, 0, sizeof(atlasedImagePtrs));
		// DemodernizerForcedCalls bit2 keeps the faked high-contrast flag for the duration of
		// the original call. It is deliberately limited to builds before 24H2: the 26100 ARM64
		// binary derives the non-client background from CTopLevelWindow::CalculateBackgroundType
		// and CLegacyNonClientBackground, so the faked flag only reaches the high-contrast block
		// that drops the modern background fill and leaves the caption buttons misdrawn. Real
		// device evidence: with bit2 set the extended-frame caption came back blank, and clearing
		// it restored that caption while the forced predicate patches stayed installed.
		static const auto s_fakeHighContrast =
			(GlassEngine::GetDwordFromRegistry(L"DemodernizerForcedCalls", 23) & 4) != 0 &&
			uDWM::g_versionInfo.build < os::build_w11_24h2;
		if (s_fakeHighContrast)
		{
			highContrastMode = true;
		}
	}
	const auto highContrastFakeScope = wil::scope_exit([&highContrastMode, old_highContrastMode, &atlasedImagePtrs]
	{
		highContrastMode = old_highContrastMode;
		for (auto& atlasedImagePtr : atlasedImagePtrs)
		{
			if (atlasedImagePtr)
			{
				*atlasedImagePtr = nullptr;
				atlasedImagePtr = nullptr;
			}
		}
	});

	return g_CTopLevelWindow_UpdateNCAreaBackground_Org(This);
}

bool WINAPI GlassFrameDemodernizer::MySetMargin_Impl(
	MARGINS* dstMargins,
	int cxLeftWidth,
	int cxRightWidth,
	int cyTopHeight,
	int cyBottomHeight,
	const MARGINS* srcMargins
)
{
	return g_SetMargin_Org.Dispatch(
		[](MARGINS* destination, int left, int right, int top, int bottom, const MARGINS* source)
		{
			return g_SetMargin_Org(
				destination,
				left,
				right,
				Shared::g_disableModernBorders ? std::max(0, top) : top,
				bottom,
				source
			);
		},
		dstMargins,
		cxLeftWidth,
		cxRightWidth,
		cyTopHeight,
		cyBottomHeight,
		srcMargins
	);
}

void GlassFrameDemodernizer::Update(GlassEngine::UpdateType type)
{
	if (uDWM::g_versionInfo.build < os::build_w11_21h2)
	{
		return;
	}

	if (type & GlassEngine::UpdateType::Theme)
	{
		Shared::g_disableModernBorders = static_cast<bool>(GlassEngine::GetDwordFromRegistry(L"DisableModernBorders", FALSE));
	}
}

void GlassFrameDemodernizer::Startup()
{
	if (uDWM::g_versionInfo.build < os::build_w11_21h2)
	{
		return;
	}

	if (
		uDWM::g_versionInfo.build == os::build_w11_21h2 &&
		uDWM::g_versionInfo.revision == os::revision_21h2_rtm_0
	)
	{
		const auto sizeOffset = uDWM::CAtlasedImage_GetSize.offset();
		FAIL_FAST_HR_IF_MSG(E_UNEXPECTED, sizeOffset < 0, "CAtlasedImage size offset is negative");
		FAIL_FAST_HR_IF_MSG(E_UNEXPECTED, static_cast<ULONGLONG>(sizeOffset) > SIZE_MAX - sizeof(DWORD), "CAtlasedImage size offset overflows");

		g_atlasedImageNullSentinel.assign(static_cast<size_t>(sizeOffset) + sizeof(DWORD), std::byte{});
		auto atlasedImageNullSentinel =
			reinterpret_cast<uDWM::CAtlasedImage*>(g_atlasedImageNullSentinel.data());
		atlasedImageNullSentinel->GetSize() = 0;
	}

#if !defined(_M_ARM64)
	auto CTopLevelWindow_UpdateWindowVisuals_Instructions =
		static_cast<UCHAR*>(uDWM::Symbol_CTopLevelWindow_UpdateWindowVisuals.get());
	const auto CDesktopManager_IsHighContrastMode_Instructions =
		static_cast<UCHAR*>(uDWM::Symbol_CDesktopManager_IsHighContrastMode.get());

	auto i = 1'500;
	const auto CTopLevelWindow_UpdateWindowVisuals_Instructions_Previous = CTopLevelWindow_UpdateWindowVisuals_Instructions;
	bool callCDesktopManager_IsHighContrastMode_SecondTime{};
	do
	{
		*reinterpret_cast<DWORD*>(&g_callCDesktopManager_IsHighContrastMode_Instructions[1]) = static_cast<DWORD>(CDesktopManager_IsHighContrastMode_Instructions - (CTopLevelWindow_UpdateWindowVisuals_Instructions + 5));
		if (
			memcmp(
				CTopLevelWindow_UpdateWindowVisuals_Instructions,
				g_callCDesktopManager_IsHighContrastMode_Instructions,
				sizeof(g_callCDesktopManager_IsHighContrastMode_Instructions)
			) == 0
		)
		{
			// in case we touched the inlined call part of CTopLevelWindow::GetBorderRect
			if (callCDesktopManager_IsHighContrastMode_SecondTime)
			{
				g_instructionsBackup.clear();
				g_instructionsToReplace.clear();
			}
			std::vector<UCHAR> backup(sizeof(g_callCDesktopManager_IsHighContrastMode_Instructions), 0);
			memcpy_s(
				backup.data(),
				backup.size(),
				CTopLevelWindow_UpdateWindowVisuals_Instructions,
				sizeof(g_callCDesktopManager_IsHighContrastMode_Instructions)
			);
			g_instructionsBackup.insert_or_assign(
				CTopLevelWindow_UpdateWindowVisuals_Instructions,
				backup
			);
			g_instructionsToReplace.insert_or_assign(
				CTopLevelWindow_UpdateWindowVisuals_Instructions,
				std::vector(std::begin(g_callCDesktopManager_IsHighContrastMode_replacedInstruction), std::end(g_callCDesktopManager_IsHighContrastMode_replacedInstruction))
			);
			callCDesktopManager_IsHighContrastMode_SecondTime = true;
		}

		CTopLevelWindow_UpdateWindowVisuals_Instructions += 1;
		i--;
	} while (i);

	CTopLevelWindow_UpdateWindowVisuals_Instructions = CTopLevelWindow_UpdateWindowVisuals_Instructions_Previous;
	if (uDWM::g_versionInfo.build < os::build_w11_24h2)
	{
		i = 450'000;
		do
		{
			g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_replacedInstructions[1] = g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_Instructions[1] = CTopLevelWindow_UpdateWindowVisuals_Instructions[1];
			if (
				memcmp(
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_Instructions,
					sizeof(g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_Instructions)
				) == 0
			)
			{
				std::vector<UCHAR> backup(sizeof(g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_Instructions), 0);
				memcpy_s(
					backup.data(),
					backup.size(),
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					sizeof(g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_Instructions)
				);
				g_instructionsBackup.insert_or_assign(
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					backup
				);
				g_instructionsToReplace.insert_or_assign(
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					std::vector(std::begin(g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_replacedInstructions), std::end(g_callCTopLevelWindow_IsShadowNCAreaPart_inlined_replacedInstructions))
				);
				break;
			}

			CTopLevelWindow_UpdateWindowVisuals_Instructions += 1;
			i--;
		} while (i);
	}
	else
	{
		const auto CTopLevelWindow_IsShadowNCAreaPart_Instructions =
			static_cast<UCHAR*>(uDWM::Symbol_CTopLevelWindow_IsShadowNCAreaPart.get());
		i = 1500;
		do
		{
			*reinterpret_cast<DWORD*>(&g_callCTopLevelWindow_IsShadowNCAreaPart_Instructions[1]) = static_cast<DWORD>(CTopLevelWindow_IsShadowNCAreaPart_Instructions - (CTopLevelWindow_UpdateWindowVisuals_Instructions + 5));
			if (
				memcmp(
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					g_callCTopLevelWindow_IsShadowNCAreaPart_Instructions,
					sizeof(g_callCTopLevelWindow_IsShadowNCAreaPart_Instructions)
				) == 0
			)
			{
				std::vector<UCHAR> backup(sizeof(g_callCTopLevelWindow_IsShadowNCAreaPart_Instructions), 0);
				memcpy_s(
					backup.data(),
					backup.size(),
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					sizeof(g_callCTopLevelWindow_IsShadowNCAreaPart_Instructions)
				);
				g_instructionsBackup.insert_or_assign(
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					backup
				);
				g_instructionsToReplace.insert_or_assign(
					CTopLevelWindow_UpdateWindowVisuals_Instructions,
					std::vector(std::begin(g_callCTopLevelWindow_IsShadowNCAreaPart_replacedInstructions), std::end(g_callCTopLevelWindow_IsShadowNCAreaPart_replacedInstructions))
				);
				break;
			}

			CTopLevelWindow_UpdateWindowVisuals_Instructions += 1;
			i--;
		} while (i);
	}
	FAIL_FAST_IF_FAILED_MSG(g_instructionsToReplace.size() == 2 ? S_OK : E_NOINTERFACE, "Unable to locate the complete frame-demodernizer instruction patch set");
	g_instructionPatches.clear();
	g_instructionPatches.reserve(g_instructionsToReplace.size());
	for (const auto& [address, instructions] : g_instructionsToReplace)
	{
		const auto backup = g_instructionsBackup.find(address);
		FAIL_FAST_IF_FAILED_MSG(backup != g_instructionsBackup.end() ? S_OK : E_UNEXPECTED, "Missing captured instruction bytes at %p", address);
		auto& patch = g_instructionPatches.emplace_back();
		patch.Prepare(
			address,
			backup->second,
			instructions
		);
	}
	for (auto& patch : g_instructionPatches)
	{
		HookHelper::GetCurrentHookTransaction().Apply(patch);
	}
#else
	PrepareForcedHighContrastPatches();
	ApplyForcedHighContrastPatches();
#endif

	HookHelper::ApplyInlineHooks(
		std::initializer_list<HookHelper::DetourInfo>
		{
			{ &g_CTopLevelWindow_ValidateVisual_Org },
			{ &g_CTopLevelWindow_UpdateNCAreaBackground_Org },
			{ &g_SetMargin_Org },
		}
	);

#if defined(_M_ARM64)
	if (
		uDWM::g_versionInfo.build >= os::build_w11_24h2 &&
		(GlassEngine::GetDwordFromRegistry(L"DemodernizerForcedCalls", 23) & 8)
	)
	{
		const auto visuals = uDWM::Symbol_CTopLevelWindow_UpdateWindowVisuals_ForcedHighContrast.get();
		if (visuals)
		{
			g_CTopLevelWindow_UpdateWindowVisuals_Org =
				reinterpret_cast<CTopLevelWindow_UpdateWindowVisuals_t>(visuals);
			HookHelper::ApplyInlineHooks(
				std::initializer_list<HookHelper::DetourInfo>
				{
					{ &g_CTopLevelWindow_UpdateWindowVisuals_Org, &MyCTopLevelWindow_UpdateWindowVisuals },
				}
			);
			StartupTrace::Write("patch: hooked UpdateWindowVisuals at %p", static_cast<void*>(visuals));
		}
		else
		{
			StartupTrace::Write("patch: UpdateWindowVisuals anchor is unresolved, skipping the backdrop override");
		}
	}
#endif
}

void GlassFrameDemodernizer::Shutdown()
{
	if (uDWM::g_versionInfo.build < os::build_w11_21h2)
	{
		return;
	}

	HookHelper::ApplyInlineHooks(
		std::initializer_list<HookHelper::DetourInfo>
		{
			{ &g_CTopLevelWindow_ValidateVisual_Org },
			{ &g_CTopLevelWindow_UpdateNCAreaBackground_Org },
			{ &g_SetMargin_Org },
		}
	);

#if defined(_M_ARM64)
	if (g_CTopLevelWindow_UpdateWindowVisuals_Org)
	{
		HookHelper::ApplyInlineHooks(
			std::initializer_list<HookHelper::DetourInfo>
			{
				{ &g_CTopLevelWindow_UpdateWindowVisuals_Org, &MyCTopLevelWindow_UpdateWindowVisuals },
			}
		);
		g_CTopLevelWindow_UpdateWindowVisuals_Org = nullptr;
	}
#endif

#if !defined(_M_ARM64)
	for (auto& patch : g_instructionPatches)
	{
		HookHelper::GetCurrentHookTransaction().Apply(patch);
	}
#else
	ApplyForcedHighContrastPatches();
#endif
}

void GlassFrameDemodernizer::Cleanup()
{
	g_instructionsBackup.clear();
	g_instructionsToReplace.clear();
	g_instructionPatches.clear();
	g_atlasedImageNullSentinel.clear();
}
