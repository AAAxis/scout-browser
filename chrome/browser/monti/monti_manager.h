// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_MANAGER_H_
#define CHROME_BROWSER_MONTI_MONTI_MANAGER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/scoped_observation.h"
#include "chrome/browser/monti/monti_fingerprint.h"
#include "chrome/browser/monti/monti_folder.h"
#include "chrome/browser/monti/monti_profile_draft.h"
#include "chrome/browser/monti/monti_profile_entry.h"
#include "chrome/browser/monti/monti_proxy.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

class BrowserWindowInterface;
class GlobalBrowserCollection;
class Profile;

namespace monti {

// Process-wide facade that owns every profile + proxy operation: it composes
// the monti/ service layer (ProfileStore, ProxyStore, MontiProfileService,
// proxy_applicator) and drives Chrome's ProfileManager. It is the single
// implementation both the debug page (chrome://monti-debug) and the future
// chrome://monti page delegate to, so the proven create/clone/launch/delete
// flows live in exactly one place.
//
// Holds no profile state of its own; the stores remain the source of truth.
// All mutating operations are async + non-blocking (the DCHECK build forbids
// UI-thread blocking I/O) and report through a base::OnceCallback. Intended for
// the UI thread only.
class MontiManager : public BrowserCollectionObserver {
 public:
  // Outcome of a LaunchProfile() attempt.
  enum class LaunchOutcome {
    kLaunched,            // a window was opened for the profile
    kConcurrencyLimited,  // refused: too many profiles already running
    kLoadFailed,          // unknown profile, or the profile failed to load
  };

  // Result of a create/clone/delete operation. `message` is human-readable and
  // suitable for the debug page status line.
  using ResultCallback =
      base::OnceCallback<void(bool ok, const std::string& message)>;
  using LaunchCallback =
      base::OnceCallback<void(LaunchOutcome outcome, const std::string& message)>;
  // Reports a bulk LaunchProfiles() result: how many windows were opened and
  // how many ids were skipped (concurrency-limited or failed to load).
  using BulkLaunchCallback =
      base::OnceCallback<void(uint32_t launched, uint32_t skipped)>;
  using DraftCallback = base::OnceCallback<void(bool ok, ProfileDraft draft)>;

  // Observes coarse manager state so live surfaces (chrome://monti) can refresh
  // without polling. Notifications fire on the UI thread.
  class Observer : public base::CheckedObserver {
   public:
    // The set/metadata of registered profiles changed (create/clone/delete/
    // assign-proxy/metadata edit). Observers should re-read GetProfiles().
    virtual void OnMontiProfilesChanged() {}
    // A profile's running status changed. `status` is "running" | "idle".
    virtual void OnMontiProfileStatusChanged(const std::string& id,
                                             const std::string& status) {}
  };

  static MontiManager* GetInstance();

  MontiManager(const MontiManager&) = delete;
  MontiManager& operator=(const MontiManager&) = delete;

  // Lists every registered Monti profile straight from ProfileStore, without
  // loading any Chrome Profile (cheap; supports hundreds registered).
  const std::vector<MontiProfileEntry>& GetProfiles();

  // Creates a new profile named `name` (defaults to "Monti Profile" if empty)
  // and assigns `proxy_id` (may be empty for none).
  void CreateProfile(const std::string& name,
                     const std::string& proxy_id,
                     ResultCallback callback);

  // Creates a fully-specified profile from the New-profile dialog draft: the
  // meta (name/proxy/folder/tags/status/type), the seeded start pages, the
  // cookie draft blob, the session-19 fingerprint, and the OSCrypt-encrypted
  // credentials are all persisted in one shot.
  void CreateProfileFull(ProfileDraft draft, ResultCallback callback);

  // Loads the existing profile metadata and per-profile draft fields used by
  // the full create/edit form.
  void GetProfileDraft(const std::string& id, DraftCallback callback);

  // Updates an existing profile from the same full form shape used to create it.
  void UpdateProfileFull(const std::string& id,
                         ProfileDraft draft,
                         ResultCallback callback);

  // Creates a copy of the profile `id`, inheriting its name (with " (copy)")
  // and its assigned proxy.
  void CloneProfile(const std::string& id, ResultCallback callback);

  // Forgets the profile `id`: releases its proxy assignment and schedules the
  // underlying Chrome profile for deletion on disk.
  void DeleteProfile(const std::string& id, ResultCallback callback);

  // Assigns `proxy_id` (empty to clear) to the profile `profile_id`, keeping
  // ProfileStore/ProxyStore/MontiProfileService in sync and applying it
  // immediately if that profile is currently loaded.
  void AssignProxy(const std::string& profile_id, const std::string& proxy_id);

  // Loads (if needed) the profile `id`, re-applies its proxy so the fresh
  // NetworkContext auth cache is seeded, and opens a browser window. Gated by
  // the concurrency cap when it would start a not-yet-running profile.
  void LaunchProfile(const std::string& id, LaunchCallback callback);

  // Updates the manager metadata (tags/notes/color/folder) for profile `id` in
  // ProfileStore. No-op if the profile is unknown.
  void UpdateProfileMeta(const std::string& id,
                         std::vector<std::string> tags,
                         const std::string& notes,
                         const std::string& color,
                         const std::string& folder_id,
                         const std::string& ua_preset);

  // Updates only the user's table status label for profile `id`.
  void UpdateProfileStatus(const std::string& id, const std::string& status);

  // Returns profile `profile_id`'s persisted fingerprint, lazily generating and
  // persisting a fresh coherent one (keyed to the profile's UA preset) the
  // first time if none exists. Returns a default fingerprint for an unknown id.
  Fingerprint GetFingerprint(const std::string& profile_id);

  // Re-rolls a fresh seed for profile `profile_id` using `preset`, preserving
  // the user's config fields (spoof modes, ports, DNT, switches, flags, Notes)
  // from the existing fingerprint, persists it, and returns it.
  Fingerprint GenerateFingerprint(const std::string& profile_id,
                                  const std::string& preset);

  // Profile-less coherent preview for the create dialog's Summary card. Pure and
  // deterministic in `seed`; nothing is persisted.
  Fingerprint PreviewFingerprint(const std::string& preset, uint32_t seed);

  // Persists a hand-edited fingerprint (Advanced editor save) for `profile_id`.
  void SetFingerprint(const std::string& profile_id, const Fingerprint& fp);

  // The folders the user has created to organize profiles.
  const std::vector<Folder>& GetFolders();

  // Creates a folder named `name` under `parent_id` (empty == top level).
  void CreateFolder(const std::string& parent_id, const std::string& name);

  // Renames an existing folder.
  void RenameFolder(const std::string& id, const std::string& name);

  // Deletes a folder and moves any profiles inside it back to "All profiles".
  void DeleteFolder(const std::string& id);

  // The user's proxy list (for the manager's proxy dropdown / assigned chip).
  const std::vector<MontiProxy>& GetProxies();

  // Launches each id in `ids` sequentially, respecting the concurrency cap, and
  // reports how many windows opened vs. were skipped.
  void LaunchProfiles(std::vector<std::string> ids, BulkLaunchCallback callback);

  // Deletes each id in `ids`, then runs `callback` once all are forgotten.
  void DeleteProfiles(std::vector<std::string> ids, base::OnceClosure callback);

  // Whether profile `id` currently has at least one open window.
  bool IsRunning(const std::string& id) const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Called when another Monti subsystem (cloud sync/import) replaces the stores
  // underneath the manager. Observers should re-read profiles/proxies/folders.
  void NotifyExternalStateChanged();

  // Number of distinct profiles with at least one open window.
  int live_profile_count() const;

  // Maximum number of simultaneously-running profiles. Defaults to 25; session
  // 16's entitlements may raise/lower it at runtime.
  void set_concurrency_cap(int cap) { concurrency_cap_ = cap; }
  int concurrency_cap() const { return concurrency_cap_; }

  // True when the signed-in account's automation entitlement is exhausted given
  // `active_count` already-running automation flows (free = 0, so always at the
  // limit). Gate seam for the automation runtime, which is not built yet: when
  // it lands, its create/run entrypoint should reject with the "automation_limit"
  // string exactly like CreateProfile() rejects with "profile_limit", and the UI
  // should surface an upgrade affordance rather than silently no-op.
  bool AtAutomationLimit(int active_count) const;

  // BrowserCollectionObserver:
  void OnBrowserCreated(BrowserWindowInterface* browser) override;
  void OnBrowserClosed(BrowserWindowInterface* browser) override;

 private:
  friend class base::NoDestructor<MontiManager>;

  MontiManager();
  ~MontiManager() override;

  // Reply trampolines, bound with a weak ptr and forwarding the caller's
  // callback once ProfileManager finishes the async create/load.
  void OnProfileCreated(const std::string& name,
                        const std::string& proxy_id,
                        ResultCallback callback,
                        Profile* profile);
  void OnProfileCreatedFull(ProfileDraft draft,
                            ResultCallback callback,
                            Profile* profile);
  void OnProfileLoadedForDraft(const std::string& id,
                               DraftCallback callback,
                               Profile* profile);
  void OnCredentialsLoadedForDraft(ProfileDraft draft,
                                   DraftCallback callback,
                                   std::vector<ProfileCredential> credentials);
  void OnProfileLoadedForFullUpdate(const std::string& id,
                                    ProfileDraft draft,
                                    ResultCallback callback,
                                    Profile* profile);
  void OnProfileLoadedForLaunch(const std::string& profile_id,
                                LaunchCallback callback,
                                Profile* profile);
  void OnLaunchProxyConnected(const std::string& profile_id,
                              LaunchCallback callback,
                              Profile* profile,
                              bool success,
                              const std::string& error);
  void OpenLaunchedProfileWindow(const std::string& profile_id,
                                 LaunchCallback callback,
                                 Profile* profile);

  // Bidirectional ProfileStore/ProxyStore sync for a proxy assignment; applies
  // (or clears) the proxy on `loaded_profile` when non-null.
  void SetProfileProxy(const std::string& profile_id,
                       const std::string& proxy_id,
                       Profile* loaded_profile);

  // Writes `fp` into profile `profile_id`'s ProfileStore record and, if that
  // profile is currently loaded, mirrors it into its MontiProfileService prefs
  // so the renderer/launch enforcement (session 17) reads it back.
  void PersistFingerprint(const std::string& profile_id, const Fingerprint& fp);

  // Carries the running tally of a LaunchProfiles() batch across the async
  // per-profile launches.
  struct BulkLaunchState {
    BulkLaunchState();
    ~BulkLaunchState();
    std::vector<std::string> ids;
    size_t index = 0;
    uint32_t launched = 0;
    uint32_t skipped = 0;
    BulkLaunchCallback callback;
  };
  void LaunchNextInBatch(std::unique_ptr<BulkLaunchState> state);
  void OnBatchLaunchResult(std::unique_ptr<BulkLaunchState> state,
                           LaunchOutcome outcome,
                           const std::string& message);

  void NotifyProfilesChanged();

  int concurrency_cap_ = 25;

  base::ObserverList<Observer> observers_;

  // Open top-level window count keyed by Profile path; an entry exists only
  // while the profile has >=1 window, so size() == running profile count.
  std::map<base::FilePath, int> window_counts_;

  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      browser_collection_observation_{this};

  base::WeakPtrFactory<MontiManager> weak_factory_{this};
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_MANAGER_H_
