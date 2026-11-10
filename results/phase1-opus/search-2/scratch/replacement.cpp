FIT_UINT8 Field::GetNum(void) const
{
    const auto* const p = profile;
    if (__builtin_expect(p != NULL, 1))
    {
        const auto idx = profileIndex;
        if (__builtin_expect(idx < p->numFields, 1))
            return p->fields[idx].num;
    }
    return FIT_FIELD_NUM_INVALID;
}
