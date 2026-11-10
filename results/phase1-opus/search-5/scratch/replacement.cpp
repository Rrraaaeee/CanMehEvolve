const Profile::MESG* Profile::GetMesg(const FIT_UINT16 num)
{
    struct Index
    {
        FIT_UINT16 idx[1024];
        Index()
        {
            for (int k = 0; k < 1024; k++)
                idx[k] = 0xFFFF;
            for (int i = MESGS - 1; i >= 0; i--)
            {
                if (mesgs[i].num < 1024)
                    idx[mesgs[i].num] = (FIT_UINT16)i;
            }
        }
    };
    static const Index index;

    if (num < 1024)
    {
        const FIT_UINT16 i = index.idx[num];
        if (i == 0xFFFF)
            return NULL;
        return &mesgs[i];
    }

    for (int i = 0; i < MESGS; i++)
    {
        if (mesgs[i].num == num)
            return &mesgs[i];
    }
    return NULL;
}
