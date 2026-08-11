// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_profile_service_factory.h"

#include <memory>

#include "chrome/browser/monti/monti_profile_service.h"
#include "chrome/browser/profiles/profile.h"

namespace monti {

// static
MontiProfileService* MontiProfileServiceFactory::GetForProfile(
    Profile* profile) {
  return static_cast<MontiProfileService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
MontiProfileServiceFactory* MontiProfileServiceFactory::GetInstance() {
  static base::NoDestructor<MontiProfileServiceFactory> instance;
  return instance.get();
}

MontiProfileServiceFactory::MontiProfileServiceFactory()
    : ProfileKeyedServiceFactory(
          "MontiProfileService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .WithGuest(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {}

MontiProfileServiceFactory::~MontiProfileServiceFactory() = default;

std::unique_ptr<KeyedService>
MontiProfileServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<MontiProfileService>(
      Profile::FromBrowserContext(context));
}

void MontiProfileServiceFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  MontiProfileService::RegisterProfilePrefs(registry);
}

bool MontiProfileServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

}  // namespace monti
