#pragma once

class NScripter : public ENGINE
{
public:
    NScripter() { check_by = CHECK_BY::CUSTOM; check_by_target = []() { return true; }; }
    bool attach_function();
};
