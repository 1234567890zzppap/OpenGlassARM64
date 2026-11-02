#include "pch.h"
#include "TestHarness.hpp"
#include "CaptionText.hpp"

using namespace OpenGlass;

namespace OpenGlassTests
{
	void TestDWriteTrimming()
	{
		winrt::com_ptr<IDWriteFactory> factory;
		winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.put())));
		winrt::com_ptr<IDWriteTextFormat> format;
		winrt::check_hresult(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.f, L"en-US", format.put()));
		winrt::check_hresult(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
		winrt::com_ptr<IDWriteInlineObject> ellipsis;
		winrt::check_hresult(factory->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()));
		const DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
		winrt::check_hresult(format->SetTrimming(&trimming, ellipsis.get()));

		for (const auto direction : { DWRITE_READING_DIRECTION_LEFT_TO_RIGHT, DWRITE_READING_DIRECTION_RIGHT_TO_LEFT })
		{
			winrt::check_hresult(format->SetReadingDirection(direction));
			for (const auto text : { L"", L"Window title", L"Line one\n Line two", L"Line one\r\nLine two", L"\n", L"Line one\nLine two\n" })
			{
				winrt::com_ptr<IDWriteTextLayout> layout;
				winrt::check_hresult(factory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.get(), 1000.f, 100.f, layout.put()));
				Check(!CaptionText::IsDWriteTextTrimmed(layout.get()));
			}

			for (const auto text : { L"A long title that needs trimming", L"I\nA long title that needs trimming", L"A long title that needs trimming\nI" })
			{
				winrt::com_ptr<IDWriteTextLayout> layout;
				winrt::check_hresult(factory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.get(), 60.f, 100.f, layout.put()));
				Check(CaptionText::IsDWriteTextTrimmed(layout.get()));
				winrt::check_hresult(layout->SetMaxWidth(1000.f));
				Check(!CaptionText::IsDWriteTextTrimmed(layout.get()));
			}
		}
	}

	void TestDWriteGeometry()
	{
		winrt::com_ptr<IDWriteFactory> factory;
		winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.put())));
		winrt::com_ptr<IDWriteTextFormat> format;
		winrt::check_hresult(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.f, L"en-US", format.put()));
		winrt::check_hresult(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
		for (const auto direction : { DWRITE_READING_DIRECTION_LEFT_TO_RIGHT, DWRITE_READING_DIRECTION_RIGHT_TO_LEFT })
		{
			winrt::check_hresult(format->SetReadingDirection(direction));
			float singleLineHeight{};
			for (const auto text : { L"Window title \x05D0\x05D1", L"Window title \x05D0\x05D1\nSecond line" })
			{
				float leadingWidth{};
				for (const auto alignment : { DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_TRAILING })
				{
					winrt::check_hresult(format->SetTextAlignment(alignment));
					winrt::com_ptr<IDWriteTextLayout> layout;
					winrt::check_hresult(factory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format.get(), 10000.f, 100.f, layout.put()));
					DWRITE_TEXT_METRICS metrics{};
					DWRITE_OVERHANG_METRICS overhangs{};
					winrt::check_hresult(layout->GetMetrics(&metrics));
					winrt::check_hresult(layout->GetOverhangMetrics(&overhangs));
					const auto bounds = CaptionText::CalculateDWriteBitmapBounds(metrics, overhangs);
					const auto width = bounds.right - bounds.left;
					const auto height = bounds.bottom - bounds.top;
					Check(width > 0.f && width < 1000.f);
					Check(height > 0.f);
					if (wcschr(text, L'\n')) Check(height > singleLineHeight);
					else singleLineHeight = height;
					if (alignment == DWRITE_TEXT_ALIGNMENT_LEADING) leadingWidth = width;
					Check(std::fabs(width - leadingWidth) <= 2.f);

					// Normalizing the bitmap must preserve the layout origin, including
					// BeginDraw's nonzero atlas offset and fractional coordinates.
					const D2D1_POINT_2F origin{ 143.25f, 79.5f };
					const D2D1_POINT_2F bitmapOrigin{ origin.x + bounds.left, origin.y + bounds.top };
					Check(bitmapOrigin.x - bounds.left == origin.x);
					Check(bitmapOrigin.y - bounds.top == origin.y);
				}
			}
		}
	}
}
