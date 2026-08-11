// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_manager.h"

#include <optional>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/time_formatting.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/monti/monti_bookmark_store.h"
#include "chrome/browser/monti/monti_extension_store.h"
#include "chrome/browser/monti/monti_fingerprint.h"
#include "chrome/browser/monti/monti_pref_names.h"
#include "chrome/browser/monti/monti_profile_launcher.h"
#include "chrome/browser/monti/monti_profile_service.h"
#include "chrome/browser/monti/monti_profile_service_factory.h"
#include "chrome/browser/monti/monti_proxy.h"
#include "chrome/browser/monti/monti_ua.h"
#include "chrome/browser/monti/monti_ua_tab_helper.h"
#include "chrome/browser/monti/folder_store.h"
#include "chrome/browser/monti/profile_store.h"
#include "chrome/browser/monti/proxy_store.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/delete_profile_helper.h"
#include "chrome/browser/profiles/profile_avatar_icon_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "chrome/browser/profiles/profile_window.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/window_metadata/window_metadata_controller.h"
#include "chrome/common/chrome_paths.h"
#include "components/prefs/pref_service.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_model_load_waiter.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "extensions/browser/unpacked_installer.h"

namespace monti {

namespace {

constexpr char16_t kMontiSharedBookmarkFolderTitle[] = u"Monti Shared";
constexpr char kMontiSharedBookmarkFolderMetaKey[] = "monti_shared_bookmarks";
constexpr char kMontiSharedBookmarkFolderMetaValue[] = "1";

std::string ProfileLaunchSwitches(const MontiProfileEntry& entry) {
  return FromJson(entry.fingerprint).command_line_switches;
}

std::string NowForProfileCreatedAt() {
  return base::TimeFormatAsIso8601(base::Time::Now());
}

// Overlays the chrome://monti-settings "Anti-Detect Defaults" onto a freshly
// generated fingerprint for a new profile. Defaults live in the manager
// profile's prefs (where the settings window writes); `source` is that profile
// (the last-used one, never the just-created `new_profile`). No-ops when the
// source/prefs are unavailable, leaving the coherent generated fingerprint.
Fingerprint ApplyDefaultFingerprintPrefs(const Fingerprint& generated,
                                         Profile* source,
                                         Profile* new_profile,
                                         uint32_t seed) {
  if (!source || source == new_profile) {
    return generated;
  }
  PrefService* prefs = source->GetPrefs();
  Fingerprint fp = generated;
  // A non-empty default preset re-keys the whole identity coherently.
  const std::string preset =
      prefs->GetString(prefs::kMontiDefaultFingerprintPreset);
  if (!preset.empty() && preset != fp.preset) {
    fp = Generate(preset, seed);
  }
  // Overlay the per-surface spoof modes.
  const base::DictValue& spoof = prefs->GetDict(prefs::kMontiDefaultSpoofModes);
  const auto overlay = [&spoof](const char* surface, std::string& field) {
    if (const std::string* v = spoof.FindString(surface); v && !v->empty()) {
      field = *v;
    }
  };
  overlay("webrtc", fp.webrtc_mode);
  overlay("canvas", fp.canvas_mode);
  overlay("webgl", fp.webgl_mode);
  overlay("webgpu", fp.webgpu_mode);
  overlay("client_rects", fp.client_rects_mode);
  overlay("audio", fp.audio_mode);
  // WebRTC leak protection forces a non-real WebRTC mode.
  if (prefs->GetBoolean(prefs::kMontiWebrtcLeakProtection) &&
      fp.webrtc_mode == "real") {
    fp.webrtc_mode = "noise";
  }
  return fp;
}

// Resolves the absolute path of a Chrome profile from its directory basename.
base::FilePath MontiProfileFullPath(const std::string& profile_dir) {
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return base::FilePath();
  }
  return user_data_dir.Append(base::FilePath::FromUTF8Unsafe(profile_dir));
}

void ApplySharedExtensionsToProfile(Profile* profile) {
  for (const base::FilePath& path : GetSharedExtensionPaths()) {
    if (path.empty() || !base::PathExists(path)) {
      continue;
    }
    std::string extension_id;
    extensions::UnpackedInstaller::Create(profile)->LoadFromCommandLine(
        path, &extension_id, /*only_allow_apps=*/false);
  }
}

const bookmarks::BookmarkNode* FindSharedBookmarkFolder(
    bookmarks::BookmarkModel* model) {
  const bookmarks::BookmarkNode* bookmark_bar = model->bookmark_bar_node();
  if (!bookmark_bar) {
    return nullptr;
  }
  for (const auto& child : bookmark_bar->children()) {
    std::string meta_value;
    if (child->is_folder() &&
        child->GetMetaInfo(kMontiSharedBookmarkFolderMetaKey, &meta_value) &&
        meta_value == kMontiSharedBookmarkFolderMetaValue) {
      return child.get();
    }
  }
  for (const auto& child : bookmark_bar->children()) {
    if (child->is_folder() &&
        child->GetTitle() == kMontiSharedBookmarkFolderTitle) {
      return child.get();
    }
  }
  return nullptr;
}

void ApplySharedBookmarksToLoadedModel(
    base::WeakPtr<bookmarks::BookmarkModel> weak_model) {
  if (!weak_model || !weak_model->loaded() || !weak_model->bookmark_bar_node()) {
    return;
  }

  bookmarks::BookmarkModel* model = weak_model.get();
  std::vector<SharedBookmark> shared_bookmarks = GetSharedBookmarks();
  const bookmarks::BookmarkNode* folder = FindSharedBookmarkFolder(model);
  if (shared_bookmarks.empty()) {
    if (folder) {
      model->Remove(folder, bookmarks::metrics::BookmarkEditSource::kOther,
                    FROM_HERE);
    }
    return;
  }

  if (!folder) {
    bookmarks::BookmarkNode::MetaInfoMap meta;
    meta[kMontiSharedBookmarkFolderMetaKey] =
        kMontiSharedBookmarkFolderMetaValue;
    folder = model->AddFolder(
        model->bookmark_bar_node(), model->bookmark_bar_node()->children().size(),
        kMontiSharedBookmarkFolderTitle, &meta);
  }

  model->BeginExtensiveChanges();
  while (!folder->children().empty()) {
    model->RemoveLastChild(folder, bookmarks::metrics::BookmarkEditSource::kOther,
                           FROM_HERE);
  }
  for (const SharedBookmark& bookmark : shared_bookmarks) {
    GURL url(bookmark.url);
    if (!url.is_valid()) {
      continue;
    }
    model->AddURL(folder, folder->children().size(),
                  base::UTF8ToUTF16(bookmark.title.empty() ? bookmark.url
                                                           : bookmark.title),
                  url);
  }
  model->EndExtensiveChanges();
}

void ApplySharedBookmarksToProfile(Profile* profile) {
  if (!profile) {
    return;
  }
  profile->GetPrefs()->SetBoolean(bookmarks::prefs::kShowBookmarkBar, true);
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(profile);
  if (!model) {
    return;
  }
  bookmarks::ScheduleCallbackOnBookmarkModelLoad(
      *model, base::BindOnce(&ApplySharedBookmarksToLoadedModel,
                             model->AsWeakPtr()));
}

Fingerprint ApplyProxyCountryDefaults(const Fingerprint& fp,
                                       const std::string& proxy_id) {
  if (proxy_id.empty()) {
    return fp;
  }
  const MontiProxy* proxy = ProxyStore::GetInstance()->GetById(proxy_id);
  if (!proxy || proxy->country.empty()) {
    return fp;
  }
  return ApplyCountryDefaults(fp, proxy->country);
}

}  // namespace

// static
MontiManager* MontiManager::GetInstance() {
  static base::NoDestructor<MontiManager> instance;
  return instance.get();
}

MontiManager::MontiManager() {
  browser_collection_observation_.Observe(GlobalBrowserCollection::GetInstance());
  // Kick the asynchronous profiles.json load now so the registry is populated
  // by the time a profile window opens. OnBrowserCreated reads the per-profile
  // fingerprint/UA/proxy from this store synchronously; starting the load at
  // manager construction (early in startup) keeps that lookup warm.
  ProfileStore::GetInstance();
}

MontiManager::~MontiManager() = default;

const std::vector<MontiProfileEntry>& MontiManager::GetProfiles() {
  return ProfileStore::GetInstance()->list();
}

bool MontiManager::AtAutomationLimit(int active_count) const {
  return false;
}

void MontiManager::CreateProfile(const std::string& name,
                                 const std::string& proxy_id,
                                 ResultCallback callback) {
  std::string profile_name = name.empty() ? "Monti Profile" : name;
  ProfileManager::CreateMultiProfileAsync(
      base::UTF8ToUTF16(profile_name), profiles::GetPlaceholderAvatarIndex(),
      /*is_hidden=*/false,
      base::BindOnce(&MontiManager::OnProfileCreated,
                     weak_factory_.GetWeakPtr(), profile_name, proxy_id,
                     std::move(callback)));
}

void MontiManager::CreateProfileFull(ProfileDraft draft,
                                     ResultCallback callback) {
  draft.name = draft.name.empty() ? "Monti Profile" : draft.name;
  const std::u16string title = base::UTF8ToUTF16(draft.name);
  ProfileManager::CreateMultiProfileAsync(
      title, profiles::GetPlaceholderAvatarIndex(),
      /*is_hidden=*/false,
      base::BindOnce(&MontiManager::OnProfileCreatedFull,
                     weak_factory_.GetWeakPtr(), std::move(draft),
                     std::move(callback)));
}

void MontiManager::GetProfileDraft(const std::string& id,
                                   DraftCallback callback) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);
  if (!entry) {
    std::move(callback).Run(false, ProfileDraft());
    return;
  }
  base::FilePath full_path = MontiProfileFullPath(entry->profile_dir);
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (Profile* loaded = profile_manager->GetProfileByPath(full_path)) {
    OnProfileLoadedForDraft(id, std::move(callback), loaded);
    return;
  }
  profile_manager->CreateProfileAsync(
      full_path, base::BindOnce(&MontiManager::OnProfileLoadedForDraft,
                                weak_factory_.GetWeakPtr(), id,
                                std::move(callback)));
}

void MontiManager::UpdateProfileFull(const std::string& id,
                                     ProfileDraft draft,
                                     ResultCallback callback) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);
  if (!entry) {
    std::move(callback).Run(false, "No such profile.");
    return;
  }
  draft.name = draft.name.empty() ? entry->name : draft.name;
  base::FilePath full_path = MontiProfileFullPath(entry->profile_dir);
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  if (Profile* loaded = profile_manager->GetProfileByPath(full_path)) {
    OnProfileLoadedForFullUpdate(id, std::move(draft), std::move(callback),
                                 loaded);
    return;
  }
  profile_manager->CreateProfileAsync(
      full_path, base::BindOnce(&MontiManager::OnProfileLoadedForFullUpdate,
                                weak_factory_.GetWeakPtr(), id,
                                std::move(draft), std::move(callback)));
}

void MontiManager::OnProfileCreatedFull(ProfileDraft draft,
                                        ResultCallback callback,
                                        Profile* profile) {
  if (!profile) {
    std::move(callback).Run(false, "Profile creation failed.");
    return;
  }
  draft.fingerprint =
      ApplyProxyCountryDefaults(draft.fingerprint, draft.proxy_id);
  std::string id = profile->GetBaseName().AsUTF8Unsafe();

  // Register the profile with the draft's meta + identity straight away so the
  // card renders with its tags/folder and the launched window gets a coherent
  // UA (keyed to the fingerprint's preset).
  MontiProfileEntry entry;
  entry.id = id;
  entry.name = draft.name;
  entry.profile_dir = id;
  entry.assigned_proxy_id = std::string();
  entry.created_at = NowForProfileCreatedAt();
  entry.profile_status = draft.status;
  entry.color = draft.color;
  entry.tags = draft.tags;
  entry.folder_id = draft.folder_id;
  entry.ua_preset = draft.fingerprint.preset;
  entry.fingerprint = ToJson(draft.fingerprint);
  ProfileStore::GetInstance()->Add(entry);
  CreateOrUpdateProfileLauncher(
      entry, GetSharedExtensionPaths(), ProfileLaunchSwitches(entry));

  // Mirror everything into the per-profile service: name + fingerprint (for
  // session-17 enforcement), the encrypted credentials, and the additive
  // cookie/status/type/start-page bookkeeping.
  MontiProfileService* service =
      MontiProfileServiceFactory::GetForProfile(profile);
  service->SetName(draft.name);
  service->SetFingerprint(ToDict(draft.fingerprint));
  service->SetCredentials(std::move(draft.credentials));
  service->SetCookies(draft.cookies);
  service->SetStatus(draft.status);
  service->SetProfileType(draft.profile_type);
  service->SetStartPages(draft.start_pages);

  // Assign + apply the proxy (also mirrors it into the profile service).
  SetProfileProxy(id, draft.proxy_id, profile);

  NotifyProfilesChanged();
  std::move(callback).Run(true, "Created profile \"" + draft.name + "\".");
}

void MontiManager::OnProfileLoadedForDraft(const std::string& id,
                                           DraftCallback callback,
                                           Profile* profile) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);
  if (!entry || !profile) {
    std::move(callback).Run(false, ProfileDraft());
    return;
  }

  MontiProfileService* service =
      MontiProfileServiceFactory::GetForProfile(profile);
  ProfileDraft draft;
  draft.name = entry->name;
  draft.proxy_id = entry->assigned_proxy_id;
  draft.folder_id = entry->folder_id;
  draft.tags = entry->tags;
  draft.status = service->status();
  if (draft.status.empty()) {
    draft.status = entry->profile_status;
  }
  draft.color = entry->color;
  draft.profile_type = service->profile_type();
  draft.start_pages = service->start_pages();
  draft.cookies = service->cookies();
  draft.fingerprint = GetFingerprint(id);
  service->GetCredentials(base::BindOnce(
      &MontiManager::OnCredentialsLoadedForDraft, weak_factory_.GetWeakPtr(),
      std::move(draft), std::move(callback)));
}

void MontiManager::OnCredentialsLoadedForDraft(
    ProfileDraft draft,
    DraftCallback callback,
    std::vector<ProfileCredential> credentials) {
  draft.credentials = std::move(credentials);
  std::move(callback).Run(true, std::move(draft));
}

void MontiManager::OnProfileLoadedForFullUpdate(const std::string& id,
                                                ProfileDraft draft,
                                                ResultCallback callback,
                                                Profile* profile) {
  const MontiProfileEntry* existing = ProfileStore::GetInstance()->GetById(id);
  if (!existing || !profile) {
    std::move(callback).Run(false, "Profile load failed.");
    return;
  }

  MontiProfileEntry entry = *existing;
  draft.fingerprint =
      ApplyProxyCountryDefaults(draft.fingerprint, draft.proxy_id);
  entry.name = draft.name;
  entry.tags = draft.tags;
  entry.profile_status = draft.status;
  entry.color = draft.color;
  entry.folder_id = draft.folder_id;
  entry.ua_preset = draft.fingerprint.preset;
  entry.fingerprint = ToJson(draft.fingerprint);
  ProfileStore::GetInstance()->Update(entry);
  CreateOrUpdateProfileLauncher(
      entry, GetSharedExtensionPaths(), ProfileLaunchSwitches(entry));

  MontiProfileService* service =
      MontiProfileServiceFactory::GetForProfile(profile);
  service->SetName(draft.name);
  service->SetFingerprint(ToDict(draft.fingerprint));
  service->SetCredentials(std::move(draft.credentials));
  service->SetCookies(draft.cookies);
  service->SetStatus(draft.status);
  service->SetProfileType(draft.profile_type);
  service->SetStartPages(draft.start_pages);

  SetProfileProxy(id, draft.proxy_id, profile);
  NotifyProfilesChanged();
  std::move(callback).Run(true, "Updated profile \"" + draft.name + "\".");
}

void MontiManager::CloneProfile(const std::string& id, ResultCallback callback) {
  const MontiProfileEntry* src = ProfileStore::GetInstance()->GetById(id);
  if (!src) {
    std::move(callback).Run(false, "No such profile.");
    return;
  }
  // Default: copy the source's proxy assignment (per session-04 spec).
  std::string new_name = src->name + " (copy)";
  std::string proxy_id = src->assigned_proxy_id;
  ProfileManager::CreateMultiProfileAsync(
      base::UTF8ToUTF16(new_name), profiles::GetPlaceholderAvatarIndex(),
      /*is_hidden=*/false,
      base::BindOnce(&MontiManager::OnProfileCreated,
                     weak_factory_.GetWeakPtr(), new_name, proxy_id,
                     std::move(callback)));
}

void MontiManager::OnProfileCreated(const std::string& name,
                                    const std::string& proxy_id,
                                    ResultCallback callback,
                                    Profile* profile) {
  if (!profile) {
    std::move(callback).Run(false, "Profile creation failed.");
    return;
  }
  std::string id = profile->GetBaseName().AsUTF8Unsafe();

  MontiProfileEntry entry;
  entry.id = id;
  entry.name = name;
  entry.profile_dir = id;
  entry.assigned_proxy_id = std::string();
  entry.created_at = NowForProfileCreatedAt();
  entry.profile_status = "New";

  // Seed a coherent fingerprint keyed to the (initially empty) UA preset, then
  // overlay the user's anti-detect defaults from chrome://monti-settings (read
  // off the manager profile, never the just-created one). It is re-rolled
  // coherently if the user later picks a preset in the manager.
  const uint32_t seed = static_cast<uint32_t>(base::RandUint64());
  Profile* source = ProfileManager::GetLastUsedProfileIfLoaded();
  Fingerprint fp = ApplyDefaultFingerprintPrefs(
      Generate(entry.ua_preset, seed), source, profile, seed);
  fp = ApplyProxyCountryDefaults(fp, proxy_id);
  entry.ua_preset = fp.preset;
  entry.fingerprint = ToJson(fp);
  ProfileStore::GetInstance()->Add(entry);
  CreateOrUpdateProfileLauncher(
      entry, GetSharedExtensionPaths(), ProfileLaunchSwitches(entry));

  // Seed per-profile metadata (name + fingerprint mirror for session-17 reads).
  MontiProfileService* service =
      MontiProfileServiceFactory::GetForProfile(profile);
  service->SetName(name);
  service->SetFingerprint(ToDict(fp));

  // Assign + apply the proxy (also mirrors it into the profile service).
  SetProfileProxy(id, proxy_id, profile);

  NotifyProfilesChanged();
  std::move(callback).Run(true, "Created profile \"" + name + "\".");
}

void MontiManager::AssignProxy(const std::string& profile_id,
                               const std::string& proxy_id) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(profile_id);
  if (!entry) {
    return;
  }
  base::FilePath full_path = MontiProfileFullPath(entry->profile_dir);
  Profile* loaded =
      g_browser_process->profile_manager()->GetProfileByPath(full_path);
  SetProfileProxy(profile_id, proxy_id, loaded);
  NotifyProfilesChanged();
}

void MontiManager::SetProfileProxy(const std::string& profile_id,
                                   const std::string& proxy_id,
                                   Profile* loaded_profile) {
  ProfileStore* profile_store = ProfileStore::GetInstance();
  ProxyStore* proxy_store = ProxyStore::GetInstance();

  const MontiProfileEntry* existing = profile_store->GetById(profile_id);
  if (!existing) {
    return;
  }
  MontiProfileEntry entry = *existing;

  // Release the previously-assigned proxy, if it changed.
  if (!entry.assigned_proxy_id.empty() &&
      entry.assigned_proxy_id != proxy_id) {
    if (const MontiProxy* old = proxy_store->GetById(entry.assigned_proxy_id)) {
      if (old->assigned_profile == profile_id) {
        MontiProxy updated = *old;
        updated.assigned_profile.clear();
        proxy_store->Update(updated);
      }
    }
  }

  entry.assigned_proxy_id = proxy_id;
  profile_store->Update(entry);

  MontiProfileService* service =
      loaded_profile
          ? MontiProfileServiceFactory::GetForProfile(loaded_profile)
          : nullptr;

  if (!proxy_id.empty()) {
    if (const MontiProxy* proxy = proxy_store->GetById(proxy_id)) {
      MontiProxy updated = *proxy;
      updated.assigned_profile = profile_id;
      proxy_store->Update(updated);
      // Drive the running profile's service through Connect() so it applies the
      // proxy, verifies egress, persists the assigned/saved id, and flips the
      // connection state (and the toolbar chip) to Connected live.
      if (service) {
        service->Connect(*proxy, base::DoNothing());
      }
    }
  } else if (service) {
    // Assignment cleared: revert the running profile to a direct connection.
    service->Disconnect(base::DoNothing());
  }
}

void MontiManager::LaunchProfile(const std::string& id, LaunchCallback callback) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);
  if (!entry) {
    std::move(callback).Run(LaunchOutcome::kLoadFailed, "No such profile.");
    return;
  }
  std::string profile_id = entry->id;

  // Refuse to open a second window for a profile that already has one running.
  if (IsRunning(profile_id)) {
    std::move(callback).Run(LaunchOutcome::kConcurrencyLimited,
                            "Profile is already open.");
    return;
  }

  base::FilePath full_path = MontiProfileFullPath(entry->profile_dir);
  ProfileManager* profile_manager = g_browser_process->profile_manager();

  if (Profile* loaded = profile_manager->GetProfileByPath(full_path)) {
    OnProfileLoadedForLaunch(profile_id, std::move(callback), loaded);
    return;
  }

  // Starting a not-yet-running profile is gated by the concurrency cap.
  if (live_profile_count() >= concurrency_cap_) {
    std::move(callback).Run(
        LaunchOutcome::kConcurrencyLimited,
        "Concurrency limit reached (" +
            base::NumberToString(live_profile_count()) + " running).");
    return;
  }

  profile_manager->CreateProfileAsync(
      full_path,
      base::BindOnce(&MontiManager::OnProfileLoadedForLaunch,
                     weak_factory_.GetWeakPtr(), profile_id,
                     std::move(callback)));
}

void MontiManager::OnProfileLoadedForLaunch(const std::string& profile_id,
                                            LaunchCallback callback,
                                            Profile* profile) {
  if (!profile) {
    std::move(callback).Run(LaunchOutcome::kLoadFailed,
                            "Failed to load profile.");
    return;
  }
  // Drive the assigned proxy through the canonical Connect() path so the
  // freshly-created service applies the proxy, verifies egress, and flips its
  // connection_state to Connected. ApplyProxyToProfile alone only seeds the
  // network context's proxy pref and never updates the state, which left the
  // toolbar status chip permanently stuck on "Direct".
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(profile_id);
  MontiProfileService* service =
      MontiProfileServiceFactory::GetForProfile(profile);
  if (entry && !entry->assigned_proxy_id.empty()) {
    if (const MontiProxy* proxy =
            ProxyStore::GetInstance()->GetById(entry->assigned_proxy_id)) {
      // Skip re-verifying when this identity is already connected to the same
      // proxy (e.g. opening a second window for a running identity), so the chip
      // doesn't flicker through Connecting again.
      const bool already_connected =
          service->connection_state() ==
              MontiProfileService::ConnectionState::kConnected &&
          service->connected_proxy_id() == proxy->id;
      if (!already_connected) {
        service->Connect(
            *proxy,
            base::BindOnce(&MontiManager::OnLaunchProxyConnected,
                           weak_factory_.GetWeakPtr(), profile_id,
                           std::move(callback), profile));
        return;
      }
    } else {
      std::move(callback).Run(LaunchOutcome::kLoadFailed,
                              "Assigned proxy was not found.");
      return;
    }
  }
  OpenLaunchedProfileWindow(profile_id, std::move(callback), profile);
}

void MontiManager::OnLaunchProxyConnected(const std::string& profile_id,
                                          LaunchCallback callback,
                                          Profile* profile,
                                          bool success,
                                          const std::string& error) {
  if (!success) {
    std::move(callback).Run(
        LaunchOutcome::kLoadFailed,
        error.empty() ? "Assigned proxy failed to connect." : error);
    return;
  }
  OpenLaunchedProfileWindow(profile_id, std::move(callback), profile);
}

void MontiManager::OpenLaunchedProfileWindow(const std::string& profile_id,
                                             LaunchCallback callback,
                                             Profile* profile) {
  MontiProfileService* service =
      MontiProfileServiceFactory::GetForProfile(profile);
  // Mirror the persisted fingerprint into the now-loaded profile's prefs so the
  // renderer/launch enforcement (session 17) can read it back.
  service->SetFingerprint(ToDict(GetFingerprint(profile_id)));
  ApplySharedBookmarksToProfile(profile);
  ApplySharedExtensionsToProfile(profile);
  Browser::CreateParams params(Browser::TYPE_NORMAL, profile,
                               /*user_gesture=*/true);
  params.should_trigger_session_restore = false;
  Browser* browser = Browser::Create(params);
  std::vector<GURL> launch_urls;
  for (const std::string& start_page : service->start_pages()) {
    GURL url(start_page);
    if (url.is_valid()) {
      launch_urls.push_back(url);
    }
  }
  if (launch_urls.empty()) {
    launch_urls.emplace_back("chrome://monti-newtab");
  }
  bool foreground = true;
  for (const GURL& url : launch_urls) {
    chrome::AddTabAt(browser, url, /*index=*/-1, foreground);
    foreground = false;
  }
  browser->window()->Show();
  std::move(callback).Run(LaunchOutcome::kLaunched, "Launched profile.");
}

void MontiManager::DeleteProfile(const std::string& id, ResultCallback callback) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);
  if (!entry) {
    std::move(callback).Run(false, "No such profile.");
    return;
  }
  std::string profile_id = entry->id;
  std::string name = entry->name;

  // Release any proxy assignment before forgetting the profile.
  if (!entry->assigned_proxy_id.empty()) {
    if (const MontiProxy* proxy =
            ProxyStore::GetInstance()->GetById(entry->assigned_proxy_id)) {
      if (proxy->assigned_profile == profile_id) {
        MontiProxy updated = *proxy;
        updated.assigned_profile.clear();
        ProxyStore::GetInstance()->Update(updated);
      }
    }
  }
  RemoveProfileLauncher(entry->profile_dir);
  ProfileStore::GetInstance()->Remove(profile_id);

  ProfileManager* profile_manager = g_browser_process->profile_manager();
  profile_manager->GetDeleteProfileHelper().MaybeScheduleProfileForDeletion(
      MontiProfileFullPath(entry->profile_dir), base::DoNothing(),
      ProfileMetrics::DELETE_PROFILE_USER_MANAGER);

  NotifyProfilesChanged();
  std::move(callback).Run(true, "Deleted profile \"" + name + "\".");
}

void MontiManager::UpdateProfileMeta(const std::string& id,
                                     std::vector<std::string> tags,
                                     const std::string& notes,
                                     const std::string& color,
                                     const std::string& folder_id,
                                     const std::string& ua_preset) {
  const MontiProfileEntry* existing = ProfileStore::GetInstance()->GetById(id);
  if (!existing) {
    return;
  }
  MontiProfileEntry entry = *existing;
  entry.tags = std::move(tags);
  entry.notes = notes;
  entry.color = color;
  entry.folder_id = folder_id;
  entry.ua_preset = ua_preset;
  ProfileStore::GetInstance()->Update(entry);
  CreateOrUpdateProfileLauncher(
      entry, GetSharedExtensionPaths(), ProfileLaunchSwitches(entry));
  NotifyProfilesChanged();
}

void MontiManager::UpdateProfileStatus(const std::string& id,
                                       const std::string& status) {
  const MontiProfileEntry* existing = ProfileStore::GetInstance()->GetById(id);
  if (!existing) {
    return;
  }
  MontiProfileEntry entry = *existing;
  entry.profile_status = status;
  ProfileStore::GetInstance()->Update(entry);

  base::FilePath full_path = MontiProfileFullPath(entry.profile_dir);
  if (Profile* loaded =
          g_browser_process->profile_manager()->GetProfileByPath(full_path)) {
    MontiProfileServiceFactory::GetForProfile(loaded)->SetStatus(status);
  }
  NotifyProfilesChanged();
}

Fingerprint MontiManager::GetFingerprint(const std::string& profile_id) {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(profile_id);
  if (!entry) {
    return Fingerprint();
  }
  if (!entry->fingerprint.empty()) {
    return FromJson(entry->fingerprint);
  }
  // First read of an as-yet-ungenerated fingerprint: roll a coherent one keyed
  // to the profile's UA preset, persist it, and return it.
  Fingerprint fp =
      Generate(entry->ua_preset, static_cast<uint32_t>(base::RandUint64()));
  PersistFingerprint(profile_id, fp);
  return fp;
}

Fingerprint MontiManager::GenerateFingerprint(const std::string& profile_id,
                                              const std::string& preset) {
  Fingerprint existing = GetFingerprint(profile_id);
  const MontiProfileEntry* entry =
      ProfileStore::GetInstance()->GetById(profile_id);
  Fingerprint fp =
      Generate(preset, static_cast<uint32_t>(base::RandUint64()));
  fp = ApplyProxyCountryDefaults(
      fp, entry ? entry->assigned_proxy_id : std::string());

  // A re-roll regenerates the hardware identity but must preserve every config
  // field the user owns (spoof modes, geolocation, ports, DNT, switches, flags,
  // and Notes); Generate() leaves these at defaults.
  fp.webrtc_mode = existing.webrtc_mode;
  fp.canvas_mode = existing.canvas_mode;
  fp.webgl_mode = existing.webgl_mode;
  fp.webgpu_mode = existing.webgpu_mode;
  fp.client_rects_mode = existing.client_rects_mode;
  fp.audio_mode = existing.audio_mode;
  if (existing.geolocation_mode != "manual" ||
      existing.latitude != 0 || existing.longitude != 0) {
    fp.geolocation_mode = existing.geolocation_mode;
    fp.latitude = existing.latitude;
    fp.longitude = existing.longitude;
  }
  fp.ports_to_protect = existing.ports_to_protect;
  fp.do_not_track = existing.do_not_track;
  fp.command_line_switches = existing.command_line_switches;
  fp.hide_profile_name = existing.hide_profile_name;
  fp.video_cookie_spoof = existing.video_cookie_spoof;
  fp.substitute_name_icon = existing.substitute_name_icon;
  fp.note_text = existing.note_text;
  fp.note_icon = existing.note_icon;
  fp.note_color = existing.note_color;
  fp.note_style = existing.note_style;

  PersistFingerprint(profile_id, fp);
  return fp;
}

Fingerprint MontiManager::PreviewFingerprint(const std::string& preset,
                                             uint32_t seed) {
  return Generate(preset, seed);
}

void MontiManager::SetFingerprint(const std::string& profile_id,
                                  const Fingerprint& fp) {
  PersistFingerprint(profile_id, fp);
}

void MontiManager::PersistFingerprint(const std::string& profile_id,
                                      const Fingerprint& fp) {
  ProfileStore* store = ProfileStore::GetInstance();
  const MontiProfileEntry* existing = store->GetById(profile_id);
  if (!existing) {
    return;
  }
  MontiProfileEntry entry = *existing;
  entry.fingerprint = ToJson(fp);
  store->Update(entry);

  // Keep the loaded profile's prefs mirror fresh for session-17 enforcement.
  base::FilePath full_path = MontiProfileFullPath(entry.profile_dir);
  if (Profile* loaded =
          g_browser_process->profile_manager()->GetProfileByPath(full_path)) {
    MontiProfileServiceFactory::GetForProfile(loaded)->SetFingerprint(
        ToDict(fp));
  }
}

const std::vector<Folder>& MontiManager::GetFolders() {
  return FolderStore::GetInstance()->list();
}

void MontiManager::CreateFolder(const std::string& parent_id,
                                const std::string& name) {
  Folder folder;
  folder.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  folder.parent_id = parent_id;
  folder.name = name.empty() ? "Folder" : name;
  FolderStore::GetInstance()->Add(folder);
  NotifyExternalStateChanged();
}

void MontiManager::RenameFolder(const std::string& id,
                                const std::string& name) {
  const Folder* existing = FolderStore::GetInstance()->GetById(id);
  if (!existing) {
    return;
  }
  Folder folder = *existing;
  folder.name = name.empty() ? "Folder" : name;
  FolderStore::GetInstance()->Update(folder);
  NotifyExternalStateChanged();
}

void MontiManager::DeleteFolder(const std::string& id) {
  if (id.empty() || !FolderStore::GetInstance()->GetById(id)) {
    return;
  }

  FolderStore::GetInstance()->Remove(id);
  ProfileStore* profiles = ProfileStore::GetInstance();
  std::vector<MontiProfileEntry> to_update;
  for (const MontiProfileEntry& profile : profiles->list()) {
    if (profile.folder_id == id) {
      MontiProfileEntry updated = profile;
      updated.folder_id.clear();
      to_update.push_back(std::move(updated));
    }
  }
  for (const MontiProfileEntry& profile : to_update) {
    profiles->Update(profile);
  }
  NotifyExternalStateChanged();
}

const std::vector<MontiProxy>& MontiManager::GetProxies() {
  return ProxyStore::GetInstance()->list();
}

void MontiManager::LaunchProfiles(std::vector<std::string> ids,
                                  BulkLaunchCallback callback) {
  auto state = std::make_unique<BulkLaunchState>();
  state->ids = std::move(ids);
  state->callback = std::move(callback);
  LaunchNextInBatch(std::move(state));
}

void MontiManager::LaunchNextInBatch(std::unique_ptr<BulkLaunchState> state) {
  if (state->index >= state->ids.size()) {
    std::move(state->callback).Run(state->launched, state->skipped);
    return;
  }
  const std::string id = state->ids[state->index];
  LaunchProfile(id, base::BindOnce(&MontiManager::OnBatchLaunchResult,
                                   weak_factory_.GetWeakPtr(),
                                   std::move(state)));
}

void MontiManager::OnBatchLaunchResult(std::unique_ptr<BulkLaunchState> state,
                                       LaunchOutcome outcome,
                                       const std::string& message) {
  if (outcome == LaunchOutcome::kLaunched) {
    ++state->launched;
  } else {
    ++state->skipped;
  }
  ++state->index;
  LaunchNextInBatch(std::move(state));
}

void MontiManager::DeleteProfiles(std::vector<std::string> ids,
                                  base::OnceClosure callback) {
  for (const std::string& id : ids) {
    DeleteProfile(id, base::DoNothing());
  }
  std::move(callback).Run();
}

bool MontiManager::IsRunning(const std::string& id) const {
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);
  if (!entry) {
    return false;
  }
  return window_counts_.count(MontiProfileFullPath(entry->profile_dir)) > 0;
}

void MontiManager::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void MontiManager::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void MontiManager::NotifyExternalStateChanged() {
  NotifyProfilesChanged();
}

void MontiManager::NotifyProfilesChanged() {
  for (Observer& observer : observers_) {
    observer.OnMontiProfilesChanged();
  }
}

int MontiManager::live_profile_count() const {
  return static_cast<int>(window_counts_.size());
}

void MontiManager::OnBrowserCreated(BrowserWindowInterface* browser) {
  Profile* profile = browser->GetProfile();
  if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession()) {
    return;
  }
  const int before = window_counts_[profile->GetPath()];
  window_counts_[profile->GetPath()] = before + 1;

  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  // An external launcher (Monti Anty) spawns the browser with
  // --user-data-dir=.../Profiles/<id> --profile-directory=Default, so the
  // profile path's basename is "Default", not the Monti profile id. Prefer
  // the id the launcher passed explicitly so ProfileStore lookups (and thus
  // fingerprint injection) resolve correctly for those sessions.
  const bool is_external_launch =
      command_line.HasSwitch("monti-profile-launch");
  const std::string id = is_external_launch &&
                                 command_line.HasSwitch("monti-profile-id")
                             ? command_line.GetSwitchValueASCII(
                                   "monti-profile-id")
                             : profile->GetPath().BaseName().AsUTF8Unsafe();
  const MontiProfileEntry* entry = ProfileStore::GetInstance()->GetById(id);

  // Since the per-profile wrapper .app was removed, every launch shares the
  // same "Monti Browser" process/dock identity, so the profile's name is no
  // longer visible anywhere. Set it as this window's user title (the same
  // mechanism behind Chrome's own tab-strip "Name Window..." feature) so it
  // still shows in the title bar, Window menu, and Mission Control.
  const std::string display_name =
      is_external_launch ? command_line.GetSwitchValueASCII(
                                "monti-profile-name")
                          : (entry ? entry->name : std::string());
  if (!display_name.empty()) {
    if (auto* metadata = WindowMetadataController::From(browser)) {
      metadata->SetWindowUserTitle(display_name);
    }
  }

  if (!entry) {
    if (!is_external_launch) {
      return;  // Not an Monti-managed profile (e.g. the manager UI window).
    }
    // External launcher session with no matching ProfileStore record (the
    // profile directory was never created through this manager). Build a
    // runtime-only identity from the launcher's --monti-fingerprint-json
    // instead of silently skipping fingerprint injection.
    std::optional<Fingerprint> fp =
        DecodeRuntimeFingerprintSwitch(command_line);
    if (!fp) {
      return;
    }
    std::optional<blink::UserAgentOverride> ua_override =
        MontiUserAgentFor(fp->preset);
    if (!ua_override && !fp->ua_string.empty()) {
      ua_override = blink::UserAgentOverride::UserAgentOnly(fp->ua_string);
    }
    const bool proxy_assigned = command_line.HasSwitch("monti-proxy-host");
    blink::mojom::WebRtcIpHandlingPolicy webrtc_policy =
        (proxy_assigned || fp->webrtc_mode == "off" ||
         fp->webrtc_mode == "noise")
            ? blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp
            : blink::mojom::WebRtcIpHandlingPolicy::kDefault;
    new MontiUaTabHelper(browser->GetTabStripModel(), std::move(ua_override),
                         ToJson(*fp), webrtc_policy);
    return;
  }
  // Apply this profile's anti-detect identity to the new window: the session-14
  // User-Agent / UA-CH override (if the preset produces one) and the session-17
  // fingerprint (if present). Host-UA profiles still get a fingerprint, so the
  // helper is created when *either* is meaningful. The tab helper is self-owned
  // and tears itself down when the window closes.
  std::optional<blink::UserAgentOverride> ua_override =
      MontiUserAgentFor(entry->ua_preset);
  const std::string& fingerprint_json = entry->fingerprint;
  if (ua_override || !fingerprint_json.empty()) {
    // Map the fingerprint's WebRTC mode onto a network-layer IP handling policy
    // (real local-IP leak protection that JS spoofing alone cannot provide).
    blink::mojom::WebRtcIpHandlingPolicy webrtc_policy =
        blink::mojom::WebRtcIpHandlingPolicy::kDefault;
    if (!entry->assigned_proxy_id.empty()) {
      webrtc_policy =
          blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
    }
    if (!fingerprint_json.empty()) {
      const std::string mode = FromJson(fingerprint_json).webrtc_mode;
      if (mode == "off") {
        webrtc_policy = blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
      } else if (mode == "noise") {
        webrtc_policy =
            blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
      }
    }
    new MontiUaTabHelper(browser->GetTabStripModel(), std::move(ua_override),
                         fingerprint_json, webrtc_policy);
  }
  // The first window for a registered Monti profile means it just started
  // running; tell observers so the manager flips its status dot live.
  if (before == 0) {
    for (Observer& observer : observers_) {
      observer.OnMontiProfileStatusChanged(id, "running");
    }
  }
}

void MontiManager::OnBrowserClosed(BrowserWindowInterface* browser) {
  Profile* profile = browser->GetProfile();
  if (!profile || profile->IsOffTheRecord() || profile->IsGuestSession()) {
    return;
  }
  auto it = window_counts_.find(profile->GetPath());
  if (it == window_counts_.end()) {
    return;
  }
  if (--it->second <= 0) {
    window_counts_.erase(it);
    // Last window closed: the profile is now idle.
    const std::string id = profile->GetPath().BaseName().AsUTF8Unsafe();
    if (ProfileStore::GetInstance()->GetById(id)) {
      for (Observer& observer : observers_) {
        observer.OnMontiProfileStatusChanged(id, "idle");
      }
    }
  }
}

MontiManager::BulkLaunchState::BulkLaunchState() = default;
MontiManager::BulkLaunchState::~BulkLaunchState() = default;

}  // namespace monti
