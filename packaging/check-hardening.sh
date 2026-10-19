#!/bin/bash
# Check that an ELF binary was built with Fedora's hardening flags.
#
#   packaging/check-hardening.sh /path/to/scroll [...]
#
# Checks: PIE, full RELRO (GNU_RELRO + BIND_NOW), stack protector, FORTIFY,
# and on x86_64 the CET notes (IBT, SHSTK). Exit 1 if any check fails.
set -euo pipefail

die() { echo "check-hardening: $*" >&2; exit 2; }

[[ $# -ge 1 ]] || die "usage: $0 <elf>..."
command -v readelf >/dev/null || die "readelf not found (binutils)"

fail=0
check() { # check <binary> <name> <ok?>
	if [[ $3 == 1 ]]; then
		echo "PASS $2 ($1)"
	else
		echo "FAIL $2 ($1)"
		fail=1
	fi
}

for bin in "$@"; do
	[[ -f $bin ]] || die "$bin: not a file"
	header=$(readelf -hW "$bin")
	segments=$(readelf -lW "$bin")
	dynamic=$(readelf -dW "$bin")
	dynsyms=$(readelf -sW --dyn-syms "$bin")
	notes=$(readelf -nW "$bin" 2>/dev/null)

	pie=0
	if grep -c 'Type: *DYN' <<<"$header" >/dev/null && grep -c 'FLAGS_1.*PIE' <<<"$dynamic" >/dev/null; then
		pie=1
	fi
	check "$bin" pie "$pie"

	relro=0
	grep -c GNU_RELRO <<<"$segments" >/dev/null && relro=1
	check "$bin" relro "$relro"

	now=0
	if grep -cE '\(FLAGS\).*BIND_NOW|FLAGS_1.*NOW' <<<"$dynamic" >/dev/null; then
		now=1
	fi
	check "$bin" bind-now "$now"

	canary=0
	grep -c '__stack_chk_fail' <<<"$dynsyms" >/dev/null && canary=1
	check "$bin" stack-protector "$canary"

	fortify=0
	grep -cE ' __[a-z_]+_chk(@|$)' <<<"$dynsyms" >/dev/null && fortify=1
	check "$bin" fortify "$fortify"

	if grep -c 'Advanced Micro Devices X86-64' <<<"$header" >/dev/null; then
		ibt=0 shstk=0
		feature=$(grep 'x86 feature:' <<<"$notes" || true)
		grep -c IBT <<<"$feature" >/dev/null && ibt=1
		grep -c SHSTK <<<"$feature" >/dev/null && shstk=1
		check "$bin" cet-ibt "$ibt"
		check "$bin" cet-shstk "$shstk"
	fi
done

exit "$fail"
