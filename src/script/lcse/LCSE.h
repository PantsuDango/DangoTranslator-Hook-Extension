#pragma once

#include "LCSEDisplay.h"
#include "LCSEScene.h"
#include "LCSESnapshot.h"
#include "LCSEControl.h"

// LCSEGlyphLayout 保存独立确认的字形入口与模块内GDI导入槽
struct LCSEGlyphLayout
{
    uintptr_t measure = 0, draw = 0, classifier = 0, failure = 0;
    uintptr_t outlineSlot = 0, createFontSlot = 0, selectSlot = 0, deleteSlot = 0;
};

class LCSE : public ENGINE
{
public:
    LCSE()
    {
        check_by = CHECK_BY::CUSTOM;
        check_by_target = []() { return true; };
    }
    bool attach_function();
    bool locateGlyphLayout(LCSEGlyphLayout &result);
    bool locateDisplayLayout(LCSEDisplayLayout &result);
    bool locatePresentationLayout(LCSEPresentationLayout &result);
    // configurePresentationCapture 控制内部捕获及整页呈现，用户注入须另行完成资源和生命周期接入
    static bool configurePresentationCapture(bool enabled, std::shared_ptr<LCSEGlyph::Renderer> renderer = {},
        std::shared_ptr<const LCSEScene::TranslationMap> translations = {}, bool submit = false);
    // configurePresentationResource 预载已验证不可变资源，用户开关与进程就绪握手仍由外层负责
    static bool configurePresentationResource(std::shared_ptr<const LCSESnapshot::Data> resource, bool submit = false);
    // processPresentationControl 仅由进程控制管道加载摘要绑定资源，不开放普通逐句注入
    static std::string processPresentationControl(const LCSEControl::Request &request);
    // resetPresentationControl 管道断开时取消资源，会话重连必须重新请求加载
    static void resetPresentationControl();
    // presentationSnapshot 返回经过真实呈现阶段的当前快照，不提供游戏对象所有权
    static std::shared_ptr<const LCSEScene::Snapshot> presentationSnapshot();
    // presentationSnapshotCurrent 在原绘制线程同时核对场景身份和未失效的当前代次
    static bool presentationSnapshotCurrent(const std::shared_ptr<const LCSEScene::Snapshot> &snapshot);
    // presentationTranslation 返回当前原帧生成的独立译文，不直接替换游戏画面
    static std::shared_ptr<const LCSEScene::TranslatedFrame> presentationTranslation();
};
