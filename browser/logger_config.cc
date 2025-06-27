#include "brave/browser/logger_config.h"

#include "base/environment.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/url_canon.h"
#include "url/url_util.h"
#include "base/strings/string_util.h"

namespace brave {

namespace {
constexpr char kEnvRotateMb[] = "BRAVE_LOGGER_ROTATE_MB";
constexpr char kEnvShotInterval[] = "BRAVE_LOGGER_SHOT_INTERVAL";  // seconds
constexpr char kEnvWhitelist[] = "BRAVE_LOGGER_SHOT_WHITELIST";    // comma list
constexpr char kEnvBlacklist[] = "BRAVE_LOGGER_SHOT_BLACKLIST";    // comma list
constexpr char kEnvRedactTypes[] = "BRAVE_LOGGER_REDACT_TYPES";
}

LoggerConfig::LoggerConfig() {
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  if (auto opt = env->GetVar(kEnvRotateMb); opt && !opt->empty()) {
    int mb;
    if (base::StringToInt(*opt, &mb) && mb > 0)
      rotation_mb_ = mb;
  }

  if (auto opt = env->GetVar(kEnvShotInterval); opt && !opt->empty()) {
    int secs;
    if (base::StringToInt(*opt, &secs) && secs > 0)
      shot_interval_sec_ = secs;
  }

  if (auto opt = env->GetVar(kEnvWhitelist); opt && !opt->empty()) {
    screenshot_whitelist_ = base::SplitString(*opt, ",",
                                              base::TRIM_WHITESPACE,
                                              base::SPLIT_WANT_NONEMPTY);
  }

  if (auto opt = env->GetVar(kEnvBlacklist); opt && !opt->empty()) {
    screenshot_blacklist_ = base::SplitString(*opt, ",",
                                              base::TRIM_WHITESPACE,
                                              base::SPLIT_WANT_NONEMPTY);
  }

  if (auto opt = env->GetVar(kEnvRedactTypes); opt && !opt->empty()) {
    redact_types_ = base::SplitString(*opt, ",",
                                      base::TRIM_WHITESPACE,
                                      base::SPLIT_WANT_NONEMPTY);
  } else {
    redact_types_ = {"password", "email", "tel", "search", "number"};
  }
}

LoggerConfig::~LoggerConfig() = default;

bool LoggerConfig::ShouldRedact(std::string_view field_type) const {
  for (const auto& t : redact_types_) {
    if (field_type == t)
      return true;
  }
  return false;
}

bool LoggerConfig::IsHostAllowedForScreenshot(const GURL& url) const {
  if (!url.is_valid())
    return false;

  std::string host_to_check;
  host_to_check = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (host_to_check.empty())
    host_to_check = url.host();

  // 1. Explicit blacklist check.
  for (const std::string& blocked : screenshot_blacklist_) {
    if (base::EqualsCaseInsensitiveASCII(host_to_check, blocked))
      return false;
  }

  // 2. Whitelist handling.
  if (screenshot_whitelist_.empty())
    return true;  // Allow everything not explicitly blocked.

  for (const std::string& allowed : screenshot_whitelist_) {
    if (base::EqualsCaseInsensitiveASCII(host_to_check, allowed))
      return true;
  }
  return false;
}

// static
const LoggerConfig& LoggerConfig::Get() {
  static base::NoDestructor<LoggerConfig> instance;
  return *instance;
}

}  // namespace brave 