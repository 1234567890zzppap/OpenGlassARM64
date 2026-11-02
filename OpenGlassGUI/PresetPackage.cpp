#include "pch.h"
#include "ManagedFiles.hpp"
#include "ApplicationPaths.hpp"
#include "PngAssetValidation.hpp"
#include "PresetPackage.hpp"
#include "ColorizationPresets.hpp"
#include "ThemeAtlasLayout.hpp"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace OpenGlass::PresetPackages
{
	namespace
	{
		constexpr std::size_t MaximumEntryCount = 32;
		constexpr std::size_t MaximumEntrySize = 64ull * 1024 * 1024;
		constexpr std::size_t MaximumTotalSize = 128ull * 1024 * 1024;
		constexpr std::size_t MaximumMetadataSize = 1024ull * 1024;
		constexpr std::size_t MaximumCompressionRatio = 200;
		constexpr std::size_t MaximumEntryPathLength = 240;
		constexpr std::size_t MaximumReportedIgnoredSettings = 64;
		constexpr std::size_t MaximumReportedSettingNameLength = 160;

		void CheckCancelled(std::stop_token stop)
		{
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), stop.stop_requested());
		}

		std::wstring DisplaySettingName(std::wstring_view value)
		{
			std::wstring result;
			for (const auto character : value)
			{
				if (result.size() >= MaximumReportedSettingNameLength)
				{
					result += L"...";
					break;
				}
				if (character < L' ' || character == 0x7f)
				{
					result += std::format(L"\\u{:04X}", static_cast<unsigned>(character));
				}
				else
				{
					result += character;
				}
			}
			return result.empty() ? L"<empty name>" : result;
		}

		std::string ToUtf8(std::wstring_view value)
		{
			if (value.empty())
			{
				return {};
			}
			const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			THROW_LAST_ERROR_IF(size <= 0);
			std::string result(size, '\0');
			THROW_LAST_ERROR_IF(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) <= 0);
			return result;
		}

		std::wstring FromUtf8(std::string_view value)
		{
			if (value.empty())
			{
				return {};
			}
			const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION), size <= 0);
			std::wstring result(size, L'\0');
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION), MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) <= 0);
			return result;
		}

		std::filesystem::path PathFromUtf8(std::string_view value)
		{
			return std::filesystem::path(FromUtf8(value));
		}

		std::string Hex(std::span<const std::byte> bytes)
		{
			constexpr char digits[] = "0123456789abcdef";
			std::string result(bytes.size() * 2, '\0');
			for (std::size_t index = 0; index < bytes.size(); ++index)
			{
				const auto value = std::to_integer<unsigned char>(bytes[index]);
				result[index * 2] = digits[value >> 4];
				result[index * 2 + 1] = digits[value & 0xf];
			}
			return result;
		}

		std::string Sha256(std::span<const std::byte> bytes)
		{
			std::array<std::byte, 32> digest{};
			THROW_IF_NTSTATUS_FAILED(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
				reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()),
				reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size())));
			return Hex(digest);
		}

		bool HasInvalidMetadataCharacters(std::wstring_view value, bool multiline)
		{
			return std::ranges::any_of(value, [multiline](wchar_t character)
			{
				if (character == 0x7f) return true;
				if (character >= 0x20) return false;
				return !multiline || (character != L'\r' && character != L'\n' && character != L'\t');
			});
		}

		bool IsCanonicalUuid(const std::string& value)
		{
			if (value.size() != 36)
			{
				return false;
			}
			for (std::size_t index = 0; index < value.size(); ++index)
			{
				if (index == 8 || index == 13 || index == 18 || index == 23)
				{
					if (value[index] != '-') return false;
				}
				else if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f')))
				{
					return false;
				}
			}
			return true;
		}

		std::string CreateUuid()
		{
			GUID guid{};
			THROW_IF_FAILED(CoCreateGuid(&guid));
			char text[37]{};
			std::snprintf(
				text,
				sizeof(text),
				"%08lx-%04x-%04x-%04x-%012llx",
				guid.Data1,
				guid.Data2,
				guid.Data3,
				(static_cast<unsigned>(guid.Data4[0]) << 8) | guid.Data4[1],
				(static_cast<unsigned long long>(guid.Data4[2]) << 40)
					| (static_cast<unsigned long long>(guid.Data4[3]) << 32)
					| (static_cast<unsigned long long>(guid.Data4[4]) << 24)
					| (static_cast<unsigned long long>(guid.Data4[5]) << 16)
					| (static_cast<unsigned long long>(guid.Data4[6]) << 8)
					| guid.Data4[7]
			);
			return text;
		}

		bool IsSafeEntryPath(std::string_view value)
		{
			if (
				value.empty()
				|| value.size() > MaximumEntryPathLength
				|| value.find('\0') != std::string_view::npos
				|| value.front() == '/'
				|| value.front() == '\\'
				|| value.find('\\') != std::string_view::npos
				|| value.find(':') != std::string_view::npos
			)
			{
				return false;
			}
			std::filesystem::path path = PathFromUtf8(value);
			if (path.is_absolute() || path.has_root_path())
			{
				return false;
			}
			for (const auto& component : path)
			{
				const auto text = component.native();
				if (component == L".." || component == L"." || text.empty() || text.ends_with(L'.') || text.ends_with(L' '))
				{
					return false;
				}
			}
			return true;
		}

		using EntryMap = std::map<std::string, std::vector<std::byte>, std::less<>>;

		bool IsReparsePoint(const std::filesystem::path& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			THROW_LAST_ERROR_IF(attributes == INVALID_FILE_ATTRIBUTES);
			return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		}

		EntryMap ReadArchiveEntries(const std::filesystem::path& path, std::stop_token stop)
		{
			CheckCancelled(stop);
			wxFFileInputStream input(path.wstring());
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), !input.IsOk());
			wxZipInputStream zip(input);
			EntryMap entries;
			std::set<std::string, std::less<>> names;
			std::size_t total{};
			while (const auto entry = std::unique_ptr<wxZipEntry>(zip.GetNextEntry()))
			{
				CheckCancelled(stop);
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), entry->IsDir() || (entry->GetFlags() & 1) != 0);
				const auto unixType = (entry->GetExternalAttributes() >> 16) & 0170000;
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), unixType != 0 && unixType != 0100000);
				const auto name = entry->GetName(wxPATH_UNIX).ToStdString(wxConvUTF8);
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), !IsSafeEntryPath(name));
				std::string folded = name;
				std::ranges::transform(folded, folded.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_DUP_NAME), !names.emplace(std::move(folded)).second);
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES), entries.size() >= MaximumEntryCount);
				const auto announcedSize = entry->GetSize();
				const auto compressedSize = entry->GetCompressedSize();
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), announcedSize != wxInvalidOffset && static_cast<std::uint64_t>(announcedSize) > MaximumEntrySize);
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID),
					(announcedSize > 0 && compressedSize == 0)
					|| (announcedSize > 0 && compressedSize > 0 && static_cast<std::uint64_t>(announcedSize) > static_cast<std::uint64_t>(compressedSize) * MaximumCompressionRatio)
				);
				std::vector<std::byte> bytes;
				if (announcedSize > 0 && announcedSize != wxInvalidOffset) bytes.reserve(static_cast<std::size_t>(announcedSize));
				std::array<std::byte, 64 * 1024> buffer{};
				for (;;)
				{
					CheckCancelled(stop);
					zip.Read(buffer.data(), buffer.size());
					const auto count = zip.LastRead();
					THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), bytes.size() + count > MaximumEntrySize || total + bytes.size() + count > MaximumTotalSize);
					bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
					if (count == 0 || zip.GetLastError() == wxSTREAM_EOF) break;
					THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), zip.GetLastError() != wxSTREAM_NO_ERROR);
				}
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), announcedSize != wxInvalidOffset && static_cast<std::uint64_t>(announcedSize) != bytes.size());
				total += bytes.size();
				entries.emplace(name, std::move(bytes));
			}
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), entries.empty() || (zip.GetLastError() != wxSTREAM_EOF && zip.GetLastError() != wxSTREAM_NO_ERROR));
			return entries;
		}

		std::string BytesToString(const std::vector<std::byte>& bytes)
		{
			return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
		}

		void ValidateLicense(const std::string& text)
		{
			THROW_HR_IF(E_INVALIDARG, text.empty() || text.size() > MaximumMetadataSize || text.find('\0') != std::string::npos);
			const auto wide = FromUtf8(text);
			THROW_HR_IF(E_INVALIDARG,
				std::ranges::all_of(wide, [](wchar_t c) { return iswspace(c); })
				|| std::ranges::any_of(wide, [](wchar_t c) { return c < 0x20 && c != L'\r' && c != L'\n' && c != L'\t'; })
			);
		}

		std::string ExpectedAssetPath(Settings::AssetRole role)
		{
			switch (role)
			{
			case Settings::AssetRole::ThemeAtlas: return "assets/theme-atlas.png";
			case Settings::AssetRole::Reflection: return "assets/reflection.png";
			case Settings::AssetRole::Material: return "assets/material.png";
			default: THROW_HR(E_INVALIDARG);
			}
		}

		wil::com_ptr<IWICImagingFactory> CreateWicFactory()
		{
			wil::com_ptr<IWICImagingFactory> factory;
			THROW_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
			return factory;
		}

		void ValidateImageWithWic(std::span<const std::byte> bytes)
		{
			PngAssetValidation::ImageInfo expectedInfo{};
			THROW_IF_FAILED(PngAssetValidation::ValidateStructure(bytes, expectedInfo));
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT), bytes.empty() || bytes.size() > MAXDWORD);
			const auto initialization = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			THROW_HR_IF(initialization, FAILED(initialization) && initialization != RPC_E_CHANGED_MODE);
			const auto uninitialize = wil::scope_exit([&] { if (SUCCEEDED(initialization)) CoUninitialize(); });
			auto factory = CreateWicFactory();
			wil::com_ptr<IWICStream> stream;
			THROW_IF_FAILED(factory->CreateStream(&stream));
			THROW_IF_FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())), static_cast<DWORD>(bytes.size())));
			wil::com_ptr<IWICFormatConverter> converter;
			THROW_IF_FAILED(PngAssetValidation::CreateValidatedWicSource(
				factory.get(),
				stream.get(),
				&expectedInfo,
				&converter
			));
		}

		void ValidateThemeAtlasLayout(std::span<const std::byte> bytes)
		{
			ThemeAtlasLayout::Document document;
			THROW_IF_FAILED(ThemeAtlasLayout::Parse(bytes, document));
		}

		std::string CalculateContentDigest(const std::string& manifest, const std::string& license, const std::map<std::string, std::vector<std::byte>>& assets)
		{
			BCRYPT_HASH_HANDLE hash{};
			THROW_IF_NTSTATUS_FAILED(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0));
			const auto destroyHash = wil::scope_exit([&] { BCryptDestroyHash(hash); });
			auto update = [&](std::span<const std::byte> bytes)
			{
				THROW_IF_NTSTATUS_FAILED(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0));
			};
			auto append = [&](std::string_view name, std::span<const std::byte> bytes)
			{
				// Preserve the existing name/uint64 length/content framing without
				// allocating another buffer containing all package assets.
				const auto size = static_cast<std::uint64_t>(bytes.size());
				update({ reinterpret_cast<const std::byte*>(name.data()), name.size() });
				update({ reinterpret_cast<const std::byte*>(&size), sizeof(size) });
				update(bytes);
			};
			append("manifest.json", { reinterpret_cast<const std::byte*>(manifest.data()), manifest.size() });
			if (!license.empty())
			{
				append("LICENSE", { reinterpret_cast<const std::byte*>(license.data()), license.size() });
			}
			for (const auto& [name, bytes] : assets)
			{
				append(name, bytes);
			}
			std::array<std::byte, 32> digest{};
			THROW_IF_NTSTATUS_FAILED(BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0));
			return Hex(digest);
		}

		void NormalizeLegacyColors(Package& package)
		{
			using enum Settings::Id;
			auto dword = [&](Settings::Id id) -> std::optional<DWORD>
			{
				const auto it = package.settings.find(id);
				if (it != package.settings.end()) if (const auto value = std::get_if<DWORD>(&it->second)) return *value;
				return {};
			};
			const auto color = dword(ColorizationColor);
			const auto afterglow = dword(ColorizationAfterglow);
			if (color && !package.accentColor)
			{
				package.accentColor = *color & 0xFFFFFF;
				package.conversions.push_back(L"ColorizationColor RGB -> accent color");
			}
			if (afterglow && (!color || (*afterglow & 0xFFFFFF) != (*color & 0xFFFFFF)))
			{
				++package.ignoredSettingCount;
				if (package.ignoredSettingNames.size() < MaximumReportedIgnoredSettings) package.ignoredSettingNames.push_back(L"ColorizationAfterglow (independent RGB is no longer applied)");
			}
			for (const auto base : { ColorizationColorBalance, ColorizationAfterglowBalance, ColorizationBlurBalance })
			{
				const auto overrideId = Settings::Find(std::wstring(Settings::Get(base).name) + L"Override")->id;
				if (const auto value = dword(base); value && !dword(overrideId))
				{
					package.settings[overrideId] = *value;
					package.conversions.push_back(std::wstring(Settings::Get(base).name) + L" -> Override");
				}
			}
			if (!dword(GlassOpacity) && dword(GlassType).value_or(0) == 1 && color)
			{
				package.settings[GlassOpacity] = ColorizationPresets::CalculateVistaOpacity(*color);
				package.conversions.push_back(L"Legacy color alpha -> missing GlassOpacity");
			}
			for (const auto& spec : Settings::Catalog)
				if (Settings::IsWindowsColorBase(spec.id)) package.settings.erase(spec.id);
		}

		Package ParsePackage(EntryMap entries, const std::filesystem::path& source, bool deployed, std::stop_token stop)
		{
			CheckCancelled(stop);
			const auto manifestIt = entries.find("manifest.json");
			THROW_HR_IF(E_INVALIDARG, manifestIt == entries.end());
			Package package;
			package.source = source;
			package.deployed = deployed;
			package.manifestText = BytesToString(manifestIt->second);

			THROW_HR_IF(E_INVALIDARG, package.manifestText.size() > MaximumMetadataSize || package.manifestText.find('\0') != std::string::npos);
			bool duplicateJsonKey{};
			std::vector<std::set<std::string>> objectKeys;
			const auto manifest = nlohmann::json::parse(package.manifestText, [&objectKeys, &duplicateJsonKey](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed)
			{
				if (event == nlohmann::json::parse_event_t::object_start) objectKeys.emplace_back();
				else if (event == nlohmann::json::parse_event_t::key)
				{
					if (objectKeys.empty() || !objectKeys.back().emplace(parsed.get<std::string>()).second) duplicateJsonKey = true;
				}
				else if (event == nlohmann::json::parse_event_t::object_end && !objectKeys.empty()) objectKeys.pop_back();
				return true;
			});
			THROW_HR_IF(E_INVALIDARG, duplicateJsonKey || !manifest.is_object());
			const auto schemaVersion = manifest.value("schema_version", 0u);
			THROW_HR_IF(E_INVALIDARG, schemaVersion != 1 && schemaVersion != 2 && schemaVersion != 3);
			package.catalogVersion = manifest.value("catalog_version", 0u);
			THROW_HR_IF(E_INVALIDARG, package.catalogVersion == 0 || (schemaVersion >= 3 && package.catalogVersion < 2));
			package.metadata.uuid = manifest.at("uuid").get<std::string>();
			THROW_HR_IF(E_INVALIDARG, !IsCanonicalUuid(package.metadata.uuid));
			package.metadata.name = FromUtf8(manifest.at("name").get<std::string>());
			package.metadata.description = FromUtf8(manifest.at("description").get<std::string>());
			const auto& author = manifest.at("author");
			THROW_HR_IF(E_INVALIDARG, !author.is_object() || author.size() != 2);
			package.metadata.authorName = FromUtf8(author.at("name").get<std::string>());
			package.metadata.authorHomepage = FromUtf8(author.at("homepage").get<std::string>());
			const auto& license = manifest.at("license");
			const auto licenseIt = entries.find("LICENSE");
			if (schemaVersion == 1)
			{
				THROW_HR_IF(E_INVALIDARG, !license.is_object() || (license.size() != 2 && license.size() != 3));
				package.metadata.licenseName = FromUtf8(license.at("name").get<std::string>());
				THROW_HR_IF(E_INVALIDARG, license.at("file").get<std::string>() != "LICENSE" || licenseIt == entries.end());
				if (license.contains("homepage"))
				{
					THROW_HR_IF(E_INVALIDARG, !IsValidHomepageUrl(FromUtf8(license.at("homepage").get<std::string>())));
				}
				package.licenseText = BytesToString(licenseIt->second);
				ValidateLicense(package.licenseText);
			}
			else if (license.is_null())
			{
				THROW_HR_IF(E_INVALIDARG, licenseIt != entries.end());
			}
			else
			{
				THROW_HR_IF(E_INVALIDARG, !license.is_object() || license.size() != 1 || license.at("file").get<std::string>() != "LICENSE" || licenseIt == entries.end());
				package.licenseText = BytesToString(licenseIt->second);
				ValidateLicense(package.licenseText);
				package.metadata.licenseName = InferLicenseName(package.licenseText);
			}
			package.legacyLicense = schemaVersion < 3;
			if (schemaVersion == 3)
			{
				const auto& rights = manifest.at("rights");
				THROW_HR_IF(E_INVALIDARG, !rights.is_object() || (rights.size() != 3 && !(rights.size() == 4 && rights.contains("inherited_licenses"))));
				const auto policy = rights.at("configuration").get<std::string>();
				THROW_HR_IF(E_INVALIDARG, policy != "openglass-attribution-v1" && policy != "legacy-package");
				package.legacyLicense = policy == "legacy-package";
				THROW_HR_IF(E_INVALIDARG, rights.at("license_scope") != (package.legacyLicense ? "package" : "image-assets"));
				const auto& sources = rights.at("sources");
				THROW_HR_IF(E_INVALIDARG, !sources.is_array() || sources.size() > 256);
				for (const auto& attribution : sources)
				{
					auto text = FromUtf8(attribution.get<std::string>());
					THROW_HR_IF(E_INVALIDARG, text.empty() || text.size() > 4096 || HasInvalidMetadataCharacters(text, false));
					package.attribution.push_back(std::move(text));
				}
				if (rights.contains("inherited_licenses"))
				{
					const auto& licenses = rights.at("inherited_licenses");
					THROW_HR_IF(E_INVALIDARG, !licenses.is_array() || licenses.size() > 256);
					for (const auto& item : licenses)
					{
						auto text = item.get<std::string>();
						ValidateLicense(text);
						THROW_HR_IF(E_INVALIDARG, package.licenseText.find(text) == std::string::npos);
						package.inheritedLicenses.push_back(std::move(text));
					}
				}
			}

			THROW_HR_IF(E_INVALIDARG,
				package.metadata.name.empty()
				|| package.metadata.name.size() > 128
				|| HasInvalidMetadataCharacters(package.metadata.name, false)
				|| package.metadata.description.size() > 4096
				|| HasInvalidMetadataCharacters(package.metadata.description, true)
				|| package.metadata.authorName.empty()
				|| package.metadata.authorName.size() > 256
				|| HasInvalidMetadataCharacters(package.metadata.authorName, false)
				|| package.metadata.licenseName.size() > 256
				|| (!package.metadata.licenseName.empty() && HasInvalidMetadataCharacters(package.metadata.licenseName, false))
				|| (!(schemaVersion >= 3 && package.metadata.authorHomepage.empty()) && !IsValidHomepageUrl(package.metadata.authorHomepage))
			);

			if (schemaVersion >= 3 && manifest.contains("accent_color") && !manifest.at("accent_color").is_null())
			{
				const auto& color = manifest.at("accent_color");
				THROW_HR_IF(E_INVALIDARG, !color.is_object() || color.size() != 1);
				const auto rgb = color.at("rgb").get<std::string>();
				THROW_HR_IF(E_INVALIDARG, rgb.size() != 7 || rgb.front() != '#');
				DWORD value{};
				for (const auto c : std::string_view(rgb).substr(1))
				{
					const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
					THROW_HR_IF(E_INVALIDARG, digit < 0);
					value = (value << 4) | digit;
				}
				package.accentColor = value;
			}
			auto inputSetting = [&](const Settings::Spec& spec)
			{
				return schemaVersion < 3 ? spec.includeInPresetPacks && spec.introducedIn <= package.catalogVersion
					: Settings::IsPresetPackSetting(spec, package.catalogVersion) && !Settings::IsWindowsColorBase(spec.id);
			};
			const auto& settings = manifest.at("settings");
			THROW_HR_IF(E_INVALIDARG, !settings.is_object());
			for (const auto& [name, _] : settings.items())
			{
				const auto decodedName = FromUtf8(name);
				const auto spec = Settings::Find(decodedName);
				if (spec && inputSetting(*spec)) continue;
				++package.ignoredSettingCount;
				if (package.ignoredSettingNames.size() < MaximumReportedIgnoredSettings)
				{
					package.ignoredSettingNames.push_back(DisplaySettingName(decodedName));
				}
			}
			for (const auto& spec : Settings::Catalog)
			{
				if (!inputSetting(spec)) continue;
				const auto name = ToUtf8(spec.name);
				THROW_HR_IF(E_INVALIDARG, !settings.contains(name));
				const auto& value = settings.at(name);
				if ((schemaVersion < 3 && value.is_null()) || (schemaVersion == 3 && value.is_object() && value.size() == 1 && value.value("state", std::string{}) == "default"))
				{
					package.settings.emplace(spec.id, std::monostate{});
				}
				else if (spec.type == Settings::ValueType::Dword)
				{
					THROW_HR_IF(E_INVALIDARG, !value.is_number_unsigned() || value.get<std::uint64_t>() > MAXDWORD);
					package.settings.emplace(spec.id, static_cast<DWORD>(value.get<std::uint64_t>()));
				}
				else
				{
					THROW_HR_IF(E_INVALIDARG, spec.assetRole == Settings::AssetRole::None || !value.is_object() || !value.contains("asset"));
					const auto assetPath = value.at("asset").get<std::string>();
					THROW_HR_IF(E_INVALIDARG, assetPath != ExpectedAssetPath(spec.assetRole));
					package.settings.emplace(spec.id, AssetReference{ assetPath });
				}
			}

			const auto& assetSpecs = manifest.at("assets");
			THROW_HR_IF(E_INVALIDARG, !assetSpecs.is_object());
			for (const auto& [path, spec] : assetSpecs.items())
			{
				CheckCancelled(stop);
				const bool knownPath = path == "assets/theme-atlas.png"
					|| path == "assets/theme-atlas.png.layout"
					|| path == "assets/reflection.png"
					|| path == "assets/material.png";
				THROW_HR_IF(E_INVALIDARG, !knownPath || !spec.is_object() || spec.size() != 2);
				const auto entry = entries.find(path);
				THROW_HR_IF(E_INVALIDARG, entry == entries.end());
				THROW_HR_IF(E_INVALIDARG, spec.at("size").get<std::uint64_t>() != entry->second.size());
				THROW_HR_IF(E_INVALIDARG, spec.at("sha256").get<std::string>() != Sha256(entry->second));
				package.assets.emplace(path, std::move(entry->second));
			}
			for (const auto& [path, bytes] : package.assets)
			{
				CheckCancelled(stop);
				if (path.ends_with(".layout"))
				{
					ValidateThemeAtlasLayout(bytes);
				}
				else ValidateImageWithWic(bytes);
				package.assetSummary.emplace_back(path, bytes.size());
			}
			for (const auto& [_, value] : package.settings)
			{
				if (const auto asset = std::get_if<AssetReference>(&value))
				{
					THROW_HR_IF(E_INVALIDARG, !package.assets.contains(asset->path));
				}
			}
			THROW_HR_IF(E_INVALIDARG, package.assets.contains("assets/theme-atlas.png.layout") && !package.assets.contains("assets/theme-atlas.png"));
			THROW_HR_IF(E_INVALIDARG, entries.size() != package.assets.size() + 1 + (package.licenseText.empty() ? 0 : 1));
			CheckCancelled(stop);
			package.digest = CalculateContentDigest(manifest.dump(), package.licenseText, package.assets);
			if (schemaVersion < 3) NormalizeLegacyColors(package);
			if (schemaVersion < 3 && std::ranges::any_of(package.settings, [](const auto& item) { return std::holds_alternative<std::monostate>(item.second); }))
				package.conversions.push_back(L"Legacy null values mean no customization in the target scope; inherited values in other scopes remain effective.");
			return package;
		}

		EntryMap ReadDeployedEntries(const std::filesystem::path& directory, std::stop_token stop)
		{
			CheckCancelled(stop);
			THROW_HR_IF(E_INVALIDARG, !std::filesystem::is_directory(directory) || IsReparsePoint(directory));
			EntryMap entries;
			std::set<std::string, std::less<>> names;
			std::size_t total{};
			for (const auto& path : std::filesystem::recursive_directory_iterator(directory))
			{
				CheckCancelled(stop);
				THROW_HR_IF(E_INVALIDARG, IsReparsePoint(path.path()));
				if (path.is_directory()) continue;
				THROW_HR_IF(E_INVALIDARG, !path.is_regular_file() || entries.size() >= MaximumEntryCount);
				const auto relative = ToUtf8(std::filesystem::relative(path.path(), directory).generic_wstring());
				if (relative == ".accepted.json") continue;
				THROW_HR_IF(E_INVALIDARG, !IsSafeEntryPath(relative));
				std::string folded = relative;
				std::ranges::transform(folded, folded.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_DUP_NAME), !names.emplace(std::move(folded)).second);
				const auto size = path.file_size();
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size > MaximumEntrySize || total + size > MaximumTotalSize);
				std::ifstream stream(path.path(), std::ios::binary);
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_READ_FAULT), !stream);
				std::vector<std::byte> bytes(static_cast<std::size_t>(size));
				stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_READ_FAULT), !stream || stream.peek() != std::char_traits<char>::eof());
				entries.emplace(relative, std::move(bytes));
				total += size;
			}
			return entries;
		}

		std::string BuildManifest(const CreateRequest& request, const std::map<std::string, std::vector<std::byte>>& assets)
		{
			nlohmann::ordered_json manifest;
			manifest["schema_version"] = 3;
			manifest["rights"] = { { "configuration", request.legacyLicense ? "legacy-package" : "openglass-attribution-v1" },
				{ "license_scope", request.legacyLicense ? "package" : "image-assets" }, { "sources", nlohmann::ordered_json::array() } };
			for (const auto& source : request.attribution) manifest["rights"]["sources"].push_back(ToUtf8(source));
			if (!request.inheritedLicenses.empty()) manifest["rights"]["inherited_licenses"] = request.inheritedLicenses;
			THROW_HR_IF(E_INVALIDARG, request.accentColor && *request.accentColor > 0xFFFFFF);
			manifest["accent_color"] = request.accentColor
				? nlohmann::ordered_json{ { "rgb", std::format("#{:06X}", *request.accentColor) } }
				: nlohmann::ordered_json(nullptr);
			manifest["catalog_version"] = Settings::CatalogVersion;
			manifest["uuid"] = request.metadata.uuid;
			manifest["name"] = ToUtf8(request.metadata.name);
			manifest["description"] = ToUtf8(request.metadata.description);
			manifest["author"] = { { "name", ToUtf8(request.metadata.authorName) }, { "homepage", ToUtf8(request.metadata.authorHomepage) } };
			manifest["license"] = request.licenseText.empty()
				? nlohmann::ordered_json(nullptr)
				: nlohmann::ordered_json{ { "file", "LICENSE" } };
			for (const auto& spec : Settings::Catalog)
			{
				if (!Settings::IsPresetPackSetting(spec)) continue;
				const auto value = request.settings.find(spec.id);
				THROW_HR_IF(E_INVALIDARG, value == request.settings.end());
				auto& output = manifest["settings"][ToUtf8(spec.name)];
				if (std::holds_alternative<std::monostate>(value->second)) output = { { "state", "default" } };
				else if (const auto dword = std::get_if<DWORD>(&value->second)) output = *dword;
				else if (const auto asset = std::get_if<AssetReference>(&value->second))
				{
					THROW_HR_IF(E_INVALIDARG, asset->path != ExpectedAssetPath(spec.assetRole) || !assets.contains(asset->path));
					output = { { "asset", asset->path } };
				}
				else THROW_HR(E_INVALIDARG);
			}
			manifest["assets"] = nlohmann::ordered_json::object();
			for (const auto& [path, bytes] : assets)
			{
				const bool knownPath = path == "assets/theme-atlas.png"
					|| path == "assets/theme-atlas.png.layout"
					|| path == "assets/reflection.png"
					|| path == "assets/material.png";
				THROW_HR_IF(E_INVALIDARG, !knownPath || (path == "assets/theme-atlas.png.layout" && !assets.contains("assets/theme-atlas.png")));
				manifest["assets"][path] = { { "size", bytes.size() }, { "sha256", Sha256(bytes) } };
			}
			return manifest.dump(2) + "\n";
		}
	}

	std::wstring InferLicenseName(std::string_view licenseText)
	{
		if (licenseText.empty()) return {};
		auto text = FromUtf8(licenseText);
		if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());
		auto lower = text;
		std::ranges::transform(lower, lower.begin(), [](wchar_t value) { return static_cast<wchar_t>(::towlower(value)); });
		const auto header = std::wstring_view(lower).substr(0, std::min<std::size_t>(lower.size(), 4096));

		const auto trim = [](std::wstring_view value)
		{
			while (!value.empty() && iswspace(value.front())) value.remove_prefix(1);
			while (!value.empty() && iswspace(value.back())) value.remove_suffix(1);
			return value;
		};
		constexpr std::wstring_view spdxPrefix = L"spdx-license-identifier:";
		if (const auto position = header.find(spdxPrefix); position != std::wstring::npos)
		{
			const auto start = position + spdxPrefix.size();
			const auto end = text.find_first_of(L"\r\n", start);
			const auto identifier = trim(std::wstring_view(text).substr(start, end == std::wstring::npos ? text.size() - start : end - start));
			if (!identifier.empty() && identifier.size() <= 128) return std::wstring(identifier);
		}

		struct KnownLicense
		{
			std::wstring_view marker;
			std::wstring_view name;
		};
		constexpr KnownLicense known[]
		{
			{ L"mozilla public license version 2.0", L"MPL-2.0" },
			{ L"gnu lesser general public license", L"LGPL" },
			{ L"gnu affero general public license", L"AGPL" },
			{ L"gnu general public license", L"GPL" },
			{ L"apache license\nversion 2.0", L"Apache-2.0" },
			{ L"apache license\r\nversion 2.0", L"Apache-2.0" },
			{ L"boost software license - version 1.0", L"BSL-1.0" },
			{ L"bsd 3-clause license", L"BSD-3-Clause" },
			{ L"bsd 2-clause license", L"BSD-2-Clause" },
			{ L"isc license", L"ISC" },
			{ L"mit license", L"MIT" },
			{ L"the unlicense", L"Unlicense" }
		};
		for (const auto& candidate : known)
		{
			if (header.find(candidate.marker) != std::wstring::npos)
			{
				std::wstring name(candidate.name);
				if ((name == L"GPL" || name == L"LGPL" || name == L"AGPL") && header.find(L"version 3") != std::wstring::npos) name += L"-3.0";
				else if ((name == L"GPL" || name == L"LGPL") && header.find(L"version 2.1") != std::wstring::npos) name += L"-2.1";
				else if ((name == L"GPL" || name == L"LGPL") && header.find(L"version 2") != std::wstring::npos) name += L"-2.0";
				return name;
			}
		}

		std::wstring_view remaining(text);
		while (!remaining.empty())
		{
			const auto end = remaining.find_first_of(L"\r\n");
			const auto line = trim(remaining.substr(0, end));
			if (!line.empty())
			{
				auto lineLower = std::wstring(line);
				std::ranges::transform(lineLower, lineLower.begin(), [](wchar_t value) { return static_cast<wchar_t>(::towlower(value)); });
				if (line.size() <= 128 && (lineLower.contains(L"license") || lineLower.contains(L"licence"))) return std::wstring(line);
				break;
			}
			if (end == std::wstring::npos) break;
			remaining.remove_prefix(end + 1);
		}
		return L"Custom license";
	}

	Package LoadArchive(const std::filesystem::path& path, std::stop_token stop)
	{
		return ParsePackage(ReadArchiveEntries(path, stop), path, false, stop);
	}

	Package LoadDeployed(const std::filesystem::path& directory, std::stop_token stop)
	{
		auto package = ParsePackage(ReadDeployedEntries(directory, stop), directory, true, stop);
		for (const auto& [name, bytes] : package.assets) package.assetSources.emplace(name, directory / PathFromUtf8(name));
		return package;
	}

	std::filesystem::path GetPresetRoot()
	{
#ifdef OPENGLASS_PRESET_TEST_STORAGE
		THROW_HR(E_ACCESSDENIED); // Tests must supply their isolated fixture library explicitly.
#else
		return ApplicationPaths::GetProgramDataSubdirectory(L"Presets");
#endif
	}

	std::string GeneratePackageUuid()
	{
		return CreateUuid();
	}

	bool IsValidHomepageUrl(std::wstring_view value)
	{
		if (value.empty()) return false;
		wxURI uri{ wxString(value.data(), value.size()) };
		const auto scheme = uri.GetScheme().Lower();
		return (scheme == L"http" || scheme == L"https") && !uri.GetServer().empty();
	}


	namespace
	{
		struct LibraryRecord
		{
			std::string id, revision, digest, sourceType, sourceDigest;

		};
		using LibraryIndex = std::vector<LibraryRecord>;

		LibraryIndex ReadLibraryIndex(const std::filesystem::path& root)
		{
			LibraryIndex records;
			if (!std::filesystem::exists(root)) return records;
			THROW_HR_IF(E_INVALIDARG, IsReparsePoint(root));
			const auto indexPath = root / L"library.json";
			if (!std::filesystem::exists(indexPath))
			{
				// Adopt existing immutable packages only before the first index exists.
				// Once indexed, unreferenced revision directories stay unreferenced.
				for (const auto& item : std::filesystem::directory_iterator(root))
				{
					const auto revision = item.path().filename().string();
					if (!item.is_directory() || !IsCanonicalUuid(revision)) continue;
					try
					{
						const auto package = LoadDeployed(item.path());
						THROW_HR_IF(E_INVALIDARG, package.metadata.uuid != revision);
						records.push_back({ revision, revision, package.digest, "imported", package.digest });
					}
					catch (...) { records.push_back({ revision, revision, {}, "imported", {} }); }
				}
				return records;
			}
			THROW_HR_IF(E_INVALIDARG, IsReparsePoint(indexPath) || std::filesystem::file_size(indexPath) > 4 * 1024 * 1024);
			std::ifstream file(indexPath, std::ios::binary);
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_READ_FAULT), !file);
			bool duplicate{};
			std::vector<std::set<std::string>> keys;
			const auto index = nlohmann::json::parse(file, [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value)
			{
				if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
				else if (event == nlohmann::json::parse_event_t::key) duplicate |= !keys.back().insert(value.get<std::string>()).second;
				else if (event == nlohmann::json::parse_event_t::object_end) keys.pop_back();
				return true;
			});
			THROW_HR_IF(E_INVALIDARG, duplicate || index.at("version") != 1 || !index.at("entries").is_array() || index.at("entries").size() > 4096);
			std::set<std::string> ids;
			for (const auto& item : index.at("entries"))
			{
				LibraryRecord record{ item.at("id"), item.at("revision"), item.at("digest"), item.at("source_type"), item.at("source_digest") };
				THROW_HR_IF(E_INVALIDARG, !IsCanonicalUuid(record.id) || !IsCanonicalUuid(record.revision)
					|| !ids.insert(record.id).second || record.digest.size() != 64
					|| (!record.sourceDigest.empty() && record.sourceDigest.size() != 64)
					|| (record.sourceType != "local" && record.sourceType != "local-copy" && record.sourceType != "imported" && record.sourceType != "draft"));

				records.push_back(std::move(record));
			}
			THROW_HR_IF(E_INVALIDARG, std::ranges::count(records, std::string("draft"), &LibraryRecord::sourceType) > 1);
			return records;
		}

		wil::unique_handle LockLibrary(const std::filesystem::path& root)
		{
			ManagedFiles::Directory(root);
			ManagedFiles::Protect(root);
			const auto path = root / L"library.lock";
			if (std::filesystem::exists(path)) THROW_HR_IF(E_INVALIDARG, IsReparsePoint(path));
			wil::unique_handle lock(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
			THROW_LAST_ERROR_IF(!lock);
			return lock;
		}

		void TextFile(const std::filesystem::path& path, const std::string& text)
		{
			ManagedFiles::Write(path, { reinterpret_cast<const std::byte*>(text.data()), text.size() });
		}
		Package ReadEntry(const std::filesystem::path& path, std::stop_token stop = {})
		{
			auto package = LoadDeployed(path, stop);
			package.libraryId = path.filename().string();
			const auto recordPath = path / L".accepted.json";
			if (std::filesystem::exists(recordPath))
			{
				const auto bytes = ManagedFiles::Read(recordPath);
				THROW_HR_IF(E_INVALIDARG, bytes.size() > 4096);
				const auto record = nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data()) + bytes.size());
				package.trusted = record.at("version") == 1 && record.at("digest") == package.digest;
				package.sourceType = record.value("source_type", "imported");
			}
			return package;
		}
		void WriteContent(const Package& package, const std::filesystem::path& path, const std::filesystem::path& root, std::string_view sourceType)
		{
			ManagedFiles::Directory(path);
			TextFile(path / L"manifest.json", package.manifestText);
			if (!package.licenseText.empty()) TextFile(path / L"LICENSE", package.licenseText);
			for (const auto& [name, bytes] : package.assets)
			{
				const auto target = path / PathFromUtf8(name);
				const auto found = package.assetSources.find(name);
				if (found != package.assetSources.end() && ManagedFiles::Matches(found->second, bytes))
				{
					ManagedFiles::LinkOrCopy(found->second, target, root.parent_path());
					THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), ManagedFiles::Read(target) != bytes);
				}
				else ManagedFiles::Write(target, bytes);
			}
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), LoadDeployed(path).digest != package.digest);
			TextFile(path / L".accepted.json", nlohmann::json{ { "version", 1 }, { "digest", package.digest }, { "source_type", sourceType } }.dump());
		}
		void RecoverLibrary(const std::filesystem::path& root, std::vector<std::wstring>* maintenance = nullptr, std::stop_token stop = {})
		{
			CheckCancelled(stop);
			const auto library = root / L"Library";
			ManagedFiles::Directory(library);
			for (const auto& item : std::filesystem::directory_iterator(library))
			{
				CheckCancelled(stop);
				const auto name = item.path().filename().string();
				if (name.ends_with(".previous"))
				{
					const auto id = name.substr(0, name.size() - 9);
					if (!IsCanonicalUuid(id)) continue;
					const auto target = library / PathFromUtf8(id);
					try
					{
						if (!std::filesystem::exists(target)) std::filesystem::rename(item.path(), target);
						else { THROW_HR_IF(E_INVALIDARG, !ReadEntry(target).trusted); ManagedFiles::RemoveTree(item.path(), library); }
					}
					catch (...) { if (maintenance) maintenance->push_back(L"Recovery or cleanup pending: " + item.path().filename().wstring()); LOG_CAUGHT_EXCEPTION(); }
				}
				else if (name.ends_with(".staging") || name.ends_with(".deleted"))
				{
					try { ManagedFiles::RemoveTree(item.path(), library); } catch (...) { if (maintenance) maintenance->push_back(L"Cleanup pending: " + item.path().filename().wstring()); LOG_CAUGHT_EXCEPTION(); }
				}
			}
			const auto marker = library / L".legacy-migrated";
			if (!std::filesystem::exists(marker))
			{
				for (const auto& record : ReadLibraryIndex(root))
				{
					CheckCancelled(stop);
					if (record.sourceType == "draft") continue;
					const auto target = library / PathFromUtf8(record.id);
					if (std::filesystem::exists(target)) continue;
					try
					{
						const auto package = LoadDeployed(root / PathFromUtf8(record.revision));
						THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), package.digest != record.digest);
						const auto staging = library / PathFromUtf8(record.id + ".staging");
						WriteContent(package, staging, root, record.sourceType);
						std::filesystem::rename(staging, target);
					}
					catch (...)
					{
						const auto error = wil::ResultFromCaughtException();
						ManagedFiles::Directory(target);
						TextFile(target / L".migration-error.txt", std::format("Legacy entry could not be migrated (0x{:08X}). Original content remains at {}.", static_cast<unsigned long>(error), record.revision));
					}
				}
				TextFile(marker, "1");
			}
		}
	}

	std::vector<Package> EnumerateInstalled(const std::filesystem::path& root, std::vector<std::wstring>* maintenance, std::stop_token stop)
	{
		CheckCancelled(stop);
		auto lock = LockLibrary(root); RecoverLibrary(root, maintenance, stop);
		std::vector<Package> packages;
		for (const auto& item : std::filesystem::directory_iterator(root / L"Library"))
		{
			CheckCancelled(stop);
			const auto id = item.path().filename().string();
			if (!IsCanonicalUuid(id)) continue;
			try { auto package = ReadEntry(item.path(), stop); package.assets.clear(); packages.push_back(std::move(package)); }
			catch (...)
			{
				CheckCancelled(stop); // Cancellation is not an invalid library entry.
				Package invalid; invalid.libraryId = id; invalid.source = item.path(); invalid.deployed = true;
				invalid.metadata.name = item.path().filename().wstring();
				invalid.loadError = std::format(L"Invalid preset (0x{:08X})", static_cast<unsigned long>(wil::ResultFromCaughtException()));
				if (std::filesystem::exists(item.path() / L".migration-error.txt"))
				{
					const auto error = ManagedFiles::Read(item.path() / L".migration-error.txt");
					invalid.loadError = wxString::FromUTF8(reinterpret_cast<const char*>(error.data()), error.size()).ToStdWstring();
				}
				packages.push_back(std::move(invalid));
			}
		}
		std::ranges::sort(packages, {}, [](const Package& package) { return package.metadata.name; });
		return packages;
	}

	Package LoadLibraryEntry(const Package& entry, const std::filesystem::path& root, std::stop_token stop)
	{
		CheckCancelled(stop);
		auto lock = LockLibrary(root); RecoverLibrary(root, nullptr, stop);
		THROW_HR_IF(E_INVALIDARG, !IsCanonicalUuid(entry.libraryId));
		auto result = ReadEntry(root / L"Library" / PathFromUtf8(entry.libraryId), stop);
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), result.digest != entry.digest);
		return result;
	}

	Package LoadTrusted(const Package& entry, const std::filesystem::path& root, std::stop_token stop)
	{
		auto result = LoadLibraryEntry(entry, root, stop);
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), !result.trusted);
		return result;
	}

	Package Publish(const Package& input, std::string_view sourceType, std::string_view existingId, const std::filesystem::path& root, std::string_view expectedDigest)
	{
		auto lock = LockLibrary(root); RecoverLibrary(root);
		const auto reloaded = input.deployed ? std::optional{ LoadDeployed(input.source) } : std::nullopt;
		const auto& package = reloaded ? *reloaded : input;
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), input.digest != package.digest);
		const auto library = root / L"Library";
		const auto id = existingId.empty() ? CreateUuid() : std::string(existingId);
		THROW_HR_IF(E_INVALIDARG, !IsCanonicalUuid(std::string(id)));
		for (const auto& item : std::filesystem::directory_iterator(library))
		{
			if (!IsCanonicalUuid(item.path().filename().string())) continue;
			Package candidate;
			try { candidate = ReadEntry(item.path()); } catch (...) { continue; }
			if (candidate.metadata.uuid == package.metadata.uuid)
			{
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_DUP_NAME), candidate.digest != package.digest);
				if (existingId.empty() && candidate.trusted) return candidate;
			}
		}
		const auto target = library / PathFromUtf8(id);
		if (!existingId.empty())
		{
			const auto previous = ReadEntry(target);
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), expectedDigest.empty() || previous.digest != expectedDigest);
			THROW_HR_IF(E_INVALIDARG, previous.legacyLicense && !package.legacyLicense);
			for (const auto& source : previous.attribution)
				THROW_HR_IF(E_INVALIDARG, std::ranges::find(package.attribution, source) == package.attribution.end());
			for (const auto& license : previous.inheritedLicenses)
				THROW_HR_IF(E_INVALIDARG, package.licenseText.find(license) == std::string::npos || std::ranges::find(package.inheritedLicenses, license) == package.inheritedLicenses.end());
		}
		const auto staging = library / PathFromUtf8(id + ".staging");
		const auto previous = library / PathFromUtf8(id + ".previous");
		auto cleanup = wil::scope_exit([&] { try { ManagedFiles::RemoveTree(staging, library); } catch (...) { LOG_CAUGHT_EXCEPTION(); } });
		WriteContent(package, staging, root, sourceType);
		if (std::filesystem::exists(target)) std::filesystem::rename(target, previous);
		try { std::filesystem::rename(staging, target); }
		catch (...) { if (std::filesystem::exists(previous)) std::filesystem::rename(previous, target); throw; }
		try { ManagedFiles::RemoveTree(previous, library); } catch (...) { LOG_CAUGHT_EXCEPTION(); } // Published; enumeration reports pending cleanup.
		return ReadEntry(target);
	}

	void RemoveLibraryEntry(std::string_view id, const std::filesystem::path& root)
	{
		auto lock = LockLibrary(root); RecoverLibrary(root);
		THROW_HR_IF(E_INVALIDARG, !IsCanonicalUuid(std::string(id)));
		const auto library = root / L"Library";
		const auto target = library / PathFromUtf8(id);
		const auto deleted = library / PathFromUtf8(std::string(id) + ".deleted");
		ManagedFiles::CheckPath(target);
		// A leftover replacement backup would otherwise restore the deleted entry.
		// Keep the current entry if its old content still cannot be removed.
		ManagedFiles::RemoveTree(library / PathFromUtf8(std::string(id) + ".previous"), library);
		std::filesystem::rename(target, deleted);
		ManagedFiles::RemoveTree(deleted, library);
	}

	namespace
	{
		void RetainLicense(CreateRequest& request, const std::string& text)
		{
			if (text.empty()) return;
			if (std::ranges::find(request.inheritedLicenses, text) == request.inheritedLicenses.end())
				request.inheritedLicenses.push_back(text);
			if (request.licenseText.find(text) == std::string::npos)
			{
				if (!request.licenseText.empty()) request.licenseText += "\n\n";
				request.licenseText += text;
			}
		}
	}

	void PreserveSource(CreateRequest& request, const Package& source, bool imageAssets)
	{
		for (const auto& attribution : source.attribution)
			if (std::ranges::find(request.attribution, attribution) == request.attribution.end()) request.attribution.push_back(attribution);
		const auto attribution = source.metadata.authorName + L" — " + source.metadata.authorHomepage;
		if (std::ranges::find(request.attribution, attribution) == request.attribution.end()) request.attribution.push_back(attribution);
		request.legacyLicense |= source.legacyLicense;
		if (imageAssets || source.legacyLicense)
		{
			for (const auto& license : source.inheritedLicenses) RetainLicense(request, license);
			RetainLicense(request, source.licenseText);
		}
	}

	void PreserveRevisionProvenance(CreateRequest& request, const Package* target, const Package* origin)
	{
		if (target)
		{
			for (const auto& notice : target->attribution)
				if (std::ranges::find(request.attribution, notice) == request.attribution.end()) request.attribution.push_back(notice);
			request.legacyLicense |= target->legacyLicense;
			for (const auto& license : target->inheritedLicenses) RetainLicense(request, license);
			if (HasInheritedMetadata(*target)) PreserveSource(request, *target, true);
			else if (request.metadata.authorName != target->metadata.authorName || request.metadata.authorHomepage != target->metadata.authorHomepage)
				PreserveSource(request, *target, false);
		}
		// Selection chooses the destination, not the provenance of the effective configuration.
		if (origin && (!target || !SameRevision(*target, *origin))) PreserveSource(request, *origin, false);
	}

	Package CreateSnapshot(CreateRequest request, std::stop_token stop)
	{
		CheckCancelled(stop);
		if (!request.licenseText.empty())
		{
			ValidateLicense(request.licenseText);
		}
		THROW_HR_IF(E_INVALIDARG,
			request.metadata.name.empty()
			|| request.metadata.authorName.empty()
			|| (!request.metadata.authorHomepage.empty() && !IsValidHomepageUrl(request.metadata.authorHomepage))
		);
		if (request.metadata.uuid.empty()) request.metadata.uuid = CreateUuid();
		THROW_HR_IF(E_INVALIDARG, !IsCanonicalUuid(request.metadata.uuid));
		THROW_HR_IF(E_INVALIDARG, request.settings.size() != Settings::PresetPackSettingCount());
		std::map<std::string, std::vector<std::byte>> assets;
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), request.assetSources.size() > MaximumEntryCount - 2);
		std::uint64_t assetSize{};
		for (const auto& [name, source] : request.assetSources)
		{
			CheckCancelled(stop);
			THROW_HR_IF(E_INVALIDARG, !name.starts_with("assets/") || !IsSafeEntryPath(name));
			THROW_HR_IF(E_INVALIDARG, IsReparsePoint(source));
			const auto size = std::filesystem::file_size(source);
			const auto limit = name.ends_with(".layout") ? ThemeAtlasLayout::MaximumFileSize : MaximumEntrySize;
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size > limit || assetSize + size > MaximumTotalSize);
			std::ifstream stream(source, std::ios::binary);
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !stream);
			std::vector<std::byte> bytes(static_cast<std::size_t>(size));
			stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), stream.gcount() != static_cast<std::streamsize>(size) || stream.peek() != std::char_traits<char>::eof());
			// Validate this captured content in ParsePackage, never reopen mutable sources.
			assetSize += size;
			assets.emplace(name, std::move(bytes));
		}
		const auto manifest = BuildManifest(request, assets);
		EntryMap entries;
		entries.merge(assets);
		auto addText = [&](const std::string& name, const std::string& text)
		{
			std::vector<std::byte> bytes(text.size());
			std::memcpy(bytes.data(), text.data(), text.size());
			entries.emplace(name, std::move(bytes));
		};
		addText("manifest.json", manifest);
		if (!request.licenseText.empty()) addText("LICENSE", request.licenseText);
		std::size_t total{};
		for (const auto& [_, bytes] : entries) total += bytes.size();
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), total > MaximumTotalSize || entries.size() > MaximumEntryCount);
		auto package = ParsePackage(std::move(entries), {}, false, stop);
		package.assetSources = std::move(request.assetSources);
		return package;
	}

	void ExportArchive(const std::filesystem::path& path, const Package& input)
	{
		const auto reloaded = !input.libraryId.empty() ? std::optional{ LoadTrusted(input, input.source.parent_path().parent_path()) }
			: input.deployed ? std::optional{ LoadDeployed(input.source) } : std::nullopt;
		const auto& package = reloaded ? *reloaded : input;
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), package.digest != input.digest);
		const auto& manifest = package.manifestText;
		const auto& assets = package.assets;
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), std::filesystem::exists(path));
		const auto temporary = path.wstring() + L".tmp-" + FromUtf8(CreateUuid());
		auto cleanup = wil::scope_exit([&]
		{
			std::error_code error;
			std::filesystem::remove(temporary, error);
		});
		wxFFileOutputStream output(temporary);
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), !output.IsOk());
		wxZipOutputStream zip(output, 9);
		auto writeEntry = [&](const wxString& name, std::span<const std::byte> bytes)
		{
			const wxDateTime stableTimestamp(1, wxDateTime::Jan, 2000, 0, 0, 0);
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), !zip.PutNextEntry(name, stableTimestamp, bytes.size()));
			zip.Write(bytes.data(), bytes.size());
			THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), !zip.IsOk());
		};
		writeEntry(L"manifest.json", { reinterpret_cast<const std::byte*>(manifest.data()), manifest.size() });
		if (!package.licenseText.empty())
		{
			writeEntry(L"LICENSE", { reinterpret_cast<const std::byte*>(package.licenseText.data()), package.licenseText.size() });
		}
		for (const auto& [name, bytes] : assets)
		{
			writeEntry(wxString::FromUTF8(name), bytes);
		}
		zip.Close();
		output.Close();
		// The creator and importer deliberately share one validation path. Never publish
		// an archive that the importer would reject.
		const auto validated = LoadArchive(temporary);
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), validated.digest != package.digest);
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH));
		cleanup.release();
		THROW_IF_WIN32_BOOL_FALSE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
	}
}
