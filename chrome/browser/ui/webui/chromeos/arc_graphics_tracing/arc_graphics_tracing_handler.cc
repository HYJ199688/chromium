// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/chromeos/arc_graphics_tracing/arc_graphics_tracing_handler.h"

#include <map>
#include <string>

#include "ash/public/cpp/shell_window_ids.h"
#include "base/base64.h"
#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/stringprintf.h"
#include "base/task/post_task.h"
#include "base/values.h"
#include "chrome/browser/chromeos/arc/tracing/arc_graphics_jank_detector.h"
#include "chrome/browser/chromeos/arc/tracing/arc_tracing_graphics_model.h"
#include "chrome/browser/chromeos/arc/tracing/arc_tracing_model.h"
#include "chrome/browser/chromeos/file_manager/path_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ash/launcher/arc_app_window_launcher_controller.h"
#include "components/arc/arc_prefs.h"
#include "components/exo/shell_surface_util.h"
#include "components/exo/surface.h"
#include "components/exo/wm_helper.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/tracing_controller.h"
#include "content/public/browser/web_ui.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/base/ui_base_features.h"
#include "ui/gfx/codec/png_codec.h"

namespace chromeos {

namespace {

constexpr char kKeyIcon[] = "icon";
constexpr char kKeyTitle[] = "title";
constexpr char kKeyTasks[] = "tasks";

constexpr char kLastTracingModelName[] = "last_tracing_model.json";

// Maximum interval to display.
constexpr base::TimeDelta kMaxIntervalToDisplay =
    base::TimeDelta::FromSecondsD(2.0);

base::FilePath GetLastTracingModelPath(Profile* profile) {
  DCHECK(profile);
  return file_manager::util::GetDownloadsFolderForProfile(profile).AppendASCII(
      kLastTracingModelName);
}

std::pair<base::Value, std::string> MaybeLoadLastGraphicsModel(
    const base::FilePath& last_model_path) {
  std::string json_content;
  if (!base::ReadFileToString(last_model_path, &json_content))
    return std::make_pair(base::Value(), std::string());

  base::Optional<base::Value> model = base::JSONReader::Read(json_content);
  if (!model || !model->is_dict())
    return std::make_pair(base::Value(), "Failed to read last tracing model");

  arc::ArcTracingGraphicsModel graphics_model;
  base::DictionaryValue* dictionary = nullptr;
  model->GetAsDictionary(&dictionary);
  if (!graphics_model.LoadFromValue(*dictionary))
    return std::make_pair(base::Value(), "Failed to load last tracing model");

  return std::make_pair(std::move(*model), "Loaded last tracing model");
}

std::pair<base::Value, std::string> BuildGraphicsModel(
    const std::string& data,
    base::DictionaryValue tasks_info,
    const base::TimeTicks& time_min,
    const base::TimeTicks& time_max,
    const base::FilePath& last_model_path) {
  arc::ArcTracingModel common_model;
  const base::TimeTicks time_min_clamped =
      std::max(time_min, time_max - kMaxIntervalToDisplay);
  common_model.SetMinMaxTime(
      (time_min_clamped - base::TimeTicks()).InMicroseconds(),
      (time_max - base::TimeTicks()).InMicroseconds());

  if (!common_model.Build(data))
    return std::make_pair(base::Value(), "Failed to process tracing data");

  arc::ArcTracingGraphicsModel graphics_model;
  if (!graphics_model.Build(common_model))
    return std::make_pair(base::Value(), "Failed to build tracing model");

  std::unique_ptr<base::DictionaryValue> model = graphics_model.Serialize();
  model->SetKey(kKeyTasks, std::move(tasks_info));

  std::string json_content;
  base::JSONWriter::WriteWithOptions(
      *model, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json_content);
  DCHECK(!json_content.empty());

  if (!base::WriteFile(last_model_path, json_content.c_str(),
                       json_content.length())) {
    LOG(ERROR) << "Failed serialize model to " << last_model_path.value()
               << ".";
  }

  return std::make_pair(std::move(*model), "Tracing model is ready");
}

}  // namespace

ArcGraphicsTracingHandler::ArcGraphicsTracingHandler()
    : wm_helper_(exo::WMHelper::HasInstance() ? exo::WMHelper::GetInstance()
                                              : nullptr),
      weak_ptr_factory_(this) {
  DCHECK(wm_helper_);

  aura::Window* const current_active = wm_helper_->GetActiveWindow();
  if (current_active) {
    OnWindowActivated(ActivationReason::ACTIVATION_CLIENT /* not used */,
                      current_active, nullptr);
  }
  wm_helper_->AddActivationObserver(this);
}

ArcGraphicsTracingHandler::~ArcGraphicsTracingHandler() {
  wm_helper_->RemoveActivationObserver(this);
  DiscardActiveArcWindow();

  if (tracing_active_)
    StopTracing();
}

void ArcGraphicsTracingHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "ready", base::BindRepeating(&ArcGraphicsTracingHandler::HandleReady,
                                   base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setStopOnJank",
      base::BindRepeating(&ArcGraphicsTracingHandler::HandleSetStopOnJank,
                          base::Unretained(this)));
}

void ArcGraphicsTracingHandler::OnWindowActivated(ActivationReason reason,
                                                  aura::Window* gained_active,
                                                  aura::Window* lost_active) {
  // Handle ARC current active window if any.
  DiscardActiveArcWindow();

  active_task_id_ =
      ArcAppWindowLauncherController::GetWindowTaskId(gained_active);
  if (active_task_id_ <= 0)
    return;

  arc_active_window_ = gained_active;
  arc_active_window_->AddObserver(this);
  arc_active_window_->AddPreTargetHandler(this);

  // Limit tracing by newly activated window.
  tracing_time_min_ = TRACE_TIME_TICKS_NOW();
  jank_detector_ =
      std::make_unique<arc::ArcGraphicsJankDetector>(base::BindRepeating(
          &ArcGraphicsTracingHandler::OnJankDetected, base::Unretained(this)));

  UpdateActiveArcWindowInfo();

  exo::Surface* const surface = exo::GetShellMainSurface(arc_active_window_);
  DCHECK(surface);
  surface->SetCommitCallback(base::BindRepeating(
      &ArcGraphicsTracingHandler::OnCommit, weak_ptr_factory_.GetWeakPtr()));
}

void ArcGraphicsTracingHandler::OnCommit(exo::Surface* surface) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  jank_detector_->OnSample();
}

void ArcGraphicsTracingHandler::OnJankDetected(const base::Time& timestamp) {
  VLOG(1) << "Jank detected " << timestamp;
  if (tracing_active_ && stop_on_jank_)
    StopTracing();
}

void ArcGraphicsTracingHandler::OnWindowPropertyChanged(aura::Window* window,
                                                        const void* key,
                                                        intptr_t old) {
  DCHECK_EQ(arc_active_window_, window);
  if (key != aura::client::kAppIconKey)
    return;

  UpdateActiveArcWindowInfo();
}

void ArcGraphicsTracingHandler::OnWindowDestroying(aura::Window* window) {
  DCHECK_EQ(arc_active_window_, window);
  DiscardActiveArcWindow();
}

void ArcGraphicsTracingHandler::OnKeyEvent(ui::KeyEvent* event) {
  DCHECK(arc_active_window_);
  if (event->type() != ui::ET_KEY_RELEASED || event->key_code() != ui::VKEY_G ||
      !event->IsControlDown() || !event->IsShiftDown()) {
    return;
  }
  if (tracing_active_)
    StopTracing();
  else
    StartTracing();
}

void ArcGraphicsTracingHandler::UpdateActiveArcWindowInfo() {
  DCHECK(arc_active_window_);
  base::DictionaryValue task_information;
  task_information.SetKey(kKeyTitle,
                          base::Value(arc_active_window_->GetTitle()));

  const gfx::ImageSkia* app_icon =
      arc_active_window_->GetProperty(aura::client::kAppIconKey);
  if (app_icon) {
    std::vector<unsigned char> png_data;
    if (gfx::PNGCodec::EncodeBGRASkBitmap(
            app_icon->GetRepresentation(1.0f).GetBitmap(),
            false /* discard_transparency */, &png_data)) {
      const std::string png_data_as_string(
          reinterpret_cast<const char*>(&png_data[0]), png_data.size());
      std::string icon_content;
      base::Base64Encode(png_data_as_string, &icon_content);
      task_information.SetKey(kKeyIcon, base::Value(icon_content));
    }
  }

  tasks_info_.SetKey(base::StringPrintf("%d", active_task_id_),
                     std::move(task_information));
}

void ArcGraphicsTracingHandler::DiscardActiveArcWindow() {
  if (!arc_active_window_)
    return;

  exo::Surface* const surface = exo::GetShellMainSurface(arc_active_window_);
  DCHECK(surface);
  surface->SetCommitCallback(exo::Surface::CommitCallback());

  arc_active_window_->RemovePreTargetHandler(this);
  arc_active_window_->RemoveObserver(this);
  jank_detector_.reset();
  arc_active_window_ = nullptr;
}

void ArcGraphicsTracingHandler::StartTracing() {
  SetStatus("Collecting samples...");

  base::trace_event::TraceConfig config(
      "-*,exo,viz,toplevel,gpu,cc,blink,disabled-by-default-android "
      "gfx,disabled-by-default-android hal",
      base::trace_event::RECORD_CONTINUOUSLY);
  config.EnableSystrace();
  tracing_active_ = true;
  if (jank_detector_)
    jank_detector_->Reset();
  content::TracingController::GetInstance()->StartTracing(
      config, base::BindOnce(&ArcGraphicsTracingHandler::OnTracingStarted,
                             weak_ptr_factory_.GetWeakPtr()));
}

void ArcGraphicsTracingHandler::StopTracing() {
  SetStatus("Building model...");

  tracing_active_ = false;

  tracing_time_max_ = TRACE_TIME_TICKS_NOW();

  content::TracingController* const controller =
      content::TracingController::GetInstance();

  if (!controller->IsTracing())
    return;

  controller->StopTracing(content::TracingController::CreateStringEndpoint(
      base::BindRepeating(&ArcGraphicsTracingHandler::OnTracingStopped,
                          weak_ptr_factory_.GetWeakPtr())));
}

void ArcGraphicsTracingHandler::SetStatus(const std::string& status) {
  AllowJavascript();
  CallJavascriptFunction("cr.ArcGraphicsTracing.setStatus",
                         base::Value(status.empty() ? "Idle" : status));
}

void ArcGraphicsTracingHandler::OnTracingStarted() {
  tasks_info_.Clear();
  UpdateActiveArcWindowInfo();

  tracing_time_min_ = TRACE_TIME_TICKS_NOW();
}

void ArcGraphicsTracingHandler::OnTracingStopped(
    std::unique_ptr<const base::DictionaryValue> metadata,
    base::RefCountedString* trace_data) {
  std::string string_data;
  string_data.swap(trace_data->data());

  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
      base::BindOnce(&BuildGraphicsModel, std::move(string_data),
                     std::move(tasks_info_), tracing_time_min_,
                     tracing_time_max_,
                     GetLastTracingModelPath(Profile::FromWebUI(web_ui()))),
      base::BindOnce(&ArcGraphicsTracingHandler::OnGraphicsModelReady,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArcGraphicsTracingHandler::OnGraphicsModelReady(
    std::pair<base::Value, std::string> result) {
  SetStatus(result.second);

  if (!result.first.is_dict())
    return;

  CallJavascriptFunction("cr.ArcGraphicsTracing.setModel",
                         std::move(result.first));
}

void ArcGraphicsTracingHandler::HandleReady(const base::ListValue* args) {
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
      base::BindOnce(&MaybeLoadLastGraphicsModel,
                     GetLastTracingModelPath(Profile::FromWebUI(web_ui()))),
      base::BindOnce(&ArcGraphicsTracingHandler::OnGraphicsModelReady,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArcGraphicsTracingHandler::HandleSetStopOnJank(
    const base::ListValue* args) {
  DCHECK_EQ(1U, args->GetSize());
  if (!args->GetList()[0].is_bool()) {
    LOG(ERROR) << "Invalid input";
    return;
  }
  stop_on_jank_ = args->GetList()[0].GetBool();
}

}  // namespace chromeos
