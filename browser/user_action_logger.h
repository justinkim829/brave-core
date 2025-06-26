#ifndef BRAVE_BROWSER_USER_ACTION_LOGGER_H_
#define BRAVE_BROWSER_USER_ACTION_LOGGER_H_

#include "base/no_destructor.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/events/event_handler.h"
#include "ui/events/types/event_type.h"
#include <string>
#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "base/timer/timer.h"
#include "third_party/skia/include/core/SkBitmap.h"

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
  void Write(base::Value::Dict payload);

  // Internal task run on the background sequence that performs the physical
  // append and handles rotation/compression/retention. Exposed for BindOnce.
  static void WriteTask(const base::FilePath& log_path, std::string data);

  void SetCurrentContext(int tab_id, int win_id);

  // Called by tab helpers/tests to indicate the currently focused input type
  // (e.g., "password", "email", or empty when not in editable field).
  void SetCurrentInputType(const std::string& type) { current_input_type_ = type; }

 private:
  friend class base::NoDestructor<UserActionLogger>;
  UserActionLogger();
  ~UserActionLogger() override;

  base::FilePath log_path_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  int current_tab_id_ = -1;
  int current_window_id_ = -1;
  std::string current_input_type_;
};

// Per-tab helper used to observe navigation events and funnel them into the
// UserActionLogger.
class UserActionLoggerTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<UserActionLoggerTabHelper> {
 public:
  ~UserActionLoggerTabHelper() override;

  void CaptureScreenshot();
  void OnScreenshotDone(int tab_id,
                        int win_id,
                        const SkBitmap& bitmap);

  // Temporary: called by tests/IPC plumbing to update the focused input type.
  void SetCurrentInputType(const std::string& type) {
    brave::UserActionLogger::GetInstance()->SetCurrentInputType(type);
  }

  // content::WebContentsObserver overrides
  void DidFinishNavigation(content::NavigationHandle* handle) override;

 private:
  explicit UserActionLoggerTabHelper(content::WebContents* web_contents);
  friend class content::WebContentsUserData<UserActionLoggerTabHelper>;

  WEB_CONTENTS_USER_DATA_KEY_DECL();

  base::RepeatingTimer periodic_timer_;
  base::WeakPtrFactory<UserActionLoggerTabHelper> weak_factory_{this};
};

}  // namespace brave

#endif  // BRAVE_BROWSER_USER_ACTION_LOGGER_H_ 