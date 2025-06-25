#include "brave/browser/user_action_logger.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#if defined(USE_AURA)
#include "ui/aura/env.h"
#endif
#include <inttypes.h>
#include "content/public/browser/navigation_handle.h"
#include "base/containers/span.h"
#include "base/path_service.h"
#include "base/files/file_util.h"
#include "base/task/thread_pool.h"

namespace brave {

namespace {
constexpr char kLogFileName[] = "user_actions.log";

std::string TimestampNow() {
  base::Time now = base::Time::Now();
  return base::StringPrintf("[%" PRId64 "] ", now.ToDeltaSinceWindowsEpoch().InMilliseconds());
}
}  // namespace

// ------------------------ UserActionLogger -----------------------------

UserActionLogger::UserActionLogger() {
  base::FilePath exe = base::CommandLine::ForCurrentProcess()->GetProgram();
  base::FilePath root = exe.DirName().DirName().DirName().DirName();
  log_path_ = root.AppendASCII(kLogFileName);
  base::CreateDirectory(root);

  task_runner_ = base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::BEST_EFFORT, base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
#if defined(USE_AURA)
  aura::Env::GetInstance()->AddPreTargetHandler(this);
#endif
}

UserActionLogger::~UserActionLogger() {
#if defined(USE_AURA)
  aura::Env::GetInstance()->RemovePreTargetHandler(this);
#endif
}

UserActionLogger* UserActionLogger::GetInstance() {
  static base::NoDestructor<UserActionLogger> instance;
  return instance.get();
}

void UserActionLogger::Write(const std::string& message) {
  task_runner_->PostTask(FROM_HERE, base::BindOnce(
      [](const base::FilePath& path, std::string msg){
          base::AppendToFile(path, std::string_view(msg));
      }, log_path_, std::string(message)));
}

void UserActionLogger::OnKeyEvent(ui::KeyEvent* event) {
  if (event->type() != ui::EventType::kKeyPressed &&
      event->type() != ui::EventType::kKeyReleased)
    return;

  const char* action = event->type() == ui::EventType::kKeyPressed ? "KEY_DOWN" : "KEY_UP";
  std::string key_text = ui::KeycodeConverter::DomCodeToCodeString(event->code());
  std::string log = TimestampNow() + action + ": " + key_text + "\n";
  Write(log);
}

void UserActionLogger::OnMouseEvent(ui::MouseEvent* event) {
  if (event->type() != ui::EventType::kMousePressed)
    return;
  int x = event->x();
  int y = event->y();
  std::string log = TimestampNow() +
                    base::StringPrintf("CLICK: x=%d y=%d\n", x, y);
  Write(log);
}

// ------------------- UserActionLoggerTabHelper -------------------------

UserActionLoggerTabHelper::UserActionLoggerTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<UserActionLoggerTabHelper>(*web_contents) {
  // Ensure singleton is initialized so that navigation events have somewhere to
  // write.
  UserActionLogger::GetInstance();
}

UserActionLoggerTabHelper::~UserActionLoggerTabHelper() = default;

void UserActionLoggerTabHelper::DidFinishNavigation(
    content::NavigationHandle* handle) {
  if (!handle->HasCommitted() || !handle->IsInPrimaryMainFrame())
    return;
  std::string log = TimestampNow() + "NAVIGATION: " + handle->GetURL().spec() + "\n";
  UserActionLogger::GetInstance()->Write(log);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(UserActionLoggerTabHelper);

}  // namespace brave 