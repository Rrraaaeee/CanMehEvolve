FIT_UINT64 FieldBase::GetBitsValue64(const FIT_UINT16 offset, const FIT_UINT8 bits) const
{
    return GetBitsValueT<FIT_UINT64>(offset, bits);
}
