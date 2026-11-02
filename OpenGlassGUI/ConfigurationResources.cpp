#include "pch.h"
#include "ConfigurationResources.hpp"
#include <nlohmann/json.hpp>

namespace OpenGlass
{
	namespace
	{
		using Json = nlohmann::json;
		std::string Utf8(const std::wstring& value) { return wxString(value).ToStdString(wxConvUTF8); }
		Json Choice(const ColorPreference::Snapshot& value)
		{
			return { { "automatic", value.automatic }, { "rgb", value.rgb ? Json(*value.rgb) : Json(nullptr) },
				{ "apply_choice", value.applyChoice }, { "accent", value.accent
					? Json{ { "present", value.accent->present }, { "type", value.accent->type }, { "bytes", value.accent->bytes } }
					: Json(nullptr) }, { "dwm_accent", value.dwmAccent
					? Json{ { "present", value.dwmAccent->present }, { "type", value.dwmAccent->type }, { "bytes", value.dwmAccent->bytes } }
					: Json(nullptr) } };
		}
		ColorPreference::Snapshot ReadChoice(const Json& value)
		{
			ColorPreference::Snapshot result;
			result.automatic = value.at("automatic").get<DWORD>();
			result.applyChoice = value.at("apply_choice").get<bool>();
			if (!value.at("accent").is_null())
			{
				const auto& accent = value.at("accent");
				result.accent = RegistryConfig::RawValue{ accent.at("present"), accent.at("type"), accent.at("bytes").get<std::vector<BYTE>>() };
			}
			if (const auto accent = value.find("dwm_accent"); accent != value.end() && !accent->is_null())
				result.dwmAccent = RegistryConfig::RawValue{ accent->at("present"), accent->at("type"), accent->at("bytes").get<std::vector<BYTE>>() };
			if (!value.at("rgb").is_null()) result.rgb = value.at("rgb").get<DWORD>();
			return result;
		}
	}
	ConfigurationResources::Preparation::~Preparation()
	{
		if (directory.empty()) return;
		lease.reset();
		try { ManagedFiles::RemoveTree(directory, directory.parent_path()); } catch (...) { LOG_CAUGHT_EXCEPTION(); }
	}
	void ConfigurationResources::Initialize(std::filesystem::path root, std::wstring sid)
	{
		PSID parsed{};
		THROW_IF_WIN32_BOOL_FALSE(ConvertStringSidToSidW(sid.c_str(), &parsed));
		wil::unique_hlocal owner(parsed);
		m_root = std::move(root); m_sid = std::move(sid);
	}
	wil::unique_hfile ConfigurationResources::AcquireWriter() const
	{
		ManagedFiles::Directory(m_root); ManagedFiles::Protect(m_root);
		const auto path = m_root / L"preview.lock";
		ManagedFiles::CheckPath(path);
		wil::unique_hfile writer(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), !writer);
		return writer;
	}
	std::filesystem::path ConfigurationResources::Path(Settings::Scope scope, Settings::Id id) const
	{
		THROW_HR_IF(E_INVALIDARG, Settings::Get(id).assetRole == Settings::AssetRole::None);
		const auto owner = scope == Settings::Scope::Machine ? m_root / L"Machine" : m_root / L"Users" / m_sid;
		return owner / (std::wstring(Settings::Get(id).name) + L".png");
	}
	bool ConfigurationResources::ValidTarget(const std::filesystem::path& path) const
	{
		for (const auto& spec : Settings::Catalog) if (spec.assetRole != Settings::AssetRole::None)
			for (const auto scope : { Settings::Scope::User, Settings::Scope::Machine })
			{
				const auto base = Path(scope, spec.id);
				if (path == base || path == base.wstring() + L".source.json" || (spec.assetRole == Settings::AssetRole::ThemeAtlas && path == base.wstring() + L".layout")) return true;
			}
		return false;
	}
	bool ConfigurationResources::IsManaged(const std::filesystem::path& path) const
	{
		return ManagedFiles::Within(path, m_root) || ManagedFiles::Within(path, m_root.parent_path() / L"Presets");
	}
	bool ConfigurationResources::NeedsImport(Settings::Scope scope, Settings::Id id, const std::filesystem::path& source) const
	{
		return IsManaged(source) && CompareStringOrdinal(source.c_str(), -1, Path(scope, id).c_str(), -1, TRUE) != CSTR_EQUAL;
	}
	ConfigurationResources::Content ConfigurationResources::Read(const std::filesystem::path& path) const
	{
		ManagedFiles::CheckPath(path);
		return std::filesystem::exists(path) ? Content{ ManagedFiles::Read(path) } : std::nullopt;
	}
	std::unique_ptr<ConfigurationResources::Preparation> ConfigurationResources::Prepare(const PresetPackages::Package& package, Settings::Scope scope)
	{
		ManagedFiles::Directory(m_root); ManagedFiles::Protect(m_root);
		for (const auto& item : std::filesystem::directory_iterator(m_root))
		{
			if (!item.path().filename().wstring().starts_with(L".prepare-")) continue;
			ManagedFiles::CheckPath(item.path());
			wil::unique_hfile lease(CreateFileW((item.path() / L"lease").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
			if (!lease) continue;
			lease.reset();
			try { ManagedFiles::RemoveTree(item.path(), m_root); } catch (...) { LOG_CAUGHT_EXCEPTION(); }
		}
		auto preparation = std::make_unique<Preparation>();
		preparation->directory = m_root / (L".prepare-" + std::filesystem::path(PresetPackages::GeneratePackageUuid()).wstring());
		ManagedFiles::Directory(preparation->directory);
		preparation->lease.reset(CreateFileW((preparation->directory / L"lease").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
		THROW_LAST_ERROR_IF(!preparation->lease);
		Json notices{ { "legacy", package.legacyLicense }, { "author", Utf8(package.metadata.authorName) }, { "homepage", Utf8(package.metadata.authorHomepage) },
			{ "license", package.licenseText }, { "sources", Json::array() }, { "licenses", package.inheritedLicenses }, { "entry", package.libraryId }, { "local", package.sourceType == "local" } };
		for (const auto& source : package.attribution) notices["sources"].push_back(Utf8(source));
		for (const auto& [id, value] : package.settings)
		{
			const auto asset = std::get_if<PresetPackages::AssetReference>(&value);
			if (!asset) continue;
			const auto destination = Path(scope, id);
			auto stage = [&](const std::string& name, const std::filesystem::path& target)
			{
				const auto bytes = package.assets.find(name);
				if (bytes == package.assets.end()) { preparation->files[target] = std::nullopt; return; }
				const auto staged = preparation->directory / target.filename();
				const auto source = package.assetSources.find(name);
				if (source != package.assetSources.end() && ManagedFiles::Matches(source->second, bytes->second))
					ManagedFiles::LinkOrCopy(source->second, staged, m_root.parent_path());
				else ManagedFiles::Write(staged, bytes->second);
				THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), ManagedFiles::Read(staged) != bytes->second);
				preparation->files[target] = staged;
			};
			stage(asset->path, destination);
			if (Settings::Get(id).assetRole == Settings::AssetRole::ThemeAtlas) stage(asset->path + ".layout", destination.wstring() + L".layout");
			const auto metadata = destination.wstring() + L".source.json";
			const auto staged = preparation->directory / std::filesystem::path(metadata).filename();
			const auto text = notices.dump();
			ManagedFiles::Write(staged, { reinterpret_cast<const std::byte*>(text.data()), text.size() });
			preparation->files[metadata] = staged;
		}
		return preparation;
	}
	std::unique_ptr<ConfigurationResources::Preparation> ConfigurationResources::PrepareFile(Settings::Scope scope, Settings::Id id, const std::filesystem::path& source)
	{
		PresetPackages::CreateRequest request;
		request.metadata.name = L"Configuration resource"; request.metadata.authorName = L"External source";
		for (const auto& spec : Settings::Catalog) if (Settings::IsPresetPackSetting(spec)) request.settings[spec.id] = std::monostate{};
		const auto role = Settings::Get(id).assetRole;
		const std::string name = role == Settings::AssetRole::ThemeAtlas ? "assets/theme-atlas.png" : role == Settings::AssetRole::Reflection ? "assets/reflection.png" : "assets/material.png";
		request.settings[id] = PresetPackages::AssetReference{ name }; request.assetSources[name] = source;
		if (role == Settings::AssetRole::ThemeAtlas && std::filesystem::exists(source.wstring() + L".layout")) request.assetSources[name + ".layout"] = source.wstring() + L".layout";
		PreserveSource(request, source);
		return Prepare(PresetPackages::CreateSnapshot(std::move(request)), scope);
	}
	void ConfigurationResources::PreserveSource(PresetPackages::CreateRequest& request, const std::filesystem::path& source, const PresetPackages::Package* updateTarget) const
	{
		if (!IsManaged(source)) return;
		const auto record = source.wstring() + L".source.json";
		if (std::filesystem::exists(record))
		{
			const auto bytes = ManagedFiles::Read(record);
			THROW_HR_IF(E_INVALIDARG, bytes.size() > 4 * 1024 * 1024);
			const auto data = Json::parse(reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data()) + bytes.size());
			PresetPackages::Package package;
			package.legacyLicense = data.at("legacy"); package.metadata.authorName = wxString::FromUTF8(data.at("author").get<std::string>()).ToStdWstring();
			package.metadata.authorHomepage = wxString::FromUTF8(data.at("homepage").get<std::string>()).ToStdWstring();
			package.licenseText = data.at("license"); package.inheritedLicenses = data.at("licenses").get<std::vector<std::string>>();
			for (const auto& notice : data.at("sources")) package.attribution.push_back(wxString::FromUTF8(notice.get<std::string>()).ToStdWstring());
			if (updateTarget && !updateTarget->libraryId.empty() && data.value("entry", std::string{}) == updateTarget->libraryId && data.value("local", false) && !package.legacyLicense)
			{
				// Updating this entry can edit its own terms; retained external terms remain separate.
				package.sourceType = "local";
				PresetPackages::PreserveRevisionProvenance(request, &package, nullptr);
			}
			else PresetPackages::PreserveSource(request, package, true);
		}
		else if (source.parent_path().filename() == L"assets")
			PresetPackages::PreserveSource(request, PresetPackages::LoadDeployed(source.parent_path().parent_path()), true);
	}
	bool ConfigurationResources::HasForeignRecovery() const
	{
		const auto marker = m_root / L"operation-owner";
		if (!std::filesystem::exists(marker)) return false;
		const auto bytes = ManagedFiles::Read(marker);
		THROW_HR_IF(E_INVALIDARG, bytes.size() > 256);
		const auto owner = wxString::FromUTF8(reinterpret_cast<const char*>(bytes.data()), bytes.size()).ToStdWstring();
		PSID sid{}; THROW_IF_WIN32_BOOL_FALSE(ConvertStringSidToSidW(owner.c_str(), &sid)); wil::unique_hlocal storage(sid);
		return owner != m_sid && std::filesystem::exists(m_root / (L".operation-" + owner) / L"state.json");
	}
	bool ConfigurationResources::HasRecovery() const { return std::filesystem::exists(m_root / (L".operation-" + m_sid) / L"state.json"); }
	void ConfigurationResources::Begin(std::optional<ColorPreference::Snapshot> color)
	{
		THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RECOVERY_FAILURE), HasRecovery());
		ClearOperation(); m_backupPrefix = std::filesystem::path(PresetPackages::GeneratePackageUuid()).wstring(); m_journal.Begin(); m_attemptFiles.clear(); m_attemptRegistry.clear(); m_color = color;
		Persist();
	}
	void ConfigurationResources::TrackColor(const ColorPreference::Snapshot& color)
	{
		m_color = color;
		Persist();
	}
	void ConfigurationResources::PrepareRevert(std::vector<RegistryBefore> registry, std::optional<ColorPreference::Snapshot> color)
	{
		// Publish a recoverable restore target before the first Revert write. Earlier
		// operation records stay intact until the replacement record is durable.
		m_backupPrefix = std::filesystem::path(PresetPackages::GeneratePackageUuid()).wstring();
		m_attemptFiles.clear(); m_attemptRegistry = std::move(registry); m_color = color;
		m_journal.VisitRestore(false, [&](const auto& path, const auto& content) { m_attemptFiles.emplace(path, content); });
		Persist();
	}
	void ConfigurationResources::Persist()
	{
		const auto directory = m_root / (L".operation-" + m_sid);
		ManagedFiles::Directory(directory);
		const auto marker = m_root / L"operation-owner";
		if (!std::filesystem::exists(marker))
		{
			const auto owner = Utf8(m_sid);
			// A failed write must not publish a partial SID and block later recovery.
			const auto temporary = directory / L"owner.tmp";
			std::filesystem::remove(temporary);
			ManagedFiles::Write(temporary, { reinterpret_cast<const std::byte*>(owner.data()), owner.size() });
			THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temporary.c_str(), marker.c_str(), MOVEFILE_WRITE_THROUGH));
		}
		Json state{ { "version", 1 }, { "sid", Utf8(m_sid) }, { "files", Json::array() }, { "registry", Json::array() }, { "color", m_color ? Choice(*m_color) : Json(nullptr) } };
		for (const auto& [path, before] : m_attemptFiles)
		{
			const auto relative = path.lexically_relative(m_root).generic_wstring();
			auto name = m_backupPrefix + L"-" + relative; std::ranges::replace(name, L'/', L'_');
			if (before && !std::filesystem::exists(directory / name))
			{
				if (Read(path) == before) ManagedFiles::LinkOrCopy(path, directory / name, m_root);
				else ManagedFiles::Write(directory / name, *before);
			}
			state["files"].push_back({ { "target", Utf8(relative) }, { "backup", before ? Json(Utf8(name)) : Json(nullptr) } });
		}
		for (const auto& entry : m_attemptRegistry) state["registry"].push_back({ { "user", entry.scope == Settings::Scope::User }, { "id", static_cast<unsigned>(entry.id) }, { "name", Utf8(std::wstring(Settings::Get(entry.id).name)) }, { "present", entry.value.present }, { "type", entry.value.type }, { "bytes", entry.value.bytes } });
		const auto text = state.dump();
		const auto temporary = directory / L"state.tmp";
		std::filesystem::remove(temporary);
		ManagedFiles::Write(temporary, { reinterpret_cast<const std::byte*>(text.data()), text.size() });
		THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temporary.c_str(), (directory / L"state.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
	}
	void ConfigurationResources::TrackRegistry(Settings::Scope scope, Settings::Id id, const RegistryConfig::RawValue& value)
	{
		if (!m_journal.IsAttemptActive()) return;
		if (std::ranges::any_of(m_attemptRegistry, [&](const auto& entry) { return entry.id == id && entry.scope == scope; })) return;
		m_attemptRegistry.push_back({ scope, id, value }); Persist();
	}
	void ConfigurationResources::Install(const Preparation& preparation)
	{
		for (const auto& [target, source] : preparation.files)
		{
			THROW_HR_IF(E_INVALIDARG, !ValidTarget(target));
			ManagedFiles::Directory(target.parent_path()); ManagedFiles::Protect(target.parent_path());
			const auto before = Read(target);
			const auto after = source ? Content{ ManagedFiles::Read(*source) } : std::nullopt;
			if (before == after) continue;
			m_journal.Touch(target, [&](const auto&) { return before; });
			m_attemptFiles.try_emplace(target, before); Persist();
			if (!source) std::filesystem::remove(target);
			else
			{
				const auto temporary = target.wstring() + L".pending";
				ManagedFiles::CheckPath(temporary); std::filesystem::remove(temporary);
				ManagedFiles::LinkOrCopy(*source, temporary, m_root);
				THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
			}
		}
		Reconcile();
	}
	void ConfigurationResources::RemoveUnused(Settings::Scope scope, Settings::Id id, const std::wstring& configuredPath)
	{
		const auto target = Path(scope, id);
		if (CompareStringOrdinal(target.c_str(), -1, configuredPath.c_str(), -1, TRUE) == CSTR_EQUAL) return;
		Preparation removal; removal.files[target] = std::nullopt; removal.files[target.wstring() + L".source.json"] = std::nullopt;
		if (Settings::Get(id).assetRole == Settings::AssetRole::ThemeAtlas) removal.files[target.wstring() + L".layout"] = std::nullopt;
		Install(removal);
	}
	bool ConfigurationResources::Restore(const std::filesystem::path& path, const Content& content) noexcept
	{
		try
		{
			THROW_HR_IF(E_INVALIDARG, !ValidTarget(path));
			if (Read(path) == content) return true;
			if (!content) std::filesystem::remove(path);
			else
			{
				const auto temporary = path.wstring() + L".restore";
				ManagedFiles::CheckPath(temporary); std::filesystem::remove(temporary); ManagedFiles::Write(temporary, *content);
				THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
			}
			return true;
		}
		catch (...) { LOG_CAUGHT_EXCEPTION(); return false; }
	}
	void ConfigurationResources::Reconcile() { m_journal.Reconcile([&](const auto& path) { return Read(path); }); }
	void ConfigurationResources::ClearOperation()
	{
		ManagedFiles::RemoveTree(m_root / (L".operation-" + m_sid), m_root);
		const auto marker = m_root / L"operation-owner";
		if (std::filesystem::exists(marker) && !HasForeignRecovery()) std::filesystem::remove(marker);
		for (const auto& spec : Settings::Catalog) if (spec.assetRole != Settings::AssetRole::None)
			for (const auto scope : { Settings::Scope::Machine, Settings::Scope::User })
				for (const auto suffix : { L".pending", L".restore", L".source.json.pending", L".source.json.restore", L".layout.pending", L".layout.restore" })
				{
					const auto leftover = Path(scope, spec.id).wstring() + suffix;
					ManagedFiles::CheckPath(leftover); std::filesystem::remove(leftover);
				}
	}
	void ConfigurationResources::FinishOperation()
	{
		const auto directory = m_root / (L".operation-" + m_sid);
		const auto pending = directory / L"state.json";
		if (std::filesystem::exists(pending))
			THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(pending.c_str(), (directory / L"completed.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
		try { ClearOperation(); } catch (...) { m_cleanupPending = true; LOG_CAUGHT_EXCEPTION(); }
	}
	void ConfigurationResources::CommitAttempt() { FinishOperation(); m_journal.CommitAttempt(); m_attemptFiles.clear(); m_attemptRegistry.clear(); m_color.reset(); }
	bool ConfigurationResources::RevertAttemptFiles()
	{
		bool result = true;
		m_journal.VisitRestore(true, [&](const auto& path, const auto& value) { if (!Restore(path, value)) result = false; });
		return result;
	}
	bool ConfigurationResources::RollbackAttempt(bool externalRestored)
	{
		if (!m_journal.IsAttemptActive()) return externalRestored;
		const bool result = m_journal.RollbackAttempt([&](const auto& path, const auto& value) { return Restore(path, value); }, externalRestored);
		if (result) FinishOperation();
		return result;
	}
	bool ConfigurationResources::Revert() { return m_journal.Revert([&](const auto& path, const auto& value) { return Restore(path, value); }, true, false); }
	void ConfigurationResources::Accept() { FinishOperation(); m_journal.Accept(); }
	bool ConfigurationResources::Recover(const std::function<bool(Settings::Scope, Settings::Id, const RegistryConfig::RawValue&)>& registry,
		const std::function<bool(const ColorPreference::Snapshot&)>& color)
	{
		if (!HasRecovery()) return true;
		const auto directory = m_root / (L".operation-" + m_sid);
		const auto bytes = ManagedFiles::Read(directory / L"state.json");
		THROW_HR_IF(E_INVALIDARG, bytes.size() > 8 * 1024 * 1024);
		const auto state = Json::parse(reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data()) + bytes.size());
		THROW_HR_IF(E_INVALIDARG, state.at("version") != 1 || state.at("sid") != Utf8(m_sid));
		// Validate the entire record before invoking any mutation callback.
		std::map<std::filesystem::path, Content> files;
		std::vector<RegistryBefore> entries;
		for (const auto& item : state.at("files"))
		{
			const auto path = m_root / std::filesystem::path(wxString::FromUTF8(item.at("target").get<std::string>()).ToStdWstring());
			THROW_HR_IF(E_INVALIDARG, !ValidTarget(path));
			Content value;
			if (!item.at("backup").is_null())
			{
				const auto backup = directory / std::filesystem::path(wxString::FromUTF8(item.at("backup").get<std::string>()).ToStdWstring());
				THROW_HR_IF(E_INVALIDARG, backup.parent_path() != directory);
				value = ManagedFiles::Read(backup);
			}
			files.emplace(path, std::move(value));
		}
		for (const auto& item : state.at("registry"))
		{
			const auto id = item.at("id").get<unsigned>();
			THROW_HR_IF(E_INVALIDARG, id >= Settings::Catalog.size());
			THROW_HR_IF(E_INVALIDARG, item.at("name") != Utf8(std::wstring(Settings::Get(static_cast<Settings::Id>(id)).name)));
			entries.push_back({ item.at("user").get<bool>() ? Settings::Scope::User : Settings::Scope::Machine, static_cast<Settings::Id>(id), { item.at("present"), item.at("type"), item.at("bytes").get<std::vector<BYTE>>() } });
		}
		const auto colorBefore = state.at("color").is_null() ? std::nullopt : std::optional{ ReadChoice(state.at("color")) };
		bool success = !colorBefore || color(*colorBefore);
		for (const auto& [path, content] : files) if (!Restore(path, content)) success = false;
		for (const auto& item : entries)
		{
			if (colorBefore && colorBefore->RestoresAutomatic() && item.scope == Settings::Scope::User
				&& (item.id == Settings::Id::ColorizationColor || item.id == Settings::Id::ColorizationAfterglow)) continue;
			if (!registry(item.scope, item.id, item.value)) success = false;
		}
		if (success) FinishOperation();
		return success;
	}
}
