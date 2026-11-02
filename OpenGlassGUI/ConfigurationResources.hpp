#pragma once
#include "ManagedFiles.hpp"
#include "PreviewJournal.hpp"
#include "PresetPackage.hpp"
#include "RegistryConfig.hpp"
#include "ColorPreference.hpp"
#include <functional>
#include <memory>
#include <utility>

namespace OpenGlass
{
	class ConfigurationResources
	{
	public:
		struct RegistryBefore { Settings::Scope scope; Settings::Id id; RegistryConfig::RawValue value; };
	private:
		using Content = std::optional<std::vector<std::byte>>;
		std::filesystem::path m_root;
		std::wstring m_sid;
		PreviewJournal<std::filesystem::path, Content> m_journal;
		std::map<std::filesystem::path, Content> m_attemptFiles;
		std::wstring m_backupPrefix;
		std::vector<RegistryBefore> m_attemptRegistry;
		std::optional<ColorPreference::Snapshot> m_color;
		Content Read(const std::filesystem::path& path) const;
		bool Restore(const std::filesystem::path& path, const Content& content) noexcept;
		void Persist();
		void ClearOperation();
		void FinishOperation();
		bool m_cleanupPending{};
		bool ValidTarget(const std::filesystem::path& path) const;
	public:
		struct Preparation
		{
			std::filesystem::path directory;
			wil::unique_hfile lease;
			std::map<std::filesystem::path, std::optional<std::filesystem::path>> files;
			~Preparation();
		};
		void Initialize(std::filesystem::path root, std::wstring sid);
		[[nodiscard]] wil::unique_hfile AcquireWriter() const;
		std::filesystem::path Path(Settings::Scope scope, Settings::Id id) const;
		bool IsManaged(const std::filesystem::path& path) const;
		bool NeedsImport(Settings::Scope scope, Settings::Id id, const std::filesystem::path& source) const;
		std::unique_ptr<Preparation> Prepare(const PresetPackages::Package& package, Settings::Scope scope);
		std::unique_ptr<Preparation> PrepareFile(Settings::Scope scope, Settings::Id id, const std::filesystem::path& source);
		void PreserveSource(PresetPackages::CreateRequest& request, const std::filesystem::path& source, const PresetPackages::Package* updateTarget = nullptr) const;
		void Begin(std::optional<ColorPreference::Snapshot> color = {});
		void TrackColor(const ColorPreference::Snapshot& color);
		void PrepareRevert(std::vector<RegistryBefore> registry, std::optional<ColorPreference::Snapshot> color);
		void TrackRegistry(Settings::Scope scope, Settings::Id id, const RegistryConfig::RawValue& value);
		void Install(const Preparation& preparation);
		void RemoveUnused(Settings::Scope scope, Settings::Id id, const std::wstring& configuredPath);
		void Reconcile();
		bool IsDirty() const { return m_journal.IsDirty(); }
		bool HasAttemptChanges() const { return m_journal.HasAttemptChanges(); }
		void CommitAttempt();
		bool RollbackAttempt(bool externalRestored);
		bool RevertAttemptFiles();
		bool Revert();
		void Accept();
		bool TakeCleanupWarning() { return std::exchange(m_cleanupPending, false); }
		bool HasRecovery() const;
		bool HasForeignRecovery() const;
		bool Recover(const std::function<bool(Settings::Scope, Settings::Id, const RegistryConfig::RawValue&)>& registry,
			const std::function<bool(const ColorPreference::Snapshot&)>& color);
	};
}
