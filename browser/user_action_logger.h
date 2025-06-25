#ifndef BRAVE_BROWSER_USER_ACTION_LOGGER_H_
#define BRAVE_BROWSER_USER_ACTION_LOGGER_H_

#include "base/files/file.h"
#include "base/synchronization/lock.h"
#include "base/no_destructor.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/events/event_handler.h"
#include "ui/events/types/event_type.h"
#include <string>
#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"

namespace ui {
class KeyEvent;
class MouseEvent;
}

namespace brave {

// Singleton that logs user input events (keyboard, mouse) to a file located at
//   ../../user_actions.log  (relative to the working directory of the Brave
//   process, which is <project_root>/src/brave when launched via npm scripts).
class UserActionLogger : public ui::EventHandler {
 public:
  static UserActionLogger* GetInstance();

  // ui::EventHandler overrides
  void OnKeyEvent(ui::KeyEvent* event) override;
  void OnMouseEvent(ui::MouseEvent* event) override;

  // Write raw string to the log file (thread‐safe).
  void Write(const std::string& message);

 private:
  friend class base::NoDestructor<UserActionLogger>;
  UserActionLogger();
  ~UserActionLogger() override;

  base::FilePath log_path_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
};

// Per-tab helper used to observe navigation events and funnel them into the
// UserActionLogger.
class UserActionLoggerTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<UserActionLoggerTabHelper> {
 public:
  ~UserActionLoggerTabHelper() override;

  // content::WebContentsObserver overrides
  void DidFinishNavigation(content::NavigationHandle* handle) override;

 private:
  explicit UserActionLoggerTabHelper(content::WebContents* web_contents);
  friend class content::WebContentsUserData<UserActionLoggerTabHelper>;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace brave

#endif  // BRAVE_BROWSER_USER_ACTION_LOGGER_H_ 