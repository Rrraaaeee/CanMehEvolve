FIT_UINT8 Mesg::GetFieldNumValues(const FIT_UINT8 fieldNum, const FIT_UINT16 subFieldIndex) const
{
    const Field *field = GetField(fieldNum);
    if (__builtin_expect(field == FIT_NULL, 0))
        return 0;
    if (__builtin_expect(!CanSupportSubField(fieldNum, subFieldIndex), 0))
        return 0;
    return field->GetNumValues();
}
