#pragma once

class DDWorks : public ENGINE
{
public:
    DDWorks()
    {
        check_by = CHECK_BY::FILE_ALL;
        check_by_target = check_by_list{L"bootup.dat", L"Data/arc_text.dat"};
    }
    bool attach_function();
};
