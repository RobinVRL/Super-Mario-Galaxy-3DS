#ifndef SMG3DS_PETARI_OVERRIDES_H
#define SMG3DS_PETARI_OVERRIDES_H

/*
 * Hybrid source manifest for the RMGE01 (NTSC-U, revision 0) DOL.
 *
 * Keep guest addresses here even though the ARM implementations have normal
 * host linker symbols. DolRecomp uses the guest address as the stable symbol
 * identity and retains its generated implementation as the fallback.
 *
 * Set an entry's ENABLED value to 0 to route it back to DolRecomp without
 * changing game call sites or removing the imported implementation.
 */
#define SMG3DS_PETARI_MANIFEST_ABI_VERSION 1u
#define SMG3DS_PETARI_GAME_ID "RMGE01"
#define SMG3DS_PETARI_DOL_SHA256 \
    "2C680585A8F58E1CC9C5521B579057F12B124FF0EF409E470A57606C50A93C09"

#define SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS 0x803E5934u
#define SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_SIZE    0x00000028u
#define SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ENABLED 1

#define SMG3DS_PETARI_OVERRIDES(APPLY, CONTEXT)                         \
    APPLY(CONTEXT, mr_is_near_zero_f32_f32,                            \
          SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ADDRESS,               \
          SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_SIZE,                  \
          SMG3DS_PETARI_MR_IS_NEAR_ZERO_F32_F32_ENABLED)

/* Small preprocessor helpers let disabled manifest rows disappear entirely. */
#define SMG3DS_PETARI_JOIN_INNER(A, B) A##B
#define SMG3DS_PETARI_JOIN(A, B) SMG3DS_PETARI_JOIN_INNER(A, B)
#define SMG3DS_PETARI_IF_0(...)
#define SMG3DS_PETARI_IF_1(...) __VA_ARGS__
#define SMG3DS_PETARI_IF(ENABLED) \
    SMG3DS_PETARI_JOIN(SMG3DS_PETARI_IF_, ENABLED)

/*
 * Generated local branches normally become C gotos and never reach the outer
 * replacement dispatcher. DolRecomp's emitter calls this predicate before a
 * local goto; enabled guest entry points are forced back through dispatch.
 */
#define SMG3DS_PETARI_FORCE_DISPATCH_ENTRY(                             \
    CANDIDATE, NAME, ADDRESS, SIZE, ENABLED)                            \
    SMG3DS_PETARI_IF(ENABLED)(|| ((CANDIDATE) == (ADDRESS)))

#ifndef DOLRECOMP_FORCE_DISPATCH
#define DOLRECOMP_FORCE_DISPATCH(CANDIDATE)                             \
    (0 SMG3DS_PETARI_OVERRIDES(                                        \
        SMG3DS_PETARI_FORCE_DISPATCH_ENTRY, (CANDIDATE)))
#endif

#endif /* SMG3DS_PETARI_OVERRIDES_H */
