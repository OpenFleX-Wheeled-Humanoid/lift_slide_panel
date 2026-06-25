#include "lift_slide_panel/lift_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <string>

#include <pluginlib/class_list_macros.hpp>
#include <QDialog>
#include <QDockWidget>
#include <QMessageBox>
#include <QScrollArea>
#include <QMetaObject>

namespace lift_slide_panel
{
namespace
{
template<typename T>
void applyOnUi(QObject * object, T && fn)
{
  if (object == nullptr) {
    return;
  }
  QMetaObject::invokeMethod(
    object,
    [fn = std::forward<T>(fn)]() mutable { fn(); },
    Qt::QueuedConnection);
}

bool parseIntField(const std::string & payload, const std::string & key, uint32_t & out_value)
{
  const std::string marker = key + "=";
  const auto begin = payload.find(marker);
  if (begin == std::string::npos) {
    return false;
  }
  const auto value_start = begin + marker.size();
  const auto value_end = payload.find(',', value_start);
  const std::string token = payload.substr(
    value_start,
    value_end == std::string::npos ? std::string::npos : value_end - value_start);
  if (token.empty()) {
    return false;
  }

  try {
    out_value = static_cast<uint32_t>(std::stoul(token, nullptr, 0));
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

QString homingActionText(HomingAction action)
{
  switch (action) {
    case HomingAction::SET_ZERO:
      return QStringLiteral("设置零点");
    case HomingAction::RETURN_HOME:
      return QStringLiteral("回零");
    case HomingAction::NONE:
    default:
      return QStringLiteral("回零");
  }
}
}  // namespace

void LiftPanel::postToUi(std::function<void()> fn)
{
  applyOnUi(this, std::move(fn));
}

void LiftPanel::resetManualMotionState()
{
  move_up_active_ = false;
  move_down_active_ = false;
  pending_manual_direction_ = 0;
  manual_motion_phase_ = ManualMotionPhase::IDLE;
  active_manual_motion_epoch_ = 0;
  if (long_press_timer_ != nullptr) {
    long_press_timer_->stop();
  }
  if (repeat_move_timer_ != nullptr) {
    repeat_move_timer_->stop();
  }
}

LiftPanel::LiftPanel(QWidget * parent)
: rviz_common::Panel(parent)
{
  setupUi();
}

LiftPanel::~LiftPanel()
{
  if (ui_timer_ != nullptr) {
    ui_timer_->stop();
  }
  stopMotion();
}

void LiftPanel::onInitialize()
{
  setupRos();
  last_motion_update_time_ = std::chrono::steady_clock::now();

  // Apply height range from ROS parameters to position bar
  if (position_bar_ != nullptr) {
    position_bar_->setRange(display_min_height_, display_max_height_);
  }
  if (bar_min_label_ != nullptr) {
    bar_min_label_->setText(QString::number(display_min_height_, 'f', 3) + QStringLiteral(" m"));
  }
  if (bar_max_label_ != nullptr) {
    bar_max_label_->setText(QString::number(display_max_height_, 'f', 3) + QStringLiteral(" m"));
  }

  ui_timer_ = new QTimer(this);
  connect(ui_timer_, &QTimer::timeout, this, &LiftPanel::onUiTimer);
  ui_timer_->start(50);

  // 长按判定定时器：100ms 后区分单点/长按
  long_press_timer_ = new QTimer(this);
  long_press_timer_->setSingleShot(true);
  connect(long_press_timer_, &QTimer::timeout, this, &LiftPanel::onLongPressTimeout);

  // 长按状态监控定时器，目标生成由 lift_manual_position_controller 负责。
  repeat_move_timer_ = new QTimer(this);
  connect(repeat_move_timer_, &QTimer::timeout, this, &LiftPanel::onRepeatMove);

  QTimer::singleShot(0, this, [this]() { ensureFloatingWindow(); });
}

void LiftPanel::setupUi()
{
  setStyleSheet(R"(
    QWidget {
      background: #FAFAFA;
      color: #1A1A1A;
      font-size: 12px;
    }
    QPushButton {
      background: #FFFFFF;
      border: 1px solid #D0D0D0;
      border-radius: 6px;
      padding: 7px 14px;
      min-height: 22px;
      font-size: 12px;
    }
    QPushButton:hover { background: #F0F0F0; border-color: #B0B0B0; }
    QPushButton:pressed { background: #E0E0E0; }
    QPushButton:disabled { color: #A0A0A0; background: #F5F5F5; border-color: #E0E0E0; }
    QSlider::groove:horizontal {
      border: none; height: 6px; background: #E0E0E0; border-radius: 3px;
    }
    QSlider::handle:horizontal {
      background: #5B5B5B; width: 16px; margin: -5px 0;
      border-radius: 8px; border: 2px solid #404040;
    }
    QSlider::sub-page:horizontal { background: #2563EB; border-radius: 3px; }
  )");

  auto create_lamp = [](const QString & title, QFrame ** lamp_ptr, QLabel ** text_ptr) {
    auto * w = new QWidget();
    auto * l = new QVBoxLayout(w);
    l->setSpacing(3);
    l->setContentsMargins(0, 0, 0, 0);
    auto * lamp = new QFrame();
    lamp->setFixedSize(18, 18);
    lamp->setStyleSheet("background:#9CA3AF;border-radius:9px;border:1px solid #6B7280;");
    auto * label = new QLabel(title);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("font-size:10px;color:#666;");
    l->addWidget(lamp, 0, Qt::AlignCenter);
    l->addWidget(label);
    *lamp_ptr = lamp;
    *text_ptr = label;
    return w;
  };

  auto * root = new QVBoxLayout();
  root->setSpacing(6);
  root->setContentsMargins(8, 6, 8, 6);

  // ─── Title bar with status indicators ───
  auto * title_bar = new QHBoxLayout();
  auto * title = new QLabel(QStringLiteral("升降立柱控制"));
  title->setStyleSheet("font-size:14px;font-weight:bold;color:#1A1A1A;");
  title_bar->addWidget(title);
  title_bar->addStretch();
  comm_status_indicator_ = new QLabel(QStringLiteral("--"));
  comm_status_indicator_->setStyleSheet("font-size:11px;font-weight:bold;color:#999;");
  drive_status_indicator_ = new QLabel(QStringLiteral("--"));
  drive_status_indicator_->setStyleSheet("font-size:11px;font-weight:bold;color:#999;");
  title_bar->addWidget(comm_status_indicator_);
  auto * sep = new QLabel(QStringLiteral(" | "));
  sep->setStyleSheet("color:#CCC;font-size:11px;");
  title_bar->addWidget(sep);
  title_bar->addWidget(drive_status_indicator_);
  root->addLayout(title_bar);

  // ─── Separator ───
  auto * line1 = new QFrame();
  line1->setFrameShape(QFrame::HLine);
  line1->setStyleSheet("color:#E0E0E0;");
  line1->setFixedHeight(1);
  root->addWidget(line1);

  // ─── Realtime data ───
  auto * data_row = new QHBoxLayout();
  position_display_label_ = new QLabel(QStringLiteral("-- m"));
  position_display_label_->setStyleSheet("font-size:22px;font-weight:bold;color:#2563EB;");
  speed_display_label_ = new QLabel(QStringLiteral("速度 0.000 m/s"));
  speed_display_label_->setStyleSheet("font-size:13px;font-weight:bold;color:#444;");
  speed_display_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  data_row->addWidget(position_display_label_);
  data_row->addStretch();
  data_row->addWidget(speed_display_label_);
  root->addLayout(data_row);

  position_bar_ = new PositionBarWidget();
  root->addWidget(position_bar_);

  auto * bar_range = new QHBoxLayout();
  bar_min_label_ = new QLabel(QStringLiteral("-0.650 m"));
  bar_min_label_->setStyleSheet("font-size:9px;color:#AAA;");
  bar_max_label_ = new QLabel(QStringLiteral("0.300 m"));
  bar_max_label_->setStyleSheet("font-size:9px;color:#AAA;");
  bar_range->addWidget(bar_min_label_);
  bar_range->addStretch();
  bar_range->addWidget(bar_max_label_);
  root->addLayout(bar_range);

  // ─── Limit lamps (compact horizontal) ───
  auto * lamp_bar = new QHBoxLayout();
  lamp_bar->setContentsMargins(0, 2, 0, 2);
  auto * limit_title = new QLabel(QStringLiteral("限位"));
  limit_title->setStyleSheet("font-size:11px;color:#888;font-weight:bold;");
  lamp_bar->addWidget(limit_title);
  lamp_bar->addSpacing(8);
  lamp_bar->addWidget(
    create_lamp(QStringLiteral("上限位"), &upper_limit_lamp_, &upper_limit_label_));
  lamp_bar->addSpacing(16);
  lamp_bar->addWidget(
    create_lamp(QStringLiteral("零点"), &home_limit_lamp_, &home_limit_label_));
  lamp_bar->addSpacing(16);
  lamp_bar->addWidget(
    create_lamp(QStringLiteral("下限位"), &lower_limit_lamp_, &lower_limit_label_));
  lamp_bar->addStretch();
  zero_state_label_ = new QLabel(QStringLiteral("零点:未设置"));
  zero_state_label_->setStyleSheet("font-size:11px;font-weight:bold;color:#888;");
  lamp_bar->addWidget(zero_state_label_);
  root->addLayout(lamp_bar);

  // ─── Separator ───
  auto * line2 = new QFrame();
  line2->setFrameShape(QFrame::HLine);
  line2->setStyleSheet("color:#E0E0E0;");
  line2->setFixedHeight(1);
  root->addWidget(line2);

  // ─── Drive control row ───
  auto * drive_row = new QHBoxLayout();
  enable_button_ = new QPushButton(QStringLiteral("使能"));
  enable_button_->setStyleSheet(
    "QPushButton{background:#16A34A;color:white;font-weight:bold;border:none;"
    "border-radius:5px;padding:6px 12px;}"
    "QPushButton:hover{background:#15803D;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  disable_button_ = new QPushButton(QStringLiteral("掉电"));
  disable_button_->setStyleSheet(
    "QPushButton{background:#F59E0B;color:white;font-weight:bold;border:none;"
    "border-radius:5px;padding:6px 12px;}"
    "QPushButton:hover{background:#D97706;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  reset_button_ = new QPushButton(QStringLiteral("复位"));
  reset_button_->setStyleSheet(
    "QPushButton{padding:6px 10px;border-radius:5px;}");
  drive_state_label_ = new QLabel(QStringLiteral("未使能"));
  drive_state_label_->setStyleSheet("font-weight:bold;font-size:12px;color:#888;");
  drive_row->addWidget(enable_button_);
  drive_row->addWidget(disable_button_);
  drive_row->addWidget(reset_button_);
  drive_row->addStretch();
  drive_row->addWidget(drive_state_label_);
  root->addLayout(drive_row);

  // ─── Speed display + gear button ───
  auto * speed_row = new QHBoxLayout();
  auto * speed_label = new QLabel(QStringLiteral("速度"));
  speed_label->setStyleSheet("font-size:11px;color:#666;");
  speed_value_label_ = new QLabel(QStringLiteral("0.010 m/s"));
  speed_value_label_->setStyleSheet("font-size:11px;font-weight:bold;color:#2563EB;");
  gear_button_ = new QPushButton(QStringLiteral("\u2699"));
  gear_button_->setFixedSize(30, 30);
  gear_button_->setStyleSheet(
    "QPushButton{font-size:16px;padding:0;border-radius:15px;border:1px solid #D0D0D0;background:#FFF;}"
    "QPushButton:hover{background:#E8E8E8;}");
  speed_row->addWidget(speed_label);
  speed_row->addWidget(speed_value_label_);
  speed_row->addStretch();
  speed_row->addWidget(gear_button_);
  root->addLayout(speed_row);

  // ─── Direction buttons ───
  auto * dir_row = new QHBoxLayout();
  dir_row->setSpacing(6);
  up_button_ = new QPushButton(QStringLiteral("\u25B2 上移"));
  up_button_->setStyleSheet(
    "QPushButton{background:#059669;color:white;font-weight:bold;border:none;"
    "border-radius:6px;padding:12px 0;font-size:13px;}"
    "QPushButton:hover{background:#047857;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  down_button_ = new QPushButton(QStringLiteral("\u25BC 下移"));
  down_button_->setStyleSheet(
    "QPushButton{background:#059669;color:white;font-weight:bold;border:none;"
    "border-radius:6px;padding:12px 0;font-size:13px;}"
    "QPushButton:hover{background:#047857;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  dir_row->addWidget(up_button_, 1);
  dir_row->addWidget(down_button_, 1);
  root->addLayout(dir_row);

  // ─── Separator ───
  auto * line3 = new QFrame();
  line3->setFrameShape(QFrame::HLine);
  line3->setStyleSheet("color:#E0E0E0;");
  line3->setFixedHeight(1);
  root->addWidget(line3);

  // ─── Homing row (compact) ───
  auto * homing_row = new QHBoxLayout();
  set_zero_button_ = new QPushButton(QStringLiteral("设置零点"));
  set_zero_button_->setStyleSheet(
    "QPushButton{background:#7C3AED;color:white;font-weight:bold;border:none;"
    "border-radius:5px;padding:6px 12px;}"
    "QPushButton:hover{background:#6D28D9;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  return_home_button_ = new QPushButton(QStringLiteral("回零"));
  return_home_button_->setStyleSheet(
    "QPushButton{background:#2563EB;color:white;font-weight:bold;border:none;"
    "border-radius:5px;padding:6px 12px;}"
    "QPushButton:hover{background:#1D4ED8;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  stop_homing_button_ = new QPushButton(QStringLiteral("停止"));
  stop_homing_button_->setStyleSheet(
    "QPushButton{background:#DC2626;color:white;font-weight:bold;border:none;"
    "border-radius:5px;padding:6px 12px;}"
    "QPushButton:hover{background:#B91C1C;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;}");
  homing_row->addWidget(set_zero_button_);
  homing_row->addWidget(return_home_button_);
  homing_row->addWidget(stop_homing_button_);
  homing_row->addStretch();
  root->addLayout(homing_row);

  // ─── Detail / feedback label ───
  detail_label_ = new QLabel(QStringLiteral("就绪"));
  detail_label_->setWordWrap(true);
  detail_label_->setStyleSheet("font-size:11px;color:#666;padding:2px 0;");
  root->addWidget(detail_label_);

  root->addStretch();

  // ─── E-Stop (always at bottom) ───
  estop_button_ = new QPushButton(QStringLiteral("!! 急  停 !!"));
  estop_button_->setFixedHeight(44);
  estop_button_->setStyleSheet(
    "QPushButton{background:#DC2626;color:white;font-weight:bold;font-size:16px;"
    "border:2px solid #991B1B;border-radius:8px;}"
    "QPushButton:hover{background:#B91C1C;}"
    "QPushButton:pressed{background:#991B1B;}"
    "QPushButton:disabled{background:#E5E7EB;color:#9CA3AF;border-color:#E5E7EB;}");
  root->addWidget(estop_button_);

  // ─── Scroll wrapper ───
  auto * content = new QWidget();
  content->setLayout(root);
  auto * scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(content);
  auto * outer = new QVBoxLayout();
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll);
  setLayout(outer);

  // ─── Create sliders (hidden, for popup dialog) ───
  speed_slider_ = new QSlider(Qt::Horizontal);
  speed_slider_->setRange(10, 100);  // 0.01 ~ 0.10 m/s
  speed_slider_->setValue(50);       // 默认 0.05 m/s
  speed_slider_->setParent(this);
  speed_slider_->hide();

  step_slider_ = new QSlider(Qt::Horizontal);
  step_slider_->setRange(5, 100);    // 0.005 ~ 0.100 m
  step_slider_->setValue(1);        // 默认最小步长
  step_slider_->setParent(this);
  step_slider_->hide();

  // Signal/slot connections
  connect(enable_button_, &QPushButton::clicked, this, &LiftPanel::onEnableClicked);
  connect(disable_button_, &QPushButton::clicked, this, &LiftPanel::onDisableClicked);
  connect(reset_button_, &QPushButton::clicked, this, &LiftPanel::onResetClicked);
  connect(set_zero_button_, &QPushButton::clicked, this, &LiftPanel::onSetZeroClicked);
  connect(return_home_button_, &QPushButton::clicked, this, &LiftPanel::onReturnHomeClicked);
  connect(up_button_, &QPushButton::pressed, this, &LiftPanel::onUpPressed);
  connect(down_button_, &QPushButton::pressed, this, &LiftPanel::onDownPressed);
  connect(up_button_, &QPushButton::released, this, &LiftPanel::onManualMoveReleased);
  connect(down_button_, &QPushButton::released, this, &LiftPanel::onManualMoveReleased);
  connect(stop_homing_button_, &QPushButton::clicked, this, &LiftPanel::onStopHomingClicked);
  connect(estop_button_, &QPushButton::clicked, this, &LiftPanel::onDisableClicked);
  connect(gear_button_, &QPushButton::clicked, this, &LiftPanel::onGearButtonClicked);

  updateMotionValueLabels();
  updateLimitLamps();
  updateStatusSummary();
}

void LiftPanel::setupRos()
{
  if (!rclcpp::ok()) {
    int argc = 0;
    char ** argv = nullptr;
    rclcpp::init(argc, argv);
  }

  node_ = std::make_shared<rclcpp::Node>(
    "lift_slide_panel",
    rclcpp::NodeOptions().use_global_arguments(true));

  node_->declare_parameter<double>("pos_min", -0.650);
  node_->declare_parameter<double>("pos_max", 0.300);
  node_->declare_parameter<std::string>(
    "position_command_topic", "/lift_position_controller/commands");
  display_min_height_ = node_->get_parameter("pos_min").as_double();
  display_max_height_ = node_->get_parameter("pos_max").as_double();
  const std::string position_command_topic =
    node_->get_parameter("position_command_topic").as_string();

  position_cmd_pub_ = node_->create_publisher<std_msgs::msg::Float64MultiArray>(
    position_command_topic,
    rclcpp::SystemDefaultsQoS());
  RCLCPP_INFO(
    node_->get_logger(), "升降台面板位置控制话题: %s", position_command_topic.c_str());

  // 创建速度命令发布器（发布到 lift_state_controller 的速度话题）
  speed_cmd_pub_ = node_->create_publisher<std_msgs::msg::Float64>(
    "/lift_state_controller/profile_speed_cmd",
    rclcpp::SystemDefaultsQoS());

  driver_homing_state_sub_ = node_->create_subscription<std_msgs::msg::String>(
    "/lift_slide_driver/homing_state",
    rclcpp::QoS(1).reliable(),
    std::bind(&LiftPanel::driverHomingStateCallback, this, std::placeholders::_1));

  limit_switch_state_sub_ = node_->create_subscription<std_msgs::msg::String>(
    "/lift_slide_driver/limit_switch_state",
    rclcpp::QoS(1).reliable(),
    std::bind(&LiftPanel::driverLimitSwitchStateCallback, this, std::placeholders::_1));

  motor_status_sub_ = node_->create_subscription<lift_slide_msgs::msg::MotorStatus>(
    "/lift_slide_driver/motor_status",
    rclcpp::SensorDataQoS(),
    std::bind(&LiftPanel::motorStatusCallback, this, std::placeholders::_1));

  start_homing_client_ = node_->create_client<std_srvs::srv::Trigger>(
    "/lift_slide_driver/start_homing");
  return_home_client_ = node_->create_client<std_srvs::srv::Trigger>(
    "/lift_slide_driver/return_home");
  enable_client_ = node_->create_client<std_srvs::srv::Trigger>(
    "/lift_slide_driver/enable");
  quick_stop_client_ = node_->create_client<std_srvs::srv::Trigger>(
    "/lift_slide_driver/quick_stop");
  manual_step_pub_ = node_->create_publisher<std_msgs::msg::Float64MultiArray>(
    "/lift_manual_position_controller/step_command", rclcpp::SystemDefaultsQoS());
  manual_jog_pub_ = node_->create_publisher<std_msgs::msg::Float64>(
    "/lift_manual_position_controller/jog_command", rclcpp::SystemDefaultsQoS());

  last_joint_update_time_ = std::chrono::steady_clock::now();
}

void LiftPanel::ensureFloatingWindow()
{
  if (floating_window_applied_) {
    return;
  }

  QWidget * parent_widget = this;
  QDockWidget * dock_widget = nullptr;
  while (parent_widget != nullptr) {
    dock_widget = qobject_cast<QDockWidget *>(parent_widget);
    if (dock_widget != nullptr) {
      break;
    }
    parent_widget = parent_widget->parentWidget();
  }

  if (dock_widget == nullptr) {
    if (floating_window_retry_count_ < 20) {
      ++floating_window_retry_count_;
      QTimer::singleShot(150, this, [this]() { ensureFloatingWindow(); });
    }
    return;
  }

  floating_window_applied_ = true;
  if (!dock_widget->isFloating()) {
    dock_widget->setFloating(true);
  }
  dock_widget->resize(420, 580);
}

void LiftPanel::driverHomingStateCallback(const std_msgs::msg::String::SharedPtr msg)
{
  const std::string payload = msg->data;
  std::string state = payload;
  std::string detail;
  const auto split_pos = payload.find(':');
  if (split_pos != std::string::npos) {
    state = payload.substr(0, split_pos);
    detail = payload.substr(split_pos + 1);
  }

  last_homing_detail_ = detail.empty() ? state : detail;

  if (state == "IN_PROGRESS") {
    driver_homing_active_ = true;
    resetManualMotionState();
    drive_enabled_known_ = true;
    drive_enabled_ = true;
    const QString action_text = homingActionText(pending_homing_action_);
    if (detail == "accepted") {
      detail_label_->setText(action_text + QStringLiteral("已接受，等待驱动执行..."));
    } else if (detail == "configuring") {
      detail_label_->setText(action_text + QStringLiteral("：配置参数中..."));
    } else if (detail == "moving_up_to_upper") {
      detail_label_->setText(action_text + QStringLiteral("：上行触发上限位..."));
    } else if (detail == "moving_up_to_home") {
      detail_label_->setText(action_text + QStringLiteral("：上行寻找 HOME 开关..."));
    } else if (detail == "moving_down_to_home") {
      detail_label_->setText(action_text + QStringLiteral("：下行返回 HOME 开关..."));
    } else {
      detail_label_->setText(
        action_text + QStringLiteral("进行中：") + QString::fromStdString(detail));
    }
    return;
  }

  if (state == "COMPLETED") {
    driver_homing_active_ = false;
    driver_homed_ = true;
    drive_enabled_known_ = true;
    drive_enabled_ = true;
    resetManualMotionState();
    const QString action_text = homingActionText(pending_homing_action_);
    detail_label_->setText(action_text + QStringLiteral("完成，当前位置已对准 HOME"));
    pending_homing_action_ = HomingAction::NONE;
    return;
  }

  if (state == "ERROR") {
    driver_homing_active_ = false;
    resetManualMotionState();
    if (detail == "aborted_by_quick_stop" || detail == "quick_stop_active") {
      if (homing_stop_reenable_pending_) {
        // We intentionally stopped and are about to re-enable — don't show "已失能"
        detail_label_->setText(QStringLiteral("正在停止并恢复驱动..."));
      } else {
        drive_enabled_known_ = true;
        drive_enabled_ = false;
        detail_label_->setText(QStringLiteral("当前动作已停止，驱动已失能"));
      }
    } else {
      const QString action_text = homingActionText(pending_homing_action_);
      detail_label_->setText(
        action_text + QStringLiteral("失败：") +
        QString::fromStdString(detail.empty() ? "failed" : detail));
    }
    pending_homing_action_ = HomingAction::NONE;
    return;
  }

  if (state == "IDLE") {
    driver_homing_active_ = false;
    if (detail == "enabled" || detail == "ready") {
      drive_enabled_known_ = true;
      drive_enabled_ = true;
      detail_label_->setText(QStringLiteral("驱动已使能，可开始控制"));
    } else if (detail == "disabled") {
      drive_enabled_known_ = true;
      drive_enabled_ = false;
      detail_label_->setText(QStringLiteral("驱动已失能，抱闸抱死"));
    } else if (detail == "disable_failed") {
      detail_label_->setText(QStringLiteral("驱动失能失败"));
    } else if (detail == "service_ready") {
      detail_label_->setText(QStringLiteral("驱动服务已就绪"));
    }
  }
}

void LiftPanel::driverLimitSwitchStateCallback(const std_msgs::msg::String::SharedPtr msg)
{
  uint32_t raw = 0U;
  uint32_t upper = 0U;
  uint32_t home = 0U;
  uint32_t lower = 0U;
  uint32_t b0_not = 0U;
  uint32_t b1_pot = 0U;
  uint32_t b2_home = 0U;
  uint32_t b7_si4 = 0U;
  uint32_t b8_si5 = 0U;
  uint32_t b9_si6 = 0U;
  uint32_t b19_di4 = 0U;
  uint32_t b20_di5 = 0U;
  uint32_t b21_di6 = 0U;
  uint32_t b27_di4 = 0U;
  uint32_t b28_di5 = 0U;
  uint32_t b29_di6 = 0U;

  const std::string & payload = msg->data;
  const bool got_raw = parseIntField(payload, "raw", raw);
  const bool got_upper = parseIntField(payload, "upper", upper);
  const bool got_home = parseIntField(payload, "home", home);
  const bool got_lower = parseIntField(payload, "lower", lower);
  const bool got_b0 = parseIntField(payload, "b0_NOT", b0_not);
  const bool got_b1 = parseIntField(payload, "b1_POT", b1_pot);
  const bool got_b2 = parseIntField(payload, "b2_HOME", b2_home);
  const bool got_b7 = parseIntField(payload, "b7_SI4", b7_si4);
  const bool got_b8 = parseIntField(payload, "b8_SI5", b8_si5);
  const bool got_b9 = parseIntField(payload, "b9_SI6", b9_si6);
  const bool got_b19 = parseIntField(payload, "b19_DI4", b19_di4);
  const bool got_b20 = parseIntField(payload, "b20_DI5", b20_di5);
  const bool got_b21 = parseIntField(payload, "b21_DI6", b21_di6);
  const bool got_b27 = parseIntField(payload, "b27_DI4", b27_di4);
  const bool got_b28 = parseIntField(payload, "b28_DI5", b28_di5);
  const bool got_b29 = parseIntField(payload, "b29_DI6", b29_di6);

  const bool home_active = got_home ? (home != 0U)
    : ((got_b27 && b27_di4 == 0U) || (got_b7 && b7_si4 == 0U) ||
      (got_b19 && b19_di4 != 0U) || (got_b2 && b2_home != 0U));
  const bool pot_active = got_lower ? (lower != 0U)
    : ((got_b28 && b28_di5 == 0U) || (got_b8 && b8_si5 == 0U) ||
      (got_b20 && b20_di5 != 0U) || (got_b1 && b1_pot != 0U));
  const bool not_active = got_upper ? (upper != 0U)
    : ((got_b29 && b29_di6 == 0U) || (got_b9 && b9_si6 == 0U) ||
      (got_b21 && b21_di6 != 0U) || (got_b0 && b0_not != 0U));

  {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    if (got_raw) {
      limit_raw_ = raw;
    }
    limit_home_ = home_active;
    limit_pot_ = pot_active;
    limit_not_ = not_active;
  }
  limit_switch_online_.store(true);
}

void LiftPanel::motorStatusCallback(const lift_slide_msgs::msg::MotorStatus::SharedPtr msg)
{
  // Update drive state from MotorStatus message
  drive_enabled_known_ = true;
  drive_enabled_ = msg->is_enabled;
  drive_fault_ = msg->is_fault;
  cia402_state_str_ = msg->cia402_state;
  mode_name_str_ = msg->mode_of_operation;

  // Update homing state
  driver_homed_ = msg->homing_complete;
  driver_homing_active_ = msg->is_homing;

  // Update position/velocity from motor_status (more reliable than joint_states)
  joint_position_.store(msg->position_m);
  joint_velocity_.store(msg->velocity_mps);
  joint_valid_.store(true);
  last_joint_update_time_ = std::chrono::steady_clock::now();

  // Update limit switch data from MotorStatus
  if (msg->limit_switch_valid) {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    limit_raw_ = msg->digital_inputs_raw;
    limit_home_ = msg->home_switch;
    limit_pot_ = msg->lower_limit_switch;
    limit_not_ = msg->upper_limit_switch;
    limit_switch_online_.store(true);
  }
}

void LiftPanel::requestHoming(HomingAction action)
{
  if (!isCommunicationOk()) {
    detail_label_->setText(QStringLiteral("等待驱动反馈，暂不能执行零点操作"));
    return;
  }
  if (!drive_enabled_known_ || !drive_enabled_) {
    detail_label_->setText(QStringLiteral("请先使能电机，再执行零点操作"));
    return;
  }
  if (driver_homing_active_) {
    detail_label_->setText(QStringLiteral("当前已有零点操作在执行"));
    return;
  }

  if (start_homing_client_ == nullptr ||
      !start_homing_client_->wait_for_service(std::chrono::milliseconds(300)))
  {
    QMessageBox::warning(
      this, QStringLiteral("服务不可用"),
      QStringLiteral("/lift_slide_driver/start_homing 未就绪。"));
    return;
  }

  stopMotion();
  pending_homing_action_ = action;
  driver_homing_active_ = true;
  detail_label_->setText(homingActionText(action) + QStringLiteral("请求已发出..."));

  auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
  start_homing_client_->async_send_request(
    request,
    [this, action](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
      try {
        const auto response = future.get();
        if (!response->success) {
          postToUi([this, action, message = response->message]() {
            driver_homing_active_ = false;
            pending_homing_action_ = HomingAction::NONE;
            detail_label_->setText(
              homingActionText(action) + QStringLiteral("被拒绝：") +
              QString::fromStdString(message));
          });
        }
      } catch (const std::exception & e) {
        postToUi([this, action, message = std::string(e.what())]() {
          driver_homing_active_ = false;
          pending_homing_action_ = HomingAction::NONE;
          detail_label_->setText(
            homingActionText(action) + QStringLiteral("异常：") +
            QString::fromStdString(message));
        });
      }
    });
}

void LiftPanel::onEnableClicked()
{
  if (driver_homing_active_) {
    detail_label_->setText(QStringLiteral("回零进行中，不能重复使能"));
    return;
  }
  if (drive_enabled_known_ && drive_enabled_) {
    detail_label_->setText(QStringLiteral("驱动已使能"));
    return;
  }

  if (enable_client_ == nullptr ||
      !enable_client_->wait_for_service(std::chrono::milliseconds(300)))
  {
    QMessageBox::warning(
      this, QStringLiteral("服务不可用"),
      QStringLiteral("/lift_slide_driver/enable 未就绪。"));
    return;
  }

  detail_label_->setText(QStringLiteral("正在使能驱动..."));
  const bool previous_enabled = drive_enabled_;
  auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
  enable_client_->async_send_request(
    request,
    [this, previous_enabled](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
      try {
        const auto response = future.get();
        postToUi([this, previous_enabled, ok = response->success, message = response->message]() {
          drive_enabled_known_ = true;
          if (ok) {
            drive_enabled_ = true;
            detail_label_->setText(QStringLiteral("驱动使能成功"));
          } else {
            drive_enabled_ = previous_enabled;
            detail_label_->setText(
              QStringLiteral("驱动使能失败：") + QString::fromStdString(message));
          }
        });
      } catch (const std::exception & e) {
        postToUi([this, message = std::string(e.what())]() {
          detail_label_->setText(
            QStringLiteral("使能异常：") + QString::fromStdString(message));
        });
      }
    });
}

void LiftPanel::onDisableClicked()
{
  // 立即停止所有移动
  stopMotion();

  if (drive_enabled_known_ && !drive_enabled_ && !driver_homing_active_) {
    detail_label_->setText(QStringLiteral("驱动已失能"));
    return;
  }

  if (quick_stop_client_ == nullptr ||
      !quick_stop_client_->wait_for_service(std::chrono::milliseconds(300)))
  {
    QMessageBox::warning(
      this, QStringLiteral("服务不可用"),
      QStringLiteral("/lift_slide_driver/quick_stop 未就绪。"));
    return;
  }

  detail_label_->setText(QStringLiteral("正在失能驱动..."));
  const bool previous_enabled = drive_enabled_;
  auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
  quick_stop_client_->async_send_request(
    request,
    [this, previous_enabled](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
      try {
        const auto response = future.get();
        postToUi([this, previous_enabled, ok = response->success, message = response->message]() {
          drive_enabled_known_ = true;
          if (ok) {
            drive_enabled_ = false;
            detail_label_->setText(QStringLiteral("驱动已失能，抱闸抱死"));
          } else {
            drive_enabled_ = previous_enabled;
            detail_label_->setText(
              QStringLiteral("驱动失能失败：") + QString::fromStdString(message));
          }
        });
      } catch (const std::exception & e) {
        postToUi([this, message = std::string(e.what())]() {
          detail_label_->setText(
            QStringLiteral("失能异常：") + QString::fromStdString(message));
        });
      }
    });
}

void LiftPanel::onSetZeroClicked()
{
  requestHoming(HomingAction::SET_ZERO);
}

void LiftPanel::onReturnHomeClicked()
{
  if (!isCommunicationOk()) {
    detail_label_->setText(QStringLiteral("等待驱动反馈，暂不能执行回零操作"));
    return;
  }
  if (!drive_enabled_known_ || !drive_enabled_) {
    detail_label_->setText(QStringLiteral("请先使能电机，再执行回零操作"));
    return;
  }
  if (!driver_homed_) {
    detail_label_->setText(QStringLiteral("请先设置零点，再执行回零操作"));
    return;
  }
  if (driver_homing_active_) {
    detail_label_->setText(QStringLiteral("当前已有零点操作在执行"));
    return;
  }

  if (return_home_client_ == nullptr ||
      !return_home_client_->wait_for_service(std::chrono::milliseconds(300)))
  {
    QMessageBox::warning(
      this, QStringLiteral("服务不可用"),
      QStringLiteral("/lift_slide_driver/return_home 未就绪。"));
    return;
  }

  // Confirmation dialog
  const auto confirm = QMessageBox::question(
    this, QStringLiteral("确认回零"),
    QStringLiteral("立柱将自动移动到零位，确定执行回零操作？"),
    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (confirm != QMessageBox::Yes) {
    return;
  }

  stopMotion();
  pending_homing_action_ = HomingAction::RETURN_HOME;
  driver_homing_active_ = true;
  detail_label_->setText(QStringLiteral("回零请求已发出..."));

  auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
  return_home_client_->async_send_request(
    request,
    [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
      try {
        const auto response = future.get();
        if (!response->success) {
          postToUi([this, message = response->message]() {
            driver_homing_active_ = false;
            pending_homing_action_ = HomingAction::NONE;
            detail_label_->setText(
              QStringLiteral("回零被拒绝：") + QString::fromStdString(message));
          });
        }
      } catch (const std::exception & e) {
        postToUi([this, message = std::string(e.what())]() {
          driver_homing_active_ = false;
          pending_homing_action_ = HomingAction::NONE;
          detail_label_->setText(
            QStringLiteral("回零异常：") + QString::fromStdString(message));
        });
      }
    });
}

void LiftPanel::onResetClicked()
{
  pending_homing_action_ = HomingAction::NONE;
  stopMotion();

  if (driver_homing_active_ && quick_stop_client_ != nullptr &&
      quick_stop_client_->wait_for_service(std::chrono::milliseconds(200)))
  {
    detail_label_->setText(QStringLiteral("复位中：先中止当前回零..."));
    auto stop_request = std::make_shared<std_srvs::srv::Trigger::Request>();
    quick_stop_client_->async_send_request(
      stop_request,
      [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture) {
        postToUi([this]() {
          if (enable_client_ == nullptr ||
              !enable_client_->wait_for_service(std::chrono::milliseconds(300)))
          {
            detail_label_->setText(QStringLiteral("复位失败：/lift_slide_driver/enable 未就绪"));
            return;
          }
          auto enable_request = std::make_shared<std_srvs::srv::Trigger::Request>();
          enable_client_->async_send_request(
            enable_request,
            [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
              try {
                const auto response = future.get();
                postToUi([this, ok = response->success, message = response->message]() {
                  drive_enabled_known_ = true;
                  drive_enabled_ = ok;
                  detail_label_->setText(
                    ok ?
                    QStringLiteral("复位完成，驱动已重新使能") :
                    QStringLiteral("复位失败：") + QString::fromStdString(message));
                });
              } catch (const std::exception & e) {
                postToUi([this, message = std::string(e.what())]() {
                  detail_label_->setText(
                    QStringLiteral("复位异常：") + QString::fromStdString(message));
                });
              }
            });
        });
      });
    return;
  }

  if (!drive_enabled_) {
    detail_label_->setText(QStringLiteral("界面状态已复位，请先手动使能驱动"));
    return;
  }

  detail_label_->setText(QStringLiteral("界面状态已复位"));
}

void LiftPanel::onUpPressed()
{
  if (!isCommunicationOk()) {
    detail_label_->setText(QStringLiteral("等待驱动反馈，暂不能上移"));
    return;
  }
  if (!drive_enabled_known_) {
    detail_label_->setText(QStringLiteral("等待驱动状态，暂不能上移"));
    return;
  }
  if (!drive_enabled_) {
    detail_label_->setText(QStringLiteral("请先使能电机"));
    return;
  }
  if (driver_homing_active_) {
    detail_label_->setText(QStringLiteral("回零进行中，不能手动上移"));
    return;
  }
  bool upper_blocked = false;
  {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    upper_blocked = limit_not_;
  }
  if (limit_switch_online_.load() && upper_blocked) {
    move_up_active_ = false;
    move_down_active_ = false;
    detail_label_->setText(QStringLiteral("上限位已遮挡，不能继续上移"));
    return;
  }
  move_up_active_ = true;
  move_down_active_ = false;
  manual_motion_epoch_++;
  active_manual_motion_epoch_ = manual_motion_epoch_;
  manual_motion_phase_ = ManualMotionPhase::SHORT_PENDING;
  RCLCPP_INFO(
    node_->get_logger(),
    "[manual] up pressed: pos=%.4f step=%.4f speed=%.4f",
    joint_position_.load(),
    sliderStepToMeters(step_slider_->value()),
    sliderSpeedToMps(speed_slider_->value()));

  pending_manual_direction_ = 1;
  if (long_press_timer_ != nullptr) {
    long_press_timer_->start(100);
  }

  const double step_size = sliderStepToMeters(step_slider_->value());

  detail_label_->setText(QString("上移 步长: %1 m").arg(step_size, 0, 'f', 3));
}

void LiftPanel::onDownPressed()
{
  if (!isCommunicationOk()) {
    detail_label_->setText(QStringLiteral("等待驱动反馈，暂不能下移"));
    return;
  }
  if (!drive_enabled_known_) {
    detail_label_->setText(QStringLiteral("等待驱动状态，暂不能下移"));
    return;
  }
  if (!drive_enabled_) {
    detail_label_->setText(QStringLiteral("请先使能电机"));
    return;
  }
  if (driver_homing_active_) {
    detail_label_->setText(QStringLiteral("回零进行中，不能手动下移"));
    return;
  }
  bool lower_blocked = false;
  {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    lower_blocked = limit_pot_;
  }
  if (limit_switch_online_.load() && lower_blocked) {
    move_up_active_ = false;
    move_down_active_ = false;
    detail_label_->setText(QStringLiteral("下限位已遮挡，不能继续下移"));
    return;
  }
  move_down_active_ = true;
  move_up_active_ = false;
  manual_motion_epoch_++;
  active_manual_motion_epoch_ = manual_motion_epoch_;
  manual_motion_phase_ = ManualMotionPhase::SHORT_PENDING;
  RCLCPP_INFO(
    node_->get_logger(),
    "[manual] down pressed: pos=%.4f step=%.4f speed=%.4f",
    joint_position_.load(),
    sliderStepToMeters(step_slider_->value()),
    sliderSpeedToMps(speed_slider_->value()));

  pending_manual_direction_ = -1;
  if (long_press_timer_ != nullptr) {
    long_press_timer_->start(100);
  }

  const double step_size = sliderStepToMeters(step_slider_->value());

  detail_label_->setText(QString("下移 步长: %1 m").arg(step_size, 0, 'f', 3));
}

void LiftPanel::onLongPressTimeout()
{
  if (!move_up_active_ && !move_down_active_) {
    return;
  }
  if (active_manual_motion_epoch_ == 0 || active_manual_motion_epoch_ != manual_motion_epoch_) {
    return;
  }

  manual_motion_phase_ = ManualMotionPhase::CONTINUOUS;
  pending_manual_direction_ = 0;
  last_motion_update_time_ = std::chrono::steady_clock::now();
  const int direction = move_up_active_ ? 1 : -1;
  RCLCPP_INFO(
    node_->get_logger(),
    "[manual] long press start: dir=%d pos=%.4f speed=%.4f",
    direction,
    joint_position_.load(),
    sliderSpeedToMps(speed_slider_->value()));
  if (manual_jog_pub_ != nullptr) {
    std_msgs::msg::Float64 msg;
    msg.data = static_cast<double>(direction) * sliderSpeedToMps(speed_slider_->value());
    manual_jog_pub_->publish(msg);
    detail_label_->setText(direction > 0 ? QStringLiteral("长按上移中") : QStringLiteral("长按下移中"));
  } else {
    detail_label_->setText(QStringLiteral("手动长按控制话题未就绪"));
  }
  if (repeat_move_timer_ != nullptr) {
    repeat_move_timer_->setInterval(100);
    repeat_move_timer_->start(100);
  }
}

void LiftPanel::onManualMoveReleased()
{
  manual_motion_epoch_++;
  active_manual_motion_epoch_ = 0;
  const bool was_moving = move_up_active_ || move_down_active_;
  const bool was_continuous = (manual_motion_phase_ == ManualMotionPhase::CONTINUOUS);
  move_up_active_ = false;
  move_down_active_ = false;

  // 停止重复移动定时器
  if (repeat_move_timer_ != nullptr) {
    repeat_move_timer_->stop();
  }
  if (long_press_timer_ != nullptr) {
    long_press_timer_->stop();
  }

  if (was_moving) {
    // 只有在进入连续移动模式后，松手时才发送位置保持
    // 快速点击（<100ms）不会进入连续移动模式，允许完成单步移动
    if (was_continuous) {
      if (manual_jog_pub_ != nullptr) {
        std_msgs::msg::Float64 msg;
        msg.data = 0.0;
        RCLCPP_INFO(
          node_->get_logger(),
          "[manual] release(long): jog stop command published, current_pos=%.4f",
          joint_position_.load());
        manual_jog_pub_->publish(msg);
        detail_label_->setText(QStringLiteral("松手即停"));
      } else {
        detail_label_->setText(QStringLiteral("手动停止话题未就绪"));
      }
    } else {
      if (pending_manual_direction_ != 0 && manual_step_pub_ != nullptr) {
        std_msgs::msg::Float64MultiArray msg;
        msg.data.push_back(static_cast<double>(pending_manual_direction_) *
          sliderStepToMeters(step_slider_->value()));
        msg.data.push_back(sliderSpeedToMps(speed_slider_->value()));
        RCLCPP_INFO(
          node_->get_logger(),
          "[manual] release(short): step request dir=%d step=%.4f speed=%.4f current_pos=%.4f",
          pending_manual_direction_,
          sliderStepToMeters(step_slider_->value()),
          sliderSpeedToMps(speed_slider_->value()),
          joint_position_.load());
        manual_step_pub_->publish(msg);
        detail_label_->setText(QStringLiteral("手动运动已停止（完成当前步长）"));
      } else if (pending_manual_direction_ != 0) {
        detail_label_->setText(QStringLiteral("手动步进话题未就绪"));
      } else {
        detail_label_->setText(QStringLiteral("手动运动已停止"));
      }
    }

    // 重置连续移动标志
    manual_motion_phase_ = ManualMotionPhase::IDLE;
    pending_manual_direction_ = 0;
    last_motion_update_time_ = std::chrono::steady_clock::now();
  }
}

void LiftPanel::onStopClicked()
{
  stopMotion();
  if (manual_jog_pub_ != nullptr) {
    std_msgs::msg::Float64 msg;
    msg.data = 0.0;
    manual_jog_pub_->publish(msg);
  }
  detail_label_->setText(QStringLiteral("运动已立即停止"));
}

void LiftPanel::onStopHomingClicked()
{
  const bool was_homing = driver_homing_active_;
  stopMotion();

  if (was_homing && quick_stop_client_ != nullptr &&
      quick_stop_client_->wait_for_service(std::chrono::milliseconds(200)))
  {
    homing_stop_reenable_pending_ = true;
    detail_label_->setText(QStringLiteral("正在停止当前操作..."));
    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    quick_stop_client_->async_send_request(
      request,
      [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture) {
        // Re-enable drive after quick_stop so it's not left disabled
        postToUi([this]() {
          if (enable_client_ != nullptr &&
              enable_client_->wait_for_service(std::chrono::milliseconds(300)))
          {
            auto enable_request = std::make_shared<std_srvs::srv::Trigger::Request>();
            enable_client_->async_send_request(
              enable_request,
              [this](rclcpp::Client<std_srvs::srv::Trigger>::SharedFuture future) {
                try {
                  const auto response = future.get();
                  postToUi([this, ok = response->success]() {
                    homing_stop_reenable_pending_ = false;
                    drive_enabled_known_ = true;
                    drive_enabled_ = ok;
                    detail_label_->setText(
                      ok
                        ? QStringLiteral("当前动作已停止，驱动已重新使能")
                        : QStringLiteral("动作已停止，重新使能失败"));
                  });
                } catch (const std::exception &) {
                  postToUi([this]() {
                    homing_stop_reenable_pending_ = false;
                    detail_label_->setText(QStringLiteral("动作已停止，重新使能异常"));
                  });
                }
              });
          } else {
            homing_stop_reenable_pending_ = false;
            detail_label_->setText(QStringLiteral("动作已停止，使能服务不可用"));
          }
        });
      });
    return;
  }

  detail_label_->setText(QStringLiteral("运动已停止"));
}

void LiftPanel::onGearButtonClicked()
{
  auto * dialog = new QDialog(this);
  dialog->setWindowTitle(QStringLiteral("运动参数"));
  dialog->setFixedWidth(320);
  dialog->setStyleSheet(R"(
    QDialog { background: #FAFAFA; }
    QLabel { color: #333; font-size: 12px; }
    QSlider::groove:horizontal { border:none; height:6px; background:#E0E0E0; border-radius:3px; }
    QSlider::handle:horizontal { background:#5B5B5B; width:16px; margin:-5px 0; border-radius:8px; border:2px solid #404040; }
    QSlider::sub-page:horizontal { background:#2563EB; border-radius:3px; }
  )");

  auto * layout = new QVBoxLayout(dialog);
  layout->setSpacing(10);
  layout->setContentsMargins(16, 16, 16, 16);

  // Speed
  auto * speed_header = new QHBoxLayout();
  auto * speed_title = new QLabel(QStringLiteral("移动速度"));
  speed_title->setStyleSheet("color:#666;");
  auto * speed_val = new QLabel(
    QString::number(sliderSpeedToMps(speed_slider_->value()), 'f', 3) + QStringLiteral(" m/s"));
  speed_val->setStyleSheet("font-weight:bold;color:#333;");
  speed_header->addWidget(speed_title);
  speed_header->addStretch();
  speed_header->addWidget(speed_val);
  layout->addLayout(speed_header);

  auto * d_speed = new QSlider(Qt::Horizontal);
  d_speed->setRange(10, 100);
  d_speed->setValue(speed_slider_->value());
  layout->addWidget(d_speed);

  // Separator
  auto * dlg_sep = new QFrame();
  dlg_sep->setFrameShape(QFrame::HLine);
  dlg_sep->setStyleSheet("color:#E0E0E0;");
  dlg_sep->setFixedHeight(1);
  layout->addWidget(dlg_sep);

  // Step Size
  auto * step_header = new QHBoxLayout();
  auto * step_title = new QLabel(QStringLiteral("移动步长"));
  step_title->setStyleSheet("color:#666;");
  auto * step_val = new QLabel(
    QString::number(sliderStepToMeters(step_slider_->value()), 'f', 3) + QStringLiteral(" m"));
  step_val->setStyleSheet("font-weight:bold;color:#333;");
  step_header->addWidget(step_title);
  step_header->addStretch();
  step_header->addWidget(step_val);
  layout->addLayout(step_header);

  auto * d_step = new QSlider(Qt::Horizontal);
  d_step->setRange(5, 100);
  d_step->setValue(step_slider_->value());
  layout->addWidget(d_step);

  // Live label updates
  connect(d_speed, &QSlider::valueChanged, this, [this, speed_val](int v) {
    speed_val->setText(QString::number(sliderSpeedToMps(v), 'f', 3) + QStringLiteral(" m/s"));
  });
  connect(d_step, &QSlider::valueChanged, this, [this, step_val](int v) {
    step_val->setText(QString::number(sliderStepToMeters(v), 'f', 3) + QStringLiteral(" m"));
  });

  // Apply button
  auto * apply_btn = new QPushButton(QStringLiteral("应用并关闭"));
  apply_btn->setStyleSheet(
    "QPushButton{background:#2563EB;color:white;font-weight:bold;border:none;"
    "border-radius:6px;padding:10px 20px;font-size:13px;}"
    "QPushButton:hover{background:#1D4ED8;}");
  layout->addWidget(apply_btn);

  connect(apply_btn, &QPushButton::clicked, this,
    [this, dialog, d_speed, d_step]() {
      speed_slider_->setValue(d_speed->value());
      step_slider_->setValue(d_step->value());
      updateMotionValueLabels();
      dialog->accept();
    });

  dialog->exec();
  dialog->deleteLater();
}

void LiftPanel::onSpeedSliderChanged(int)
{
  updateMotionValueLabels();
}

void LiftPanel::publishPosition(double target_position)
{
  if (position_cmd_pub_ == nullptr) {
    return;
  }

  // 先发布速度命令
  const double speed = sliderSpeedToMps(speed_slider_->value());
  if (speed_cmd_pub_ != nullptr) {
    std_msgs::msg::Float64 speed_msg;
    speed_msg.data = speed;
    speed_cmd_pub_->publish(speed_msg);
  }

  // 然后发布位置命令
  std_msgs::msg::Float64MultiArray msg;
  msg.data.push_back(target_position);
  position_cmd_pub_->publish(msg);

  RCLCPP_INFO(
    node_->get_logger(),
    "发布位置命令: %.4f m (速度: %.3f m/s)",
    target_position,
    speed);
}

void LiftPanel::stopMotion()
{
  resetManualMotionState();
}

bool LiftPanel::isCommunicationOk() const
{
  if (!joint_valid_.load()) {
    return false;
  }

  return std::chrono::duration<double>(
    std::chrono::steady_clock::now() - last_joint_update_time_).count() < 1.0;
}

bool LiftPanel::canOperate() const
{
  return isCommunicationOk() && drive_enabled_known_ && drive_enabled_ && !driver_homing_active_;
}

void LiftPanel::updateButtons()
{
  bool limit_pot = false;
  bool limit_not = false;
  {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    limit_pot = limit_pot_;
    limit_not = limit_not_;
  }

  const bool comm_ok = isCommunicationOk();
  const bool drive_ready = drive_enabled_known_ && drive_enabled_;
  const bool manual_allowed = canOperate();
  const bool limits_online = limit_switch_online_.load();

  enable_button_->setEnabled(comm_ok && !driver_homing_active_ && !drive_ready);
  disable_button_->setEnabled(comm_ok && (drive_ready || driver_homing_active_));
  reset_button_->setEnabled(true);
  set_zero_button_->setEnabled(comm_ok && drive_ready && !driver_homing_active_);
  return_home_button_->setEnabled(comm_ok && drive_ready && !driver_homing_active_ && driver_homed_);
  up_button_->setEnabled(manual_allowed && (!limits_online || !limit_not));
  down_button_->setEnabled(manual_allowed && (!limits_online || !limit_pot));

  if (stop_homing_button_ != nullptr) {
    stop_homing_button_->setEnabled(driver_homing_active_);
  }
  if (estop_button_ != nullptr) {
    estop_button_->setEnabled(comm_ok && (drive_ready || driver_homing_active_));
  }
}

void LiftPanel::updateMotionValueLabels()
{
  const double speed = sliderSpeedToMps(speed_slider_->value());

  if (speed_value_label_ != nullptr) {
    speed_value_label_->setText(QString::number(speed, 'f', 3) + QStringLiteral(" m/s"));
  }

  if (joint_valid_.load()) {
    const double pos = joint_position_.load();
    const double vel = joint_velocity_.load();
    if (position_display_label_ != nullptr) {
      position_display_label_->setText(
        QStringLiteral("%1 m").arg(pos, 0, 'f', 3));
    }
    if (speed_display_label_ != nullptr) {
      speed_display_label_->setText(
        QStringLiteral("速度 %1 m/s").arg(vel, 0, 'f', 3));
    }
    if (position_bar_ != nullptr) {
      position_bar_->setPosition(pos);
    }
  } else {
    if (position_display_label_ != nullptr) {
      position_display_label_->setText(QStringLiteral("-- m"));
    }
    if (speed_display_label_ != nullptr) {
      speed_display_label_->setText(QStringLiteral("速度 -- m/s"));
    }
    if (position_bar_ != nullptr) {
      position_bar_->setPosition(0.0);
    }
  }
}

void LiftPanel::updateStatusSummary()
{
  const auto now = std::chrono::steady_clock::now();
  const double since_update =
    std::chrono::duration<double>(now - last_joint_update_time_).count();
  const bool communication_ok = joint_valid_.load() && since_update < 1.0;

  if (comm_status_indicator_ != nullptr) {
    comm_status_indicator_->setText(
      communication_ok ? QStringLiteral("通信正常") : QStringLiteral("等待反馈"));
    comm_status_indicator_->setStyleSheet(
      communication_ok
        ? "font-size:11px;font-weight:bold;color:#16A34A;"
        : "font-size:11px;font-weight:bold;color:#DC2626;");
  }

  const QString homed_text =
    driver_homed_ ? QStringLiteral("已设置") : QStringLiteral("未设置");
  if (zero_state_label_ != nullptr) {
    zero_state_label_->setText(QStringLiteral("零点:%1").arg(homed_text));
    zero_state_label_->setStyleSheet(
      driver_homed_
        ? "font-size:11px;font-weight:bold;color:#16A34A;"
        : "font-size:11px;font-weight:bold;color:#F59E0B;");
  }

  if (drive_state_label_ != nullptr) {
    QString drive_text = QStringLiteral("未使能");
    QString color = QStringLiteral("#888");
    if (driver_homing_active_) {
      drive_text = QStringLiteral("寻零中");
      color = QStringLiteral("#2563EB");
    } else if (drive_enabled_known_ && drive_enabled_) {
      drive_text = QStringLiteral("已使能");
      color = QStringLiteral("#16A34A");
    } else if (drive_enabled_known_) {
      drive_text = QStringLiteral("已掉电");
      color = QStringLiteral("#F59E0B");
    }
    drive_state_label_->setText(drive_text);
    drive_state_label_->setStyleSheet(
      QStringLiteral("font-weight:bold;font-size:12px;color:%1;").arg(color));
  }

  if (drive_status_indicator_ != nullptr) {
    const QString enable_text =
      !drive_enabled_known_ ? QStringLiteral("驱动未确认") :
      (drive_enabled_ ? QStringLiteral("已使能") : QStringLiteral("已失能"));
    drive_status_indicator_->setText(enable_text);
    const bool ok = drive_enabled_known_ && drive_enabled_;
    drive_status_indicator_->setStyleSheet(
      ok
        ? "font-size:11px;font-weight:bold;color:#16A34A;"
        : "font-size:11px;font-weight:bold;color:#F59E0B;");
  }
}

void LiftPanel::updateLimitLamps()
{
  bool limit_home = false;
  bool limit_pot = false;
  bool limit_not = false;
  const bool online = limit_switch_online_.load();
  {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    limit_home = limit_home_;
    limit_pot = limit_pot_;
    limit_not = limit_not_;
  }

  const bool upper_blocked = online && limit_not;
  const bool home_blocked = online && limit_home;
  const bool lower_blocked = online && limit_pot;

  setLampState(upper_limit_lamp_, online, upper_blocked);
  setLampState(home_limit_lamp_, online, home_blocked);
  setLampState(lower_limit_lamp_, online, lower_blocked);

  const auto state_text = [](bool is_online, bool blocked) -> QString {
    if (!is_online) {
      return QStringLiteral("离线");
    }
    return blocked ? QStringLiteral("遮挡(绿)") : QStringLiteral("通光(红)");
  };

  upper_limit_label_->setText(
    QStringLiteral("上限位(%1)").arg(state_text(online, upper_blocked)));
  home_limit_label_->setText(
    QStringLiteral("零点(%1)").arg(state_text(online, home_blocked)));
  lower_limit_label_->setText(
    QStringLiteral("下限位(%1)").arg(state_text(online, lower_blocked)));
}

void LiftPanel::setLampState(QFrame * lamp, bool online, bool blocked) const
{
  if (lamp == nullptr) {
    return;
  }

  if (!online) {
    // Grey: offline / not yet receiving data
    lamp->setStyleSheet(
      QStringLiteral("background:#9CA3AF;border-radius:9px;border:1px solid #1F2937;"));
    return;
  }

  // RED = sensor unblocked (normal/idle), GREEN = sensor blocked (slide touching limit)
  lamp->setStyleSheet(
    blocked ?
    QStringLiteral("background:#16A34A;border-radius:9px;border:1px solid #14532D;") :
    QStringLiteral("background:#DC2626;border-radius:9px;border:1px solid #7F1D1D;"));
}

void LiftPanel::onRepeatMove()
{
  // 检查是否还在按下状态
  if (!move_up_active_ && !move_down_active_) {
    if (repeat_move_timer_ != nullptr) {
      repeat_move_timer_->stop();
    }
    return;
  }
  if (active_manual_motion_epoch_ != manual_motion_epoch_) {
    if (repeat_move_timer_ != nullptr) {
      repeat_move_timer_->stop();
    }
    return;
  }

  if (manual_motion_phase_ != ManualMotionPhase::CONTINUOUS) {
    return;
  }
  if (active_manual_motion_epoch_ != manual_motion_epoch_) {
    return;
  }

  // 检查限位
  bool upper_blocked = false;
  bool lower_blocked = false;
  {
    std::lock_guard<std::mutex> lock(limit_switch_mutex_);
    upper_blocked = limit_not_;
    lower_blocked = limit_pot_;
  }

  RCLCPP_INFO_THROTTLE(
    node_->get_logger(),
    *node_->get_clock(),
    500,
    "[manual] repeat monitor: dir=%s cur=%.4f",
    move_up_active_ ? "up" : "down",
    joint_position_.load());

  if (move_up_active_) {
    if (limit_switch_online_.load() && upper_blocked) {
      if (repeat_move_timer_ != nullptr) {
        repeat_move_timer_->stop();
      }
      if (manual_jog_pub_ != nullptr) {
        std_msgs::msg::Float64 msg;
        msg.data = 0.0;
        manual_jog_pub_->publish(msg);
      }
      move_up_active_ = false;
      move_down_active_ = false;
      manual_motion_phase_ = ManualMotionPhase::IDLE;
      active_manual_motion_epoch_ = 0;
      pending_manual_direction_ = 0;
      detail_label_->setText(QStringLiteral("上限位已遮挡，停止上移"));
      return;
    }
  } else if (move_down_active_) {
    if (limit_switch_online_.load() && lower_blocked) {
      if (repeat_move_timer_ != nullptr) {
        repeat_move_timer_->stop();
      }
      if (manual_jog_pub_ != nullptr) {
        std_msgs::msg::Float64 msg;
        msg.data = 0.0;
        manual_jog_pub_->publish(msg);
      }
      move_up_active_ = false;
      move_down_active_ = false;
      manual_motion_phase_ = ManualMotionPhase::IDLE;
      active_manual_motion_epoch_ = 0;
      pending_manual_direction_ = 0;
      detail_label_->setText(QStringLiteral("下限位已遮挡，停止下移"));
      return;
    }
  }
}

void LiftPanel::onUiTimer()
{
  if (node_ != nullptr) {
    rclcpp::spin_some(node_);
  }

  updateMotionValueLabels();
  updateStatusSummary();
  updateLimitLamps();
  updateButtons();
}

void LiftPanel::save(rviz_common::Config config) const
{
  Panel::save(config);
  config.mapSetValue("speed_slider", speed_slider_->value());
  config.mapSetValue("step_slider", step_slider_->value());
}

void LiftPanel::load(const rviz_common::Config & config)
{
  Panel::load(config);

  int int_value = 0;
  if (config.mapGetInt("speed_slider", &int_value)) {
    speed_slider_->setValue(int_value);
  }
  if (config.mapGetInt("step_slider", &int_value)) {
    step_slider_->setValue(int_value);
  }
  updateMotionValueLabels();
}

double LiftPanel::clamp(double value, double lower, double upper) const
{
  return std::max(lower, std::min(upper, value));
}

double LiftPanel::sliderSpeedToMps(int slider_value) const
{
  // 滑块范围 10-100 → 速度 0.01-0.10 m/s
  return clamp(static_cast<double>(slider_value) / 1000.0, 0.010, 0.100);
}

double LiftPanel::sliderStepToMeters(int slider_value) const
{
  // 滑块范围 5-100 → 步长 0.005-0.100 m
  return clamp(static_cast<double>(slider_value) / 1000.0, 0.005, 0.100);
}

}  // namespace lift_slide_panel

PLUGINLIB_EXPORT_CLASS(lift_slide_panel::LiftPanel, rviz_common::Panel)
