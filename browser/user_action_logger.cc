#include "brave/browser/user_action_logger.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/time/time.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "build/build_config.h"
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
#if defined(USE_AURA) && !BUILDFLAG(IS_MAC)
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#endif  // defined(USE_AURA) && !BUILDFLAG(IS_MAC)
#include "base/environment.h"
#if BUILDFLAG(IS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif
#include "base/task/single_thread_task_runner.h"

namespace brave {

namespace {
constexpr char kLogFileName[] = "user_actions.log";

void SaveScreenshotTask(std::vector<uint8_t> png_data,
                        int tab_id,
                        int win_id,
                        int width,
                        int height,
                        base::Time now) {
  // Use the same root as user_actions.log (resolves correctly for .app bundles)
  base::FilePath root = UserActionLogger::GetInstance()->root_dir();

  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);

  base::FilePath shots_dir = root.AppendASCII("shots")
                               .AppendASCII(base::StringPrintf("%04d", exploded.year))
                               .AppendASCII(base::StringPrintf("%02d", exploded.month))
                               .AppendASCII(base::StringPrintf("%02d", exploded.day_of_month));

  base::CreateDirectory(shots_dir);

  uint64_t ts_ms = static_cast<uint64_t>(now.ToDeltaSinceWindowsEpoch().InMilliseconds());
  std::string file_name = base::StringPrintf("%llu-%d-%d.png", static_cast<unsigned long long>(ts_ms), tab_id, win_id);
  base::FilePath png_path = shots_dir.AppendASCII(file_name);

  if (!base::WriteFile(png_path, base::span<const uint8_t>(png_data))) {
    return;  // Give up silently.
  }

  // Compute relative path (forward slashes for portability).
  std::string relative_path = png_path.AsUTF8Unsafe();
  std::string root_str = root.AsUTF8Unsafe();
  if (relative_path.rfind(root_str, 0) == 0) {
    relative_path = relative_path.substr(root_str.length());
    if (!relative_path.empty() && (relative_path[0] == '/' || relative_path[0] == '\\'))
      relative_path.erase(0, 1);
  }

  base::Value::Dict payload;
  payload.Set("type", "shot");
  payload.Set("tab_id", tab_id);
  payload.Set("win_id", win_id);
  payload.Set("w", width);
  payload.Set("h", height);
  payload.Set("file", std::move(relative_path));

  UserActionLogger::GetInstance()->Write(std::move(payload));
}

#if BUILDFLAG(IS_MAC)
namespace {

static CFMachPortRef g_event_tap = nullptr;

CGEventRef MacEventTapCallback(CGEventTapProxy /*proxy*/, CGEventType type,
                               CGEventRef event, void* /*user_info*/) {
  // Re-enable tap if it was disabled by the system.
  if (type == kCGEventTapDisabledByTimeout && g_event_tap) {
    CGEventTapEnable(g_event_tap, true);
    return event;
  }

  if (type != kCGEventKeyDown && type != kCGEventKeyUp &&
      type != kCGEventLeftMouseDown)
    return event;

  brave::UserActionLogger* logger = brave::UserActionLogger::GetInstance();
  base::Value::Dict payload;

  uint64_t flags = CGEventGetFlags(event);
  bool ctrl  = (flags & kCGEventFlagMaskControl) != 0;
  bool shift = (flags & kCGEventFlagMaskShift) != 0;
  bool alt   = (flags & kCGEventFlagMaskAlternate) != 0;

  if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
    payload.Set("type", "key");
    payload.Set("down", type == kCGEventKeyDown);
    // Raw keycode (hardware) – good enough for now.
    int64_t keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    ui::DomCode dom_code = ui::KeycodeConverter::NativeKeycodeToDomCode(static_cast<int>(keycode));
    std::string code_str = ui::KeycodeConverter::DomCodeToCodeString(dom_code);
    payload.Set("code", std::move(code_str));
    payload.Set("ctrl", ctrl);
    payload.Set("shift", shift);
    payload.Set("alt", alt);
  } else {
    payload.Set("type", "click");
    CGPoint loc = CGEventGetLocation(event);
    payload.Set("x", static_cast<int>(loc.x));
    payload.Set("y", static_cast<int>(loc.y));
    payload.Set("button", 1);
  }

  logger->Write(std::move(payload));
  return event;
}
}  // namespace
#endif  // BUILDFLAG(IS_MAC)

}  // namespace (anonymous)

// ------------------------ UserActionLogger -----------------------------

UserActionLogger::UserActionLogger() {
  base::FilePath exe = base::CommandLine::ForCurrentProcess()->GetProgram();

  // 1. Respect BRAVE_LOGGER_PATH env variable if set (absolute path to the
  //    desired log file, not directory). This gives power users full control.
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  if (auto opt_path = env->GetVar("BRAVE_LOGGER_PATH"); opt_path && !opt_path->empty()) {
    log_path_ = base::FilePath::FromUTF8Unsafe(*opt_path);
  } else {
    // 2. Otherwise, walk up the ancestor chain until we find the workspace
    //    folder named "meteor". This lets packaged .app bundles on macOS and
    //    deep out/Component_* trees still resolve to the developer root.
    base::FilePath cursor = exe.DirName();
    const int kMaxAscend = 20;  // safety guard
    int steps = 0;
    while (!cursor.empty() && steps++ < kMaxAscend) {
      if (cursor.BaseName().MaybeAsASCII() == "meteor")
        break;
      cursor = cursor.DirName();
    }

    base::FilePath root;
    if (!cursor.empty() && cursor.BaseName().MaybeAsASCII() == "meteor") {
      root = cursor;
    } else {
      // Fallback to historical behaviour: 4 dirs above the binary.
      root = exe.DirName().DirName().DirName().DirName();
    }

    log_path_ = root.AppendASCII(kLogFileName);
  }

  // Record the resolved root directory for reuse (e.g., screenshots).
  root_dir_ = log_path_.DirName();

  base::CreateDirectory(log_path_.DirName());

#if BUILDFLAG(IS_MAC)
  // macOS doesn't deliver Aura pre-target events; install a CGEventTap to
  // capture keys and primary-button clicks at the session level.
  {
    CGEventMask mask = (1ull << kCGEventKeyDown) | (1ull << kCGEventKeyUp) |
                       (1ull << kCGEventLeftMouseDown);
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                         kCGEventTapOptionDefault, mask,
                                         MacEventTapCallback, nullptr);
    if (tap) {
      g_event_tap = tap;
      CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
      CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
      CGEventTapEnable(tap, true);
      CFRelease(src);
    }
  }
#endif

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

    // Optionally prune orphaned screenshots older than retention window.
    base::FilePath shots_root = log_path.DirName().AppendASCII("shots");
    base::FileEnumerator shot_iter(shots_root, true /* recursive */, base::FileEnumerator::FILES, FILE_PATH_LITERAL("*.png"));
    for (base::FilePath p = shot_iter.Next(); !p.empty(); p = shot_iter.Next()) {
      base::File::Info info;
      if (base::GetFileInfo(p, &info)) {
        if (now - info.last_modified > base::Days(kRetentionDays)) {
          base::DeleteFile(p);
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
#if defined(USE_AURA) && !BUILDFLAG(IS_MAC)
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
    payload.Set("tab_id", current_tab_id_);
  if (current_window_id_ != -1)
    payload.Set("win_id", current_window_id_);
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
    payload.Set("tab_id", current_tab_id_);
  if (current_window_id_ != -1)
    payload.Set("win_id", current_window_id_);
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
    payload.Set("tab_id", tab_id);
  if (win_id_obj.is_valid())
    payload.Set("win_id", win_id);
  // Title (UTF8)
  std::string title_utf8 = base::UTF16ToUTF8(handle->GetWebContents()->GetTitle());
  if (!title_utf8.empty())
    payload.Set("title", std::move(title_utf8));

  // Viewport size in device-independent pixels.
  if (auto* rwhv = handle->GetWebContents()->GetRenderWidgetHostView()) {
    gfx::Rect bounds = rwhv->GetViewBounds();
    base::Value::Dict vp;
    vp.Set("w", bounds.width());
    vp.Set("h", bounds.height());
    payload.Set("vp", std::move(vp));
  }
  UserActionLogger::GetInstance()->SetCurrentContext(tab_id, win_id);
  UserActionLogger::GetInstance()->Write(std::move(payload));

  // Schedule a screenshot 1 s after navigation commit so the first paint has
  // time to finish; this prevents empty bitmaps on macOS.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&UserActionLoggerTabHelper::CaptureScreenshot,
                     weak_factory_.GetWeakPtr()),
      base::Seconds(1));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(UserActionLoggerTabHelper);

// ------------ Screenshot helpers ------------------

void UserActionLoggerTabHelper::CaptureScreenshot() {
  if (!web_contents())
    return;
  const LoggerConfig& cfg = LoggerConfig::Get();
  if (!cfg.IsHostAllowedForScreenshot(web_contents()->GetLastCommittedURL()))
    return;  // Skip capture for disallowed hosts.
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
  // Some platforms may deliver a bitmap without pixel data immediately; fall
  // back to writing a metadata-only shot event so the timeline still shows the
  // capture point. Only bail out entirely if there is no pixel data *and* no
  // dimensions.

  if (!bitmap.readyToDraw() && (bitmap.width() == 0 || bitmap.height() == 0)) {
    // Log stub event so downstream agent knows a frame boundary even though we
    // lacked pixels.
    base::Value::Dict payload;
    payload.Set("type", "shot");
    payload.Set("tab_id", tab_id);
    payload.Set("win_id", win_id);
    UserActionLogger::GetInstance()->Write(std::move(payload));
    return;
  }

  std::optional<std::vector<uint8_t>> png_bytes = gfx::PNGCodec::FastEncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png_bytes) {
    base::Value::Dict payload;
    payload.Set("type", "shot");
    payload.Set("tab_id", tab_id);
    payload.Set("win_id", win_id);
    payload.Set("w", bitmap.width());
    payload.Set("h", bitmap.height());
    UserActionLogger::GetInstance()->Write(std::move(payload));
    return;
  }

  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
      base::BindOnce(&SaveScreenshotTask, std::move(*png_bytes), tab_id, win_id,
                     bitmap.width(), bitmap.height(), base::Time::Now()));
}

}  // namespace brave 