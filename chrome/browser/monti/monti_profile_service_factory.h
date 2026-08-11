// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MONTI_MONTI_PROFILE_SERVICE_FACTORY_H_
#define CHROME_BROWSER_MONTI_MONTI_PROFILE_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class KeyedService;
class Profile;

namespace content {
class BrowserContext;
}  // namespace content

namespace monti {

class MontiProfileService;

// Creates and owns one MontiProfileService per profile.
class MontiProfileServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static MontiProfileService* GetForProfile(Profile* profile);
  static MontiProfileServiceFactory* GetInstance();

  MontiProfileServiceFactory(const MontiProfileServiceFactory&) = delete;
  MontiProfileServiceFactory& operator=(const MontiProfileServiceFactory&) =
      delete;

 private:
  friend base::NoDestructor<MontiProfileServiceFactory>;

  MontiProfileServiceFactory();
  ~MontiProfileServiceFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
  // Created eagerly with the profile so the proxy connection fail-safe and
  // verified auto-reconnect run at startup, independent of the side panel.
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace monti

#endif  // CHROME_BROWSER_MONTI_MONTI_PROFILE_SERVICE_FACTORY_H_
