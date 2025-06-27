#ifndef BRAVE_BROWSER_LOGGER_CONFIG_H_
#define BRAVE_BROWSER_LOGGER_CONFIG_H_

#include <string>
#include <vector>
#include <string_view>

#include "base/no_destructor.h"
#include "url/gurl.h"

namespace brave {

// Lightweight singleton that reads environment variables once at startup and
// exposes configuration knobs for UserActionLogger and friends.
class LoggerConfig {
 public:
  static const LoggerConfig& Get();

  int rotation_mb() const { return rotation_mb_; }
  int shot_interval_sec() const { return shot_interval_sec_; }
  const std::vector<std::string>& screenshot_whitelist() const {
    return screenshot_whitelist_;
  }
  // New: domains for which screenshots are explicitly disallowed. Takes
  // precedence over the whitelist.
  const std::vector<std::string>& screenshot_blacklist() const {
    return screenshot_blacklist_;
  }

  // Returns true if the given field/input type should be redacted.
  bool ShouldRedact(std::string_view field_type) const;

  // Returns true if screenshots are allowed for the given URL based on the
  // blacklist → whitelist evaluation order:
  //   1. If the host matches the blacklist, deny.
  //   2. If the whitelist is empty, allow.
  //   3. If the host matches the whitelist, allow; otherwise deny.
  bool IsHostAllowedForScreenshot(const GURL& url) const;

 private:
  friend class base::NoDestructor<LoggerConfig>;
  LoggerConfig();
  ~LoggerConfig();

  int rotation_mb_ = 50;            // default 50 MB rotation
  int shot_interval_sec_ = 30;      // default 30-second screenshots
  std::vector<std::string> screenshot_whitelist_;
  std::vector<std::string> screenshot_blacklist_;

  std::vector<std::string> redact_types_;
};

}  // namespace brave

#endif  // BRAVE_BROWSER_LOGGER_CONFIG_H_ 