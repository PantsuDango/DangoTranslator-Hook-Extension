#pragma once
class CSystem : public ENGINE
{
public:
    CSystem() { check_by = CHECK_BY::FILE_ALL; check_by_target = check_by_list{L"Arc00.dat", L"Arc01.dat", L"Arc04.dat"}; }
    bool attach_function();
};
