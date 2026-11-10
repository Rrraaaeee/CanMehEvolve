const Profile::MESG* Profile::GetMesg(const FIT_UINT16 num)
{
    enum { SMALL = 1024 };
    typedef std::conditional<(MESGS < 0xFF), FIT_UINT8, FIT_UINT16>::type IdxT;
    static constexpr IdxT NONE = (IdxT)~(IdxT)0;

    struct Index
    {
        IdxT idx[SMALL];
        FIT_UINT16 maxNum;
        Index()
        {
            for (int k = 0; k < SMALL; k++)
                idx[k] = NONE;
            maxNum = 0;
            for (int i = MESGS - 1; i >= 0; i--)
            {
                const FIT_UINT16 n = mesgs[i].num;
                if (n < SMALL)
                    idx[n] = (IdxT)i;
                if (n > maxNum)
                    maxNum = n;
            }
        }
    };

    if (MESGS < 0xFFFF)
    {
        static const Index table;
        if (num < SMALL)
        {
            const IdxT i = table.idx[num];
            if (i == NONE)
                return NULL;
            return &mesgs[i];
        }
        if (num > table.maxNum)
            return NULL;
    }

    for (int i = 0; i < MESGS; i++)
    {
        if (mesgs[i].num == num)
            return &mesgs[i];
    }

    return NULL;
}
