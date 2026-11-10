template <typename TTo, typename TFrom>
TTo FieldBase::ConvertBaseType(const FIT_UINT8 fieldArrayIndex, const FIT_UINT8 toBaseType) const
{
    FIT_CONST_UINT8_PTR invalid = baseTypeInvalids[GetType() & FIT_BASE_TYPE_NUM_MASK];
    const TFrom value = GetValue<TFrom>(fieldArrayIndex);

    TFrom invalidValue;
    memcpy(&invalidValue, invalid, sizeof(TFrom));

    if (memcmp(&value, &invalidValue, sizeof(TFrom)) == 0)
    {
        TTo result;
        memcpy(&result, baseTypeInvalids[toBaseType & FIT_BASE_TYPE_NUM_MASK], sizeof(TTo));
        return result;
    }

    return static_cast<TTo>(value);
}
