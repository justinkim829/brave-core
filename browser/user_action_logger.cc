#include "brave/browser/user_action_logger.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/time/time.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#if defined(USE_AURA)
#include "ui/aura/env.h"
#endif
#include "content/public/browser/navigation_handle.h"
#include "base/containers/span.h"
#include "base/path_service.h"
#include "base/files/file_util.h"
#include "base/files/file_enumerator.h"
#include "base/task/thread_pool.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include <string_view>
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/session_id.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/gfx/geometry/rect.h"
#include "content/public/browser/render_widget_host_view.h"
#include "ui/gfx/codec/png_codec.h"
#include "base/base64.h"
#include "base/timer/timer.h"
#include <vector>
#include "third_party/zlib/google/compression_utils.h"
#include "base/strings/stringprintf.h"
#include "brave/browser/logger_config.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"

namespace brave {

namespace {
constexpr char kLogFileName[] = "user_actions.log";
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

void UserActionLogger::Write(base::Value::Dict payload) {
  payload.Set("ts", static_cast<double>(
                        base::Time::Now().ToDeltaSinceWindowsEpoch().InMilliseconds()));
  std::string json;
  base::JSONWriter::Write(payload, &json);
  json.append("\n");

  task_runner_->PostTask(FROM_HERE, base::BindOnce(&UserActionLogger::WriteTask,
                                                 log_path_, std::move(json)));
}

// static
void UserActionLogger::WriteTask(const base::FilePath& log_path, std::string data) {
  const LoggerConfig& cfg = LoggerConfig::Get();
  constexpr int kRetentionDays = 30;  // Still hard-coded for now.
  const int64_t kMaxBytes = static_cast<int64_t>(cfg.rotation_mb()) * 1024 * 1024;

  // Ensure parent directory exists (in case first call races constructor).
  base::CreateDirectory(log_path.DirName());

  // Determine if rotation is needed.
  int64_t current_size = 0;
  if (auto opt_size = base::GetFileSize(log_path); opt_size)
    current_size = *opt_size;
  if (current_size > 0 && current_size + static_cast<int64_t>(data.size()) > kMaxBytes) {
    // Build rotated filename: user_actions-YYYYMMDD-HHMMSS.log
    base::Time now = base::Time::Now();
    base::Time::Exploded exploded;
    now.LocalExplode(&exploded);
    std::string timestamp = base::StringPrintf("%04d%02d%02d-%02d%02d%02d",
                                              exploded.year, exploded.month,
                                              exploded.day_of_month, exploded.hour,
                                              exploded.minute, exploded.second);
    base::FilePath rotated_plain =
        log_path.DirName().AppendASCII("user_actions-" + timestamp + ".log");

    // If by chance the name exists, append a counter.
    int counter = 1;
    while (base::PathExists(rotated_plain)) {
      rotated_plain = log_path.DirName().AppendASCII(
          base::StringPrintf("user_actions-%s-%02d.log", timestamp.c_str(), counter++));
    }

    // Move current log to rotated filename.
    base::Move(log_path, rotated_plain);

    // Read rotated file content.
    std::string uncompressed;
    base::ReadFileToString(rotated_plain, &uncompressed);

    // Compress using gzip.
    std::string compressed;
    auto byte_span = base::as_byte_span(uncompressed);
    if (compression::GzipCompress(byte_span, &compressed)) {
      base::FilePath gz_path = rotated_plain.AddExtensionASCII("gz");
      base::WriteFile(gz_path, compressed);
      base::DeleteFile(rotated_plain);
    }

    // Cleanup compressed logs older than retention window.
    base::FileEnumerator iter(log_path.DirName(), false, base::FileEnumerator::FILES,
                              FILE_PATH_LITERAL("user_actions-*.log.gz"));
    for (base::FilePath path = iter.Next(); !path.empty(); path = iter.Next()) {
      base::File::Info info;
      if (base::GetFileInfo(path, &info)) {
        if (now - info.last_modified > base::Days(kRetentionDays)) {
          base::DeleteFile(path);
        }
      }
    }
  }

  // Finally, append the new data.
  base::AppendToFile(log_path, std::string_view(data));
}

void UserActionLogger::OnKeyEvent(ui::KeyEvent* event) {
  if (event->type() != ui::EventType::kKeyPressed &&
      event->type() != ui::EventType::kKeyReleased)
    return;

  base::Value::Dict payload;
  payload.Set("type", "key");
  payload.Set("down", event->type() == ui::EventType::kKeyPressed);

  // Determine input field type via the platform InputMethod associated with
  // the focused window. This lets us detect <input type="password"> etc.
  std::string field_type;
#if defined(USE_AURA)
  aura::Env* env = aura::Env::GetInstance();
  if (env) {
    for (auto host_raw : env->window_tree_hosts()) {
      aura::WindowTreeHost* host_ptr = host_raw.get();
      if (!host_ptr)
        continue;
      if (auto* im = host_ptr->GetInputMethod()) {
        ui::TextInputType t = im->GetTextInputType();
        switch (t) {
          case ui::TEXT_INPUT_TYPE_PASSWORD:
            field_type = "password";
            break;
          case ui::TEXT_INPUT_TYPE_EMAIL:
            field_type = "email";
            break;
          case ui::TEXT_INPUT_TYPE_TELEPHONE:
            field_type = "tel";
            break;
          case ui::TEXT_INPUT_TYPE_NUMBER:
            field_type = "number";
            break;
          case ui::TEXT_INPUT_TYPE_SEARCH:
            field_type = "search";
            break;
          default:
            field_type.clear();
        }
        if (!field_type.empty())
          break;  // Found meaningful type.
      }
    }
  }
#endif

  const LoggerConfig& cfg = LoggerConfig::Get();
  if (!field_type.empty())
    payload.Set("field", field_type);

  bool redact = cfg.ShouldRedact(field_type);
  if (redact) {
    payload.Set("action", "redacted");
  } else {
    payload.Set("code", ui::KeycodeConverter::DomCodeToCodeString(event->code()));
  }

  payload.Set("ctrl", (event->flags() & ui::EF_CONTROL_DOWN) != 0);
  payload.Set("shift", (event->flags() & ui::EF_SHIFT_DOWN) != 0);
  payload.Set("alt", (event->flags() & ui::EF_ALT_DOWN) != 0);
  if (current_tab_id_ != -1)
    payload.Set("tabId", current_tab_id_);
  if (current_window_id_ != -1)
    payload.Set("winId", current_window_id_);
  Write(std::move(payload));
}

void UserActionLogger::OnMouseEvent(ui::MouseEvent* event) {
  if (event->type() != ui::EventType::kMousePressed)
    return;

  base::Value::Dict payload;
  payload.Set("type", "click");
  payload.Set("x", event->x());
  payload.Set("y", event->y());
  payload.Set("button", static_cast<int>(event->changed_button_flags()));
  if (current_tab_id_ != -1)
    payload.Set("tabId", current_tab_id_);
  if (current_window_id_ != -1)
    payload.Set("winId", current_window_id_);
  Write(std::move(payload));
}

void UserActionLogger::SetCurrentContext(int tab_id, int win_id) {
  current_tab_id_ = tab_id;
  current_window_id_ = win_id;
}

// ------------------- UserActionLoggerTabHelper -------------------------

UserActionLoggerTabHelper::UserActionLoggerTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<UserActionLoggerTabHelper>(*web_contents) {
  // Ensure singleton is initialized so that navigation events have somewhere to
  // write.
  UserActionLogger::GetInstance();

  // Start periodic 30-second screenshots.
  int interval = LoggerConfig::Get().shot_interval_sec();
  if (interval > 0) {
    periodic_timer_.Start(FROM_HERE, base::Seconds(interval),
                          base::BindRepeating(&UserActionLoggerTabHelper::CaptureScreenshot,
                                              weak_factory_.GetWeakPtr()));
  }
}

UserActionLoggerTabHelper::~UserActionLoggerTabHelper() = default;

void UserActionLoggerTabHelper::DidFinishNavigation(
    content::NavigationHandle* handle) {
  if (!handle->HasCommitted() || !handle->IsInPrimaryMainFrame())
    return;
  base::Value::Dict payload;
  payload.Set("type", "nav");
  payload.Set("url", handle->GetURL().spec());
  auto tab_id_obj = sessions::SessionTabHelper::IdForTab(handle->GetWebContents());
  auto win_id_obj = sessions::SessionTabHelper::IdForWindowContainingTab(handle->GetWebContents());
  int tab_id = tab_id_obj.id();
  int win_id = win_id_obj.id();
  if (tab_id_obj.is_valid())
    payload.Set("tabId", tab_id);
  if (win_id_obj.is_valid())
    payload.Set("winId", win_id);
  // Title (UTF8)
  std::string title_utf8 = base::UTF16ToUTF8(handle->GetWebContents()->GetTitle());
  if (!title_utf8.empty())
    payload.Set("title", std::move(title_utf8));

  // Viewport size in device-independent pixels.
  if (auto* rwhv = handle->GetWebContents()->GetRenderWidgetHostView()) {
    gfx::Rect bounds = rwhv->GetViewBounds();
    payload.Set("vpW", bounds.width());
    payload.Set("vpH", bounds.height());
  }
  UserActionLogger::GetInstance()->SetCurrentContext(tab_id, win_id);
  UserActionLogger::GetInstance()->Write(std::move(payload));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(UserActionLoggerTabHelper);

// ------------ Screenshot helpers ------------------

void UserActionLoggerTabHelper::CaptureScreenshot() {
  if (!web_contents())
    return;
  auto* rwhv = web_contents()->GetRenderWidgetHostView();
  if (!rwhv)
    return;

  auto tab_id_obj = sessions::SessionTabHelper::IdForTab(web_contents());
  auto win_id_obj = sessions::SessionTabHelper::IdForWindowContainingTab(web_contents());
  int tab_id = tab_id_obj.id();
  int win_id = win_id_obj.id();

  gfx::Size dst_size(320, 200);
  rwhv->CopyFromSurface(gfx::Rect(), dst_size,
                        base::BindOnce(&UserActionLoggerTabHelper::OnScreenshotDone, weak_factory_.GetWeakPtr(), tab_id, win_id));
}

void UserActionLoggerTabHelper::OnScreenshotDone(int tab_id,
                                                int win_id,
                                                const SkBitmap& bitmap) {
  if (!bitmap.readyToDraw())
    return;

  std::optional<std::vector<uint8_t>> png_bytes = gfx::PNGCodec::FastEncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png_bytes)
    return;

  std::string b64 = base::Base64Encode(base::span<const uint8_t>(*png_bytes));

  base::Value::Dict payload;
  payload.Set("type", "shot");
  payload.Set("tabId", tab_id);
  payload.Set("winId", win_id);
  payload.Set("w", bitmap.width());
  payload.Set("h", bitmap.height());
  payload.Set("png", std::move(b64));

  UserActionLogger::GetInstance()->Write(std::move(payload));
}

}  // namespace brave 