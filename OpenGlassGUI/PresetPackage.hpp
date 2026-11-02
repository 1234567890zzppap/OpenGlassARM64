#pragma once
#include "SettingsCatalog.hpp"

#include <Windows.h>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace OpenGlass::PresetPackages
{
	struct AssetReference
	{
		std::string path;
		bool operator==(const AssetReference&) const = default;
	};

	using SettingValue = std::variant<std::monostate, DWORD, AssetReference>;

	struct Metadata
	{
		std::string uuid;
		std::wstring name;
		std::wstring description;
		std::wstring authorName;
		std::wstring authorHomepage;
		std::wstring licenseName;
	};

	struct Package
	{
		bool legacyLicense{};
		std::vector<std::wstring> attribution;
		std::vector<std::string> inheritedLicenses;
		std::optional<DWORD> accentColor;
		std::vector<std::wstring> conversions;
		std::string libraryId;
		std::string sourceType;
		// Local admission is never read from an imported manifest.
		bool trusted{};
		std::wstring loadError;
		Metadata metadata;
		std::map<Settings::Id, SettingValue> settings;
		std::vector<std::wstring> ignoredSettingNames;
		std::map<std::string, std::vector<std::byte>> assets;
		std::map<std::string, std::filesystem::path> assetSources;
		std::vector<std::pair<std::string, std::uint64_t>> assetSummary;
		std::string licenseText;
		std::string manifestText;
		std::string digest;
		std::filesystem::path source;
		unsigned catalogVersion{};
		std::size_t ignoredSettingCount{};
		bool deployed{};
	};

	[[nodiscard]] inline bool HasInheritedMetadata(const Package& package)
	{
		return package.legacyLicense || package.sourceType != "local";
	}

	[[nodiscard]] inline bool SameRevision(const Package& left, const Package& right)
	{
		return !left.digest.empty() && left.metadata.uuid == right.metadata.uuid && left.digest == right.digest;
	}

	// Retain known source notices through configuration previews, independently of list selection.
	class PreviewProvenance
	{
		std::optional<Package> m_origin, m_checkpointOrigin;
	public:
		const std::optional<Package>& Origin() const { return m_origin; }
		void SetOrigin(const Package& package, bool pending)
		{
			// Extraction needs identity and source terms, never another retained
			// copy of the images or configuration already captured elsewhere.
			m_origin = Package{
				.legacyLicense = package.legacyLicense,
				.attribution = package.attribution,
				.inheritedLicenses = package.inheritedLicenses,
				.libraryId = package.libraryId,
				.sourceType = package.sourceType,
				.metadata = package.metadata,
				.licenseText = package.licenseText,
				.digest = package.digest
			};
			if (!pending) Accept();
		}
		void Accept() { m_checkpointOrigin = m_origin; }
		void Revert() { m_origin = m_checkpointOrigin; }
	};

	struct CreateRequest
	{
		bool legacyLicense{};
		std::vector<std::wstring> attribution;
		std::vector<std::string> inheritedLicenses;
		std::optional<DWORD> accentColor;
		Metadata metadata;
		std::map<Settings::Id, SettingValue> settings;
		std::map<std::string, std::filesystem::path> assetSources;
		std::string licenseText;
	};

	[[nodiscard]] Package LoadArchive(const std::filesystem::path& path, std::stop_token stop = {});
	[[nodiscard]] Package LoadDeployed(const std::filesystem::path& directory, std::stop_token stop = {});
	[[nodiscard]] std::filesystem::path GetPresetRoot();
	[[nodiscard]] std::vector<Package> EnumerateInstalled(const std::filesystem::path& root = GetPresetRoot(), std::vector<std::wstring>* maintenance = nullptr, std::stop_token stop = {});
	[[nodiscard]] std::string GeneratePackageUuid();
	[[nodiscard]] bool IsValidHomepageUrl(std::wstring_view value);
	[[nodiscard]] std::wstring InferLicenseName(std::string_view licenseText);
	[[nodiscard]] Package CreateSnapshot(CreateRequest request, std::stop_token stop = {});
	void ExportArchive(const std::filesystem::path& path, const Package& package);
	[[nodiscard]] Package Publish(const Package& package, std::string_view sourceType,
		std::string_view existingId = {}, const std::filesystem::path& root = GetPresetRoot(), std::string_view expectedDigest = {});
	[[nodiscard]] Package LoadLibraryEntry(const Package& entry, const std::filesystem::path& root = GetPresetRoot(), std::stop_token stop = {});
	[[nodiscard]] Package LoadTrusted(const Package& entry, const std::filesystem::path& root = GetPresetRoot(), std::stop_token stop = {});
	void RemoveLibraryEntry(std::string_view id, const std::filesystem::path& root = GetPresetRoot());
	void PreserveSource(CreateRequest& request, const Package& source, bool imageAssets);
	void PreserveRevisionProvenance(CreateRequest& request, const Package* target, const Package* origin);
}
