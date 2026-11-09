# Isolated offline native settings

`RESKATE_OFFLINE_LOCAL_SETTINGS=1` selects a bounded native settings boot adapter for an isolated
offline launch. The default is off. This is a launch option, not a plugin API or a requirement of
any plugin. Ordinary ReSkate launches retain their existing startup path.

The selected launch must use offline mode and a checked `--dev-data-root` separate from the normal
player data. Both game folder lookups must already be redirected, and the production offline boot
hook must actually be active. The adapter is installed only during suspended initialization,
before the native settings manager exists. An invalid explicit request stops startup with a reason;
it never silently uses the normal profile. There is no late activation or rearming in that process.

For that launch only, the adapter can preserve the real primary guest user's local settings query
at two inspected native callers. It calls the original predicate once and preserves any true
result. It adapts a false result only for the current first settings query, with the verified native
type and independently observed primary guest/no-provider state. Repeated observations check for
replacement; they do not retain native objects or establish an ownership relationship between a
query and a user. Other callers and queries keep their original predicate result.

Native PC storage receives its own original configuration call only after the actual UTF-8 path
is stable, absolute and resolves inside the selected canonical root. A rejected path returns the
native invalid-path result. A permitted call preserves the native result, including failures;
configuration result zero alone is not evidence that the settings query completed or a player
spawned. The root, selected option and boot prerequisites are checked again at the dependent
boundaries. An observed change to that selection permanently revokes adaptation.

The native settings backend performs its real local I/O and owns its file format. This adapter does
not fabricate an account, storage handle, query request or completion byte. It does not write
entitlements, cosmetics, points, XP or progression values, and offers no plugin access to those
operations. Its path confinement does not prove every field in native files is a setting, pin
filesystem handles, or remove a read-to-use race. Native file-write scope and startup behavior
require an actual game trial on the one supported executable.

Every native address, type, layout offset, callsite and fingerprint is in
`Engine/Game/Build/20260929/local_native_settings.h`. Query, user and path snapshot readers are pure
helpers in `Engine/Game/Profile/local_settings_guard.h`; installation and Win32 path validation
are in `Extension/Boot`.

The focused fixtures build with `DINGOSDK_BUILD_LOCAL_SETTINGS_TESTS=ON`. They cover copied native
observations, admission and permanent revocation, exact original/error forwarding, partial hook
installation rollback, Unicode and canonical path confinement. The junction fixture reports a skip
when Windows cannot create a junction. Pure fixtures do not qualify native calling conventions,
object lifetimes, native file contents or a successful game startup.
