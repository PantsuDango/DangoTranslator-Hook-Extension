#pragma once

class DxCompiled : public ENGINE
{
public:
    DxCompiled() { check_by = CHECK_BY::CUSTOM; check_by_target = []() { return true; }; }
    bool attach_function();
};
