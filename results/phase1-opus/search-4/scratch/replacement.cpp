const Profile::MESG* Profile::GetMesg(const FIT_UINT16 num)
{
    struct Table
    {
        FIT_UINT16 idx[65536];
        Table()
        {
            for (int k = 0; k < 65536; k++)
                idx[k] = 0xFFFF;
            for (int i = 0; i < MESGS; i++)
            {
                FIT_UINT16 n = mesgs[i].num;
                if (idx[n] == 0xFFFF)
                    idx[n] = (FIT_UINT16)i;
            }
        }
    };
    static const Table table;

    FIT_UINT16 i = table.idx[num];
    if (i == 0xFFFF)
        return NULL;
    return &mesgs[i];
}
