#!/usr/bin/env bash
# check_extension.sh PLUGIN [TOOLCHAIN_ROOT] - is this a plugin the launcher can load as an extension?
#
# An extension binds to the launcher's own copy of the SDK when it is loaded (the launcher's
# docs/extensions-plan.md). So it must export its two entry points, ab_extension_abi and
# ab_extension_create, and define nothing of the SDK itself. A plugin that linked ab_core or ab_classic in
# would carry a second Gui, Env and Lang, and draw into a window nobody sees. Exit 1 on either.
set -euo pipefail
plugin="$1"
toolchain="${2:-}"
readelf="$(ls "$toolchain"/bin/*-readelf 2>/dev/null | head -1 || true)"
[ -n "$readelf" ] || readelf=readelf

# the dynamic symbols the plugin defines (not UND), visible from outside
defined="$("$readelf" --dyn-syms -W "$plugin" | awk '$7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") && $6 == "DEFAULT" {print $8}')"
fail=0
for entry in ab_extension_abi ab_extension_create; do
    if ! grep -qx "$entry" <<<"$defined"; then
        echo "$plugin: does not export $entry"
        fail=1
    fi
done
sdk="$(grep -E '^_ZN(3Gui|7AppBase|3Env|6ableem|4Lang|6Config|5Theme|10PanelStyle|12TextRenderer)' <<<"$defined" || true)"
if [ -n "$sdk" ]; then
    echo "$plugin: defines SDK symbols - it links the SDK instead of binding to the launcher's:"
    head -5 <<<"$sdk"
    fail=1
fi
[ "$fail" = 0 ] && echo "$plugin: an extension ($(wc -l <<<"$defined") exported symbols, none of the SDK)"
exit "$fail"
