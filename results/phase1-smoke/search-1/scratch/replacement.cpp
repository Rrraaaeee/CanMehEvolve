Field* Mesg::GetFieldByIndex(const FIT_UINT16 index)
{
    const size_t i = static_cast<size_t>(index);
    if (i < fields.size())
        return fields.data() + i;

    return FIT_NULL;
}
