    FIT_SIDE GetMountingSide(void) const
    {
        return static_cast<FIT_SIDE>(GetFieldENUMValue(56, 0, FIT_SUBFIELD_INDEX_MAIN_FIELD));
    }
