// main.cpp
// 应用入口与 Qt 控件编排。播放、解码、状态和 UI 计算已拆到独立模块。

#include "ControlClient.h"
#include "FFmpegD3D11Player.h"
#include "PlayerStatus.h"
#include "PlayerUi.h"
#include "SrtUrl.h"

#include <QApplication>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <windows.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    qRegisterMetaType<PlayerStatus>("PlayerStatus");

    if (argc < 2) {
        std::cout << "用法: player.exe <srt_url>\n";
        return 0;
    }

    const std::string url            = argv[1];
    const std::string push_ctrl_host = extract_host_from_srt_url(url);

    QWidget surface;
    surface.setAttribute(Qt::WA_NativeWindow);
    surface.setAttribute(Qt::WA_PaintOnScreen, true);
    surface.setAttribute(Qt::WA_NoSystemBackground, true);
    surface.setAutoFillBackground(false);
    surface.setMinimumSize(640, 360);

    QWidget container;
    container.setStyleSheet("background:#111;");
    QVBoxLayout* mainLayout = new QVBoxLayout(&container);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(&surface, 1);

    QWidget* statusBar = new QWidget;
    statusBar->setFixedHeight(28);
    statusBar->setStyleSheet("background:#1a1a1a;");

    QHBoxLayout* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(10, 0, 10, 0);
    statusLayout->setSpacing(8);

    QLabel* dotLbl   = new QLabel;
    dotLbl->setFixedSize(10, 10);
    dotLbl->setStyleSheet("background:#888780; border-radius:5px;");

    QLabel* stateLbl = new QLabel("空闲");
    stateLbl->setStyleSheet("color:#b4b2a9; font-size:12px; font-weight:500;");

    QLabel* msgLbl = new QLabel;
    msgLbl->setStyleSheet("color:#5f5e5a; font-size:11px;");
    msgLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    msgLbl->setMaximumWidth(600);

    QLabel* audioLbl = new QLabel;
    audioLbl->setStyleSheet("color:#5f5e5a; font-size:11px;");

    QLabel* reconLbl = new QLabel;
    reconLbl->setStyleSheet("color:#ef9f27; font-size:11px;");
    reconLbl->hide();

    statusLayout->addWidget(dotLbl);
    statusLayout->addWidget(stateLbl);
    statusLayout->addWidget(msgLbl);
    statusLayout->addStretch();
    statusLayout->addWidget(audioLbl);
    statusLayout->addWidget(reconLbl);

    mainLayout->addWidget(statusBar, 0);

    QWidget* ctrlBar = new QWidget;
    ctrlBar->setFixedHeight(38);
    ctrlBar->setStyleSheet("background:#161616;");

    QHBoxLayout* ctrlLayout = new QHBoxLayout(ctrlBar);
    ctrlLayout->setContentsMargins(10, 5, 10, 5);
    ctrlLayout->setSpacing(6);

    QLabel* stateBadge = new QLabel("空闲");
    stateBadge->setFixedHeight(24);
    stateBadge->setContentsMargins(8, 0, 8, 0);
    stateBadge->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    stateBadge->setStyleSheet(
        "border-radius:4px; font-size:12px; font-weight:500;"
        "background:#2c2c2a; color:#888780; padding:0 8px;");

    auto makeBtn = [](const QString& text) -> QPushButton* {
        auto* b = new QPushButton(text);
        b->setFixedHeight(26);
        b->setStyleSheet(
            "QPushButton {"
            "  padding:0 10px; font-size:12px;"
            "  border:0.5px solid rgba(255,255,255,0.14);"
            "  border-radius:4px;"
            "  background:rgba(255,255,255,0.06);"
            "  color:#c2c0b6;"
            "}"
            "QPushButton:hover  { background:rgba(255,255,255,0.11); }"
            "QPushButton:pressed{ background:rgba(255,255,255,0.18); }");
        return b;
    };

    QPushButton* pauseBtn  = makeBtn("暂停推流");
    QPushButton* recordBtn = makeBtn("开始录制");

    ctrlLayout->addWidget(stateBadge);
    ctrlLayout->addSpacing(6);
    ctrlLayout->addWidget(pauseBtn);
    ctrlLayout->addWidget(recordBtn);
    ctrlLayout->addStretch();

    mainLayout->addWidget(ctrlBar, 0);

    HWND hwnd = reinterpret_cast<HWND>(surface.winId());
    ControlClient ctrl(push_ctrl_host, 10090);
    FFmpegD3D11Player player(hwnd, url);
    

    QObject::connect(&player, &FFmpegD3D11Player::statusChanged,
                     [&](const PlayerStatus& s) {

        dotLbl->setStyleSheet(
            QString("background:%1; border-radius:5px;").arg(dotColor(s)));

        stateLbl->setText(stateText(s));
        msgLbl->setText(s.message);

        if (s.reconnect_count > 0 && s.state == PlayerState::Reconnecting) {
            reconLbl->setText(QString("第%1次").arg(s.reconnect_count));
            reconLbl->show();
        } else {
            reconLbl->hide();
        }

        if (s.audio_available) {
            audioLbl->setText("有声");
            audioLbl->setStyleSheet("color:#1d9e75; font-size:11px;");
        } else {
            audioLbl->setText("无声");
            audioLbl->setStyleSheet("color:#5f5e5a; font-size:11px;");
        }

        auto [bg, fg] = stateBadgeColors(s);
        stateBadge->setText(stateText(s));
        stateBadge->setStyleSheet(
            QString("border-radius:4px; font-size:12px; font-weight:500;"
                    "background:%1; color:%2; padding:0 8px;").arg(bg).arg(fg));

        if (s.remote_paused) {
            pauseBtn->setText("恢复推流");
            pauseBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid #854f0b;"
                "  border-radius:4px;"
                "  background:#412402; color:#fac775;"
                "}"
                "QPushButton:hover  { background:#633806; }"
                "QPushButton:pressed{ background:#854f0b; }");
        } else {
            pauseBtn->setText("暂停推流");
            pauseBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid rgba(255,255,255,0.14);"
                "  border-radius:4px;"
                "  background:rgba(255,255,255,0.06); color:#c2c0b6;"
                "}"
                "QPushButton:hover  { background:rgba(255,255,255,0.11); }"
                "QPushButton:pressed{ background:rgba(255,255,255,0.18); }");
        }

        if (recordBtnShowStop(s)) {
            recordBtn->setText("停止录制");
            recordBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid #a32d2d;"
                "  border-radius:4px;"
                "  background:#501313; color:#f09595;"
                "}"
                "QPushButton:hover  { background:#791f1f; }"
                "QPushButton:pressed{ background:#a32d2d; }");
        } else {
            recordBtn->setText("开始录制");
            recordBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid rgba(255,255,255,0.14);"
                "  border-radius:4px;"
                "  background:rgba(255,255,255,0.06); color:#c2c0b6;"
                "}"
                "QPushButton:hover  { background:rgba(255,255,255,0.11); }"
                "QPushButton:pressed{ background:rgba(255,255,255,0.18); }");
        }
    });

    QObject::connect(pauseBtn, &QPushButton::clicked, [&] {
        const bool currently_paused = player.currentStatus().remote_paused;
        const bool ok = currently_paused ? ctrl.resume() : ctrl.pause();
        if (ok) {
            player.setRemotePaused(!currently_paused);
        } else {
            msgLbl->setText(currently_paused ? "恢复指令发送失败" : "暂停指令发送失败");
        }
    });

    QObject::connect(recordBtn, &QPushButton::clicked, [&] {
        const RecordPhase phase = player.currentStatus().record_phase;
        if (phase == RecordPhase::Idle || phase == RecordPhase::StopRequested) {
            player.requestStartRecord("C:/Users/dkw/Desktop/record.ts");
        } else {
            player.requestStopRecord();
        }
    });

    container.resize(1280, 800);
    container.show();

    if (!player.start()) {
        std::cerr << "player.start() failed\n";
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&player] { player.stop(); });

    int code = app.exec();
    player.stop();
    
    return code;
}
