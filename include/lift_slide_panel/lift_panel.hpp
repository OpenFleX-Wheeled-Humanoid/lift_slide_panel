#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <rviz_common/panel.hpp>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/parameter_client.hpp"
#include "lift_slide_msgs/msg/motor_status.hpp"
#include "std_msgs/msg/float64.hpp"
#include "std_msgs/msg/float64_multi_array.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_srvs/srv/trigger.hpp"

namespace lift_slide_panel
{

// 自定义位置条：以零点为中心，正方向蓝色向右，负方向橙色向左
class PositionBarWidget : public QWidget
{
  Q_OBJECT
public:
  explicit PositionBarWidget(QWidget * parent = nullptr)
  : QWidget(parent) { setFixedHeight(20); }

  void setRange(double min_val, double max_val)
  { range_min_ = min_val; range_max_ = max_val; update(); }

  void setPosition(double pos)
  { position_ = pos; update(); }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int w = width();
    const int h = height();
    const double span = range_max_ - range_min_;
    if (span < 1e-9) return;

    // 背景灰条（圆角）
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#E0E0E0"));
    p.drawRoundedRect(0, 0, w, h, 4, 4);

    // 零点 x 坐标
    const double zero_ratio = (0.0 - range_min_) / span;
    const int zero_x = static_cast<int>(zero_ratio * w);

    // 当前位置 x 坐标
    const double clamped = std::max(range_min_, std::min(range_max_, position_));
    const double pos_ratio = (clamped - range_min_) / span;
    const int pos_x = static_cast<int>(pos_ratio * w);

    // 填充色：正方向蓝，负方向橙
    if (pos_x >= zero_x) {
      p.setBrush(QColor("#2563EB"));
      p.drawRoundedRect(zero_x, 0, pos_x - zero_x, h, 2, 2);
    } else {
      p.setBrush(QColor("#F59E0B"));
      p.drawRoundedRect(pos_x, 0, zero_x - pos_x, h, 2, 2);
    }

    // 零点标记线
    p.setPen(QPen(QColor("#1A1A1A"), 2));
    p.drawLine(zero_x, 0, zero_x, h);

    // 零点文字 "0"
    p.setPen(QColor("#666"));
    p.setFont(QFont("sans-serif", 7));
    p.drawText(zero_x - 3, h + 12, "0");

    // 位置数值文字（显示在条内）
    const QString text = QString::number(position_ * 1000.0, 'f', 0) + " mm";
    p.setPen(QColor("#333"));
    p.setFont(QFont("sans-serif", 8, QFont::Bold));
    p.drawText(rect(), Qt::AlignCenter, text);
  }

private:
  double range_min_{-0.750};
  double range_max_{0.400};
  double position_{0.0};
};

enum class HomingAction
{
  NONE = 0,
  SET_ZERO = 1,
  RETURN_HOME = 2,
};

enum class ManualMotionPhase
{
  IDLE = 0,
  SHORT_PENDING = 1,
  CONTINUOUS = 2,
};

class LiftPanel : public rviz_common::Panel
{
  Q_OBJECT

public:
  explicit LiftPanel(QWidget * parent = nullptr);
  ~LiftPanel() override;

  void onInitialize() override;
  void save(rviz_common::Config config) const override;
  void load(const rviz_common::Config & config) override;

private Q_SLOTS:
  void onEnableClicked();
  void onDisableClicked();
  void onSetZeroClicked();
  void onReturnHomeClicked();
  void onResetClicked();
  void onUpPressed();
  void onDownPressed();
  void onManualMoveReleased();
  void onStopClicked();
  void onStopHomingClicked();
  void onSpeedSliderChanged(int value);
  void onGearButtonClicked();
  void onUiTimer();
  void onLongPressTimeout();
  void onRepeatMove();  // 长按状态监控回调

private:
  void setupUi();
  void setupRos();
  void ensureFloatingWindow();
  void postToUi(std::function<void()> fn);
  void resetManualMotionState();
  void requestHoming(HomingAction action);
  void stopMotion();
  bool isCommunicationOk() const;
  bool canOperate() const;
  void updateButtons();
  void updateMotionValueLabels();
  void updateStatusSummary();
  void updateLimitLamps();
  void setLampState(QFrame * lamp, bool online, bool blocked) const;
  double clamp(double value, double lower, double upper) const;
  double sliderSpeedToMps(int slider_value) const;
  double sliderStepToMeters(int slider_value) const;

  void driverHomingStateCallback(const std_msgs::msg::String::SharedPtr msg);
  void driverLimitSwitchStateCallback(const std_msgs::msg::String::SharedPtr msg);
  void motorStatusCallback(const lift_slide_msgs::msg::MotorStatus::SharedPtr msg);

  rclcpp::Node::SharedPtr node_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr driver_homing_state_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr limit_switch_state_sub_;
  rclcpp::Subscription<lift_slide_msgs::msg::MotorStatus>::SharedPtr motor_status_sub_;
  rclcpp::Publisher<std_msgs::msg::Float64>::SharedPtr speed_cmd_pub_;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr manual_step_pub_;
  rclcpp::Publisher<std_msgs::msg::Float64>::SharedPtr manual_jog_pub_;
  rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr start_homing_client_;
  rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr return_home_client_;
  rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr enable_client_;
  rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr quick_stop_client_;

  QTimer * ui_timer_{nullptr};
  QTimer * long_press_timer_{nullptr};  // 用于区分单点与长按
  QTimer * repeat_move_timer_{nullptr};  // 用于长按状态监控
  int pending_manual_direction_{0};

  // 手动运动阶段：等待单点、连续移动、空闲
  ManualMotionPhase manual_motion_phase_{ManualMotionPhase::IDLE};
  uint64_t manual_motion_epoch_{0};
  uint64_t active_manual_motion_epoch_{0};

  // Status indicators (top bar)
  QLabel * comm_status_indicator_{nullptr};
  QLabel * drive_status_indicator_{nullptr};

  // Realtime data
  QLabel * speed_display_label_{nullptr};
  QLabel * position_display_label_{nullptr};
  PositionBarWidget * position_bar_{nullptr};
  QLabel * bar_min_label_{nullptr};
  QLabel * bar_max_label_{nullptr};

  // Limit lamps
  QFrame * upper_limit_lamp_{nullptr};
  QFrame * home_limit_lamp_{nullptr};
  QFrame * lower_limit_lamp_{nullptr};
  QLabel * upper_limit_label_{nullptr};
  QLabel * home_limit_label_{nullptr};
  QLabel * lower_limit_label_{nullptr};

  // Control
  QLabel * drive_state_label_{nullptr};
  QLabel * speed_value_label_{nullptr};
  QLabel * detail_label_{nullptr};

  // Homing
  QLabel * zero_state_label_{nullptr};

  // Buttons
  QPushButton * enable_button_{nullptr};
  QPushButton * disable_button_{nullptr};
  QPushButton * reset_button_{nullptr};
  QPushButton * gear_button_{nullptr};
  QPushButton * up_button_{nullptr};
  QPushButton * down_button_{nullptr};
  QPushButton * set_zero_button_{nullptr};
  QPushButton * return_home_button_{nullptr};
  QPushButton * stop_homing_button_{nullptr};
  QPushButton * estop_button_{nullptr};

  // Sliders (speed and step_size for position control)
  QSlider * speed_slider_{nullptr};
  QSlider * step_slider_{nullptr};
  QLabel * step_value_label_{nullptr};
  QLabel * profile_status_label_{nullptr};

  std::atomic<double> joint_position_{0.0};
  std::atomic<double> joint_velocity_{0.0};
  std::atomic<bool> joint_valid_{false};

  std::string joint_name_{"lift_joint"};
  double display_min_height_{-0.750};
  double display_max_height_{0.400};

  std::atomic<bool> limit_switch_online_{false};
  bool limit_home_{false};
  bool limit_pot_{false};
  bool limit_not_{false};
  uint32_t limit_raw_{0U};
  mutable std::mutex limit_switch_mutex_;

  bool drive_enabled_known_{false};
  bool drive_enabled_{false};
  bool drive_fault_{false};
  bool driver_homing_active_{false};
  bool driver_homed_{false};
  bool motion_ready_{false};
  bool reference_valid_{false};
  bool encoder_reference_lost_{false};
  std::string cia402_state_str_;
  std::string mode_name_str_;
  std::string last_homing_detail_;
  HomingAction pending_homing_action_{HomingAction::NONE};
  bool homing_stop_reenable_pending_{false};

  bool move_up_active_{false};
  bool move_down_active_{false};

  std::chrono::steady_clock::time_point last_joint_update_time_;
  std::chrono::steady_clock::time_point last_motion_update_time_;

  bool floating_window_applied_{false};
  int floating_window_retry_count_{0};
};

}  // namespace lift_slide_panel
