FIT_WSTRING Mesg::GetFieldSTRINGValue(const FIT_UINT8 fieldNum, const FIT_UINT8 fieldArrayIndex, const FIT_UINT16 subFieldIndex) const
{
    const Field *field = GetField(fieldNum);
    if (field == FIT_NULL)
        return FIT_WSTRING_INVALID;

    if (!CanSupportSubField(fieldNum, subFieldIndex))
        return FIT_WSTRING_INVALID;

    const FIT_UINT16 effectiveSubField = (subFieldIndex == FIT_SUBFIELD_INDEX_ACTIVE_SUBFIELD)
                                         ? GetActiveSubFieldIndex(fieldNum)
                                         : subFieldIndex;
    return field->GetSTRINGValue(fieldArrayIndex, effectiveSubField);
}
