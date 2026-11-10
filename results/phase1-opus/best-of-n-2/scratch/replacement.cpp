FIT_UINT8 Field::GetType() const
{
    const FIT_UINT8 t = type;
    if (__builtin_expect(t != FIT_UINT8_INVALID, 1))
        return t;

    const auto* const p = profile;
    if ((p == NULL) || (profileIndex >= p->numFields))
        return FIT_UINT8_INVALID;

    return p->fields[profileIndex].type;
}
