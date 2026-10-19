#!/bin/bash
# Make a source RPM of a commit of this checkout, offline: the tarball is a
# git archive of that commit, and the spec is that commit's
# packaging/scroll.spec with the @...@ fields filled in.
#
#   packaging/make-srpm.sh [-o <outdir>] [<commit>]   # default: HEAD, ./rpmbuild-out
#
# Prints the path of the SRPM. Refuses a dirty tree when building HEAD,
# because git archive would silently leave the changes out.
set -euo pipefail

die() { echo "make-srpm: $*" >&2; exit 1; }

out=rpmbuild-out
if [[ ${1:-} == -o ]]; then
	[[ $# -ge 2 ]] || die "-o needs a directory"
	out=$2
	shift 2
fi
rev=${1:-HEAD}

top=$(git rev-parse --show-toplevel) || die "not in a git checkout"
command -v rpmbuild >/dev/null || die "rpmbuild not found (dnf install rpm-build)"
commit=$(git -C "$top" rev-parse --verify "$rev^{commit}") || die "unknown commit $rev"
if [[ $rev == HEAD ]] && [[ -n $(git -C "$top" status --porcelain --untracked-files=no) ]]; then
	die "uncommitted changes; commit them or name a commit"
fi
short=${commit:0:8}

# meson.build: "	version: '1.13-dev'," -> 1.13~dev (rpm sorts ~ before release)
version=$(git -C "$top" show "$commit:meson.build" | sed -n "s/^[[:space:]]*version: '\([^']*\)',.*/\1/p" | head -n 1)
[[ -n $version ]] || die "no version in meson.build"
version=${version//-/\~}
snapdate=$(TZ=UTC git -C "$top" log -1 --format=%cd --date=format-local:%Y%m%d "$commit")
changelogdate=$(LC_ALL=C TZ=UTC git -C "$top" log -1 --format=%cd --date=format-local:'%a %b %d %Y' "$commit")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/SOURCES" "$work/SPECS" "$out"

git -C "$top" archive --format=tar.gz --prefix="scroll-$short/" \
	-o "$work/SOURCES/scroll-$short.tar.gz" "$commit"
git -C "$top" show "$commit:packaging/scroll.spec" \
	| sed -e "s/@COMMIT@/$commit/g" -e "s/@SHORTCOMMIT@/$short/g" -e "s/@VERSION@/$version/g" \
		-e "s/@SNAPDATE@/$snapdate/g" -e "s/@CHANGELOGDATE@/$changelogdate/g" \
	> "$work/SPECS/scroll.spec"
if grep -c '@[A-Z]*@' "$work/SPECS/scroll.spec" >/dev/null; then
	die "unfilled @...@ field in the spec"
fi

rpmbuild -bs --define "_topdir $work" "$work/SPECS/scroll.spec" >/dev/null
srpm=$(find "$work/SRPMS" -name '*.src.rpm' -print -quit)
[[ -n $srpm ]] || die "rpmbuild made no SRPM"
cp "$srpm" "$out/"
echo "$out/$(basename "$srpm")"
