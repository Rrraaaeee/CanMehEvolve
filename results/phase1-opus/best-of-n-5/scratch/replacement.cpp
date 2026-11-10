Field::Field(const Field &field)
    : FieldBase(static_cast<const FieldBase &>(field))
    , profile(field.profile)
    , profileIndex(field.profileIndex)
    , type(field.type)
    , isFieldExpanded(field.isFieldExpanded)
{
}
