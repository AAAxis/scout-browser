// Copyright 2026 The Monti Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/monti/monti_profile_launcher.h"

#import <CoreServices/CoreServices.h>

#include <string>
#include <utility>
#include <vector>

#include "base/apple/bridging.h"
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/monti/monti_profile_entry.h"
#include "chrome/common/chrome_paths.h"

namespace monti {
namespace {

std::string XmlEscape(std::string text) {
  base::ReplaceSubstringsAfterOffset(&text, 0, "&", "&amp;");
  base::ReplaceSubstringsAfterOffset(&text, 0, "<", "&lt;");
  base::ReplaceSubstringsAfterOffset(&text, 0, ">", "&gt;");
  base::ReplaceSubstringsAfterOffset(&text, 0, "\"", "&quot;");
  return text;
}

std::string ShellQuote(const std::string& text) {
  std::string out = "'";
  for (char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

std::string BundleSafeId(std::string text) {
  for (char& c : text) {
    if (!base::IsAsciiAlpha(c) && !base::IsAsciiDigit(c)) {
      c = '-';
    }
  }
  return text;
}

std::vector<std::string> ParseLaunchSwitches(const std::string& switches) {
  std::vector<std::string> out;
  for (std::string line : base::SplitString(
           switches, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (!base::StartsWith(line, "--")) {
      line = base::StrCat({"--", line});
    }
    out.push_back(std::move(line));
  }
  return out;
}

base::FilePath LaunchersRoot() {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return base::FilePath();
  }
  return home.AppendASCII("Applications").AppendASCII("Monti Profiles");
}

base::FilePath LauncherPathForProfileDir(const std::string& profile_dir) {
  return LaunchersRoot().Append(base::FilePath::FromUTF8Unsafe(profile_dir))
      .AddExtensionASCII("app");
}

std::string BuildInfoPlist(const MontiProfileEntry& entry) {
  const std::string display_name = entry.name.empty() ? entry.profile_dir
                                                     : entry.name;
  const std::string bundle_id =
      base::StrCat({base::apple::BaseBundleID(), ".monti.profile.",
                    BundleSafeId(entry.profile_dir)});
  return base::StrCat({
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\"><dict>\n"
      "<key>CFBundleName</key><string>",
      XmlEscape(display_name),
      "</string>\n"
      "<key>CFBundleDisplayName</key><string>",
      XmlEscape(display_name),
      "</string>\n"
      "<key>CFBundleIdentifier</key><string>",
      XmlEscape(bundle_id),
      "</string>\n"
      "<key>CFBundleExecutable</key><string>MontiProfileLauncher</string>\n"
      "<key>CFBundleIconFile</key><string>app</string>\n"
      "<key>CFBundlePackageType</key><string>APPL</string>\n"
      "<key>CFBundleShortVersionString</key><string>1.0</string>\n"
      "<key>CFBundleVersion</key><string>1</string>\n"
      "<key>LSMinimumSystemVersion</key><string>13.0</string>\n"
      "</dict></plist>\n"});
}

std::string BuildLauncherScript(const MontiProfileEntry& entry,
                                std::vector<base::FilePath> extensions,
                                const std::string& launch_switches) {
  base::FilePath user_data_dir;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);

  std::string script =
      "#!/bin/zsh\n"
      "set -e\n"
      "MONTI_APP=\"/Applications/Monti Browser.app\"\n"
      "MONTI_BIN=\"$MONTI_APP/Contents/MacOS/Monti Browser\"\n"
      "if [[ ! -x \"$MONTI_BIN\" ]]; then\n"
      "  MONTI_BIN=\"$MONTI_APP/Contents/MacOS/Monti\"\n"
      "fi\n"
      "if [[ ! -x \"$MONTI_BIN\" ]]; then\n"
      "  MONTI_APP=\"/Applications/Monti.app\"\n"
      "  MONTI_BIN=\"$MONTI_APP/Contents/MacOS/Monti\"\n"
      "fi\n"
      "if [[ ! -x \"$MONTI_BIN\" ]]; then\n"
      "  MONTI_BIN=\"";
  script += base::SysNSStringToUTF8(base::apple::MainBundle().executablePath);
  script += "\"\nfi\n";
  script += "\"$MONTI_BIN\" --profile-directory=";
  script += ShellQuote(entry.profile_dir);
  script += " --monti-profile-launch";
  if (!user_data_dir.empty()) {
    script += " --user-data-dir=" + ShellQuote(user_data_dir.AsUTF8Unsafe());
  }
  if (!extensions.empty()) {
    std::vector<std::string> paths;
    for (const base::FilePath& extension : extensions) {
      if (!extension.empty()) {
        paths.push_back(extension.AsUTF8Unsafe());
      }
    }
    if (!paths.empty()) {
      script += " --load-extension=" + ShellQuote(base::JoinString(paths, ","));
    }
  }
  for (const std::string& launch_switch : ParseLaunchSwitches(launch_switches)) {
    script += " " + ShellQuote(launch_switch);
  }
  script += " if [[ $# -eq 0 ]]; then set -- chrome://monti-newtab; fi\n";
  script += " \"$@\" >/tmp/monti-profile-launcher.log 2>&1 &\n";
  script += "sleep 3\n";
  script += "PROFILE_ARG=\"--profile-directory=";
  script += entry.profile_dir;
  script += "\"\n";
  script +=
      "while pgrep -f \"$MONTI_BIN\" "
      ">/dev/null 2>&1; do\n"
      "  sleep 5\n"
      "done\n";
  return script;
}

void CreateOrUpdateProfileLauncherOnWorker(
    MontiProfileEntry entry,
    std::vector<base::FilePath> extensions,
    std::string launch_switches) {
  const base::FilePath app_path = LauncherPathForProfileDir(entry.profile_dir);
  if (app_path.empty()) {
    return;
  }

  const base::FilePath contents = app_path.AppendASCII("Contents");
  const base::FilePath macos = contents.AppendASCII("MacOS");
  const base::FilePath resources = contents.AppendASCII("Resources");
  if (!base::CreateDirectory(macos) || !base::CreateDirectory(resources)) {
    return;
  }

  base::WriteFile(contents.AppendASCII("Info.plist"), BuildInfoPlist(entry));
  base::WriteFile(contents.AppendASCII("PkgInfo"), "APPL????");
  if (!base::CopyFile(base::FilePath(FILE_PATH_LITERAL(
                          "/Applications/Monti Browser.app/Contents/Resources/app.icns")),
                      resources.AppendASCII("app.icns"))) {
    base::CopyFile(base::FilePath(FILE_PATH_LITERAL(
                       "/Applications/Monti.app/Contents/Resources/app.icns")),
                   resources.AppendASCII("app.icns"));
  }
  const base::FilePath executable = macos.AppendASCII("MontiProfileLauncher");
  base::WriteFile(executable, BuildLauncherScript(
                                  entry, std::move(extensions),
                                  launch_switches));
  base::SetPosixFilePermissions(executable, 0755);

  if (base::apple::ScopedCFTypeRef<CFURLRef> url =
          base::apple::FilePathToCFURL(app_path)) {
    LSRegisterURL(url.get(), true);
  }
}

void RemoveProfileLauncherOnWorker(std::string profile_dir) {
  const base::FilePath app_path = LauncherPathForProfileDir(profile_dir);
  if (!app_path.empty()) {
    base::DeletePathRecursively(app_path);
  }
}

}  // namespace

void CreateOrUpdateProfileLauncher(const MontiProfileEntry& entry,
                                   std::vector<base::FilePath> extensions,
                                   std::string launch_switches) {
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&CreateOrUpdateProfileLauncherOnWorker, entry,
                     std::move(extensions), std::move(launch_switches)));
}

void RemoveProfileLauncher(const std::string& profile_dir) {
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&RemoveProfileLauncherOnWorker, profile_dir));
}

bool LaunchProfileLauncher(const MontiProfileEntry& entry) {
  const base::FilePath app_path = LauncherPathForProfileDir(entry.profile_dir);
  if (app_path.empty() || !base::PathExists(app_path)) {
    return false;
  }
  base::CommandLine command_line(base::FilePath(FILE_PATH_LITERAL("/usr/bin/open")));
  command_line.AppendArgPath(app_path);
  return base::LaunchProcess(command_line, base::LaunchOptions()).IsValid();
}

}  // namespace monti
