#include "python/python.h"

class Renpy : public ENGINE
{
public:
    Renpy()
    {
        check_by = CHECK_BY::CUSTOM;
        check_by_target = [this]()
        { return attach_function_(); };
    }

    bool attach_function()
    {
        return true;
    }

    bool attach_function_()
    {
#ifdef _WIN64
        return InsertRenpy3Hook();
#else
        return false;
#endif
    }
};
