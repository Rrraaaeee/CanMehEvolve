const Profile::MESG* Profile::GetMesg(const FIT_UINT16 num)
{
    struct LookupTable
    {
        FIT_UINT16 idx[65536];
        LookupTable()
        {
            for (int n = 0; n < 65536; n++)
                idx[n] = 0xFFFF;
            for (int i = 0; i < MESGS; i++)
            {
                FIT_UINT16 k = mesgs[i].num;
                if (idx[k] == 0xFFFF)
                    idx[k] = (FIT_UINT16)i;
            }
        }
    };

    static const LookupTable table;

    FIT_UINT16 i = table.idx[num];
    if (i == 0xFFFF)
        return NULL;
    return &mesgs[i];
}
