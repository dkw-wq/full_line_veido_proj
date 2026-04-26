#pragma once

#include "PlayerStatus.h"

#include <QString>

#include <utility>

static PlayerState effectiveDisplayState(const PlayerStatus& s) {
    if (s.remote_paused) return PlayerState::Idle; // 用一个不存在的枚举占位
    return s.state;
}

static QString stateText(const PlayerStatus& s) {
    if (s.remote_paused) return "远端已暂停";
    switch (s.state) {
        case PlayerState::Idle:            return "空闲";
        case PlayerState::Starting:        return "启动中";
        case PlayerState::OpeningInput:    return "连接输入...";
        case PlayerState::ProbingStreams:  return "探测流...";
        case PlayerState::Initializing:    return "初始化解码器...";
        case PlayerState::WaitingKeyframe: return "等待关键帧...";
        case PlayerState::Playing:         return "播放中";
        case PlayerState::Reconnecting:    return "重连中";
        case PlayerState::Stopping:        return "停止中";
        case PlayerState::Stopped:         return "已停止";
        case PlayerState::Error:           return "错误";
    }
    return {};
}

static QString dotColor(const PlayerStatus& s) {
    if (s.remote_paused) return "#ef9f27";  // 橙
    switch (s.state) {
        case PlayerState::Playing:         return "#1d9e75";  // 绿
        case PlayerState::Error:           return "#e24b4a";  // 红
        case PlayerState::Reconnecting:
        case PlayerState::OpeningInput:
        case PlayerState::ProbingStreams:
        case PlayerState::Initializing:
        case PlayerState::WaitingKeyframe:
        case PlayerState::Starting:        return "#378add";  // 蓝（过渡态）
        default:                           return "#888780";  // 灰
    }
}

static std::pair<QString,QString> stateBadgeColors(const PlayerStatus& s) {
    if (s.remote_paused) return {"#854f0b", "#fac775"};
    switch (s.state) {
        case PlayerState::Playing:      return {"#0f6e56", "#9fe1cb"};
        case PlayerState::Error:        return {"#a32d2d", "#f09595"};
        case PlayerState::Reconnecting: return {"#185fa5", "#b5d4f4"};
        default:                        return {"#2c2c2a", "#888780"};
    }
}

// 录制按钮：用 RecordPhase 统一决定显示，不读 recorder_.isRecording()
// Requested / Active → 都显示"停止录制"（用户已有意图，立即响应）
// StopRequested / Idle → 都显示"开始录制"
static bool recordBtnShowStop(const PlayerStatus& s) {
    return s.record_phase == RecordPhase::Requested
        || s.record_phase == RecordPhase::Active;
}