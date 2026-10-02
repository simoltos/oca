#!/bin/sh
set -eu

stage=$(mktemp -d /tmp/oca-install-test-XXXXXX)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
install_oca() {
    make --no-print-directory install DESTDIR="$stage" PREFIX=/usr/local \
        MANUALDIR=/usr/local/share/oca/manual
}

first=$(install_oca)
printf '%s\n' "$first" | grep -qx 'oca: installed'
printf '%s\n' "$first" | grep -qx 'manual: installed'
mkdir "$stage/home"
HOME="$stage/home" "$stage/usr/local/bin/oca" --help > /dev/null
[ ! -e "$stage/home/.oca" ]
HOME="$stage/home" "$stage/usr/local/bin/oca" models > "$stage/listing"
grep -q '^gpt-6-astra ' "$stage/listing"
[ ! -e "$stage/home/.oca/models.json" ]
[ -z "$(install_oca)" ]
[ ! -e "$stage/usr/local/share/oca/models.json" ]
cmp -s oca "$stage/usr/local/bin/oca"

chmod 0600 "$stage/usr/local/bin/oca"
[ "$(install_oca)" = 'oca: updated' ]
[ -z "$(install_oca)" ]

manual="$stage/usr/local/share/oca/manual"
chmod 0600 "$manual/.installed-files"
[ "$(install_oca)" = 'manual: updated' ]
[ -z "$(install_oca)" ]
printf 'old\n' > "$manual/references/obsolete.md"
printf 'private\n' > "$manual/references/my-notes.md"
printf 'references/obsolete.md\n' >> "$manual/.installed-files"
[ "$(install_oca)" = 'manual: updated' ]
[ ! -e "$manual/references/obsolete.md" ]
[ -f "$manual/references/my-notes.md" ]
[ -z "$(install_oca)" ]
printf 'test_install: passed\n'
