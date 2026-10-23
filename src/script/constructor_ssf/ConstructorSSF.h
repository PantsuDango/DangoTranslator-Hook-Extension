#pragma once

class ConstructorSSF : public ENGINE
{
public:
    // ConstructorSSF 仅在专用组件中启用独立语义布局验证
    ConstructorSSF() { check_by = CHECK_BY::CUSTOM; check_by_target = []() { return true; }; }
    bool attach_function();
};
