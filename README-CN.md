# lift_slide_panel

[English](./README.md) | 中文

---

![封面](./image/cover.gif)


用于手动控制升降滑台的 RViz2 面板插件（Qt5）。

**插件名：** `lift_slide_panel/LiftPanel`
**基类：** `rviz_common::Panel`

## 功能

- 实时位置显示，带图形化进度条（以零点为中心，正方向蓝色，负方向橙色）
- 驱动器使能/失能控制
- 回零：设定零点和返回原点
- 手动移动：上/下按钮支持短按（步进）和长按（点动）
- 速度滑块调节运行速度
- 步长滑块调节位置步进量
- 限位开关状态指示灯（上限位/下限位/原点）
- 急停按钮
- 通信和驱动状态指示

## 订阅的话题

| 话题 | 类型 | 说明 |
|------|------|------|
| `/lift_slide_driver/motor_status` | `lift_slide_msgs/MotorStatus` | 综合电机状态 |
| `/lift_slide_driver/homing_state` | `std_msgs/String` | 回零状态变化 |
| `/lift_slide_driver/limit_switch_state` | `std_msgs/String` | 限位开关状态变化 |

## 发布的话题

| 话题 | 类型 | 说明 |
|------|------|------|
| `/lift_position_controller/commands` | `std_msgs/Float64MultiArray` | 位置指令 |
| `/lift_state_controller/profile_speed_cmd` | `std_msgs/Float64` | 运行速度设置 |
| `/lift_manual_position_controller/step_command` | `std_msgs/Float64MultiArray` | 步进指令 [方向, 步长m, 速度m/s] |
| `/lift_manual_position_controller/jog_command` | `std_msgs/Float64` | 点动指令 |

## 调用的服务

| 服务 | 类型 | 说明 |
|------|------|------|
| `/lift_slide_driver/start_homing` | `std_srvs/Trigger` | 启动回零序列 |
| `/lift_slide_driver/return_home` | `std_srvs/Trigger` | 返回原点位置 |
| `/lift_slide_driver/enable` | `std_srvs/Trigger` | 使能驱动器 |
| `/lift_slide_driver/quick_stop` | `std_srvs/Trigger` | 紧急快速停止 |

## 使用方法

使用 bringup 启动文件时，面板会自动加载到 RViz 中。也可以在 RViz 中手动添加：Panels -> Add New Panel -> lift_slide_panel/LiftPanel。

## 编译

```bash
colcon build --packages-select lift_slide_panel
source install/setup.bash
```

## 前置依赖

- ROS 2（Humble/Iron）
- rviz_common
- Qt5
- pluginlib
- lift_slide_msgs
- std_msgs、std_srvs

## 许可证

本包通过 知识共享 署名-非商业性使用-相同方式共享 4.0 国际许可协议 (CC BY-NC-SA 4.0) 进行许可。

版权所有 (c) 2026 成都长数机器人有限公司 (Chengdu Changshu Robot Co., Ltd.)

详情请参阅 [LICENSE](LICENSE) 文件或访问：http://creativecommons.org/licenses/by-nc-sa/4.0/

## 致谢

本包是 OpenFlex 全身人形机器人平台生态系统的一部分，专为人形机器人领域的研究和工业应用而开发。

---

## 📞 联系我们

### 成都长数机器人有限公司
**Chengdu Changshu Robotics Co., Ltd.**

| 联系方式 | 信息 |
|---------|------|
| 📧 邮箱 | openarmrobot@gmail.com |
| 📱 电话/微信 | +86-17746530375 |
| 🌐 官网 | https://openarmx.com/ |
| 🌐 文档 | http://docs.openarmx.com/ |
| 📍 地址 | 天津市西青区・稻潮机器人体验基地（明日之城）・天津市人形机器人中心 |
| 👤 联系人 | 王先生 |
