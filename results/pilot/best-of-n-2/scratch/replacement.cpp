const Field* Mesg::GetField(const std::string& name) const
{
    const size_t count = fields.size();
    for (size_t i = 0; i < count; i++)
    {
        const Field& f = fields[i];
        if (f.GetName() == name)
            return &f;

        const FIT_UINT16 numSub = f.GetNumSubFields();
        for (FIT_UINT16 j = 0; j < numSub; j++)
        {
            if ((f.GetName(j) == name) && CanSupportSubField(&f, j))
                return &f;
        }
    }

    return FIT_NULL;
}
