#!/bin/bash
# extract-media.sh [project dir]
# Unpacks the PowerPC BeOS media and the most useful documentation archives
# from the user's collections (iso/, docs/) into work/ (gitignored).
# Already extracted entries are skipped, so the script can be re-run.
set -u
P="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
ISO="$P/iso"; DOCS="$P/docs"; W="$P/work"
mkdir -p "$W/media" "$W/docs"

x() { # x <archive> <dest dir>
	local a="$1" d="$2"
	if [ -e "$d/.done" ]; then echo "skip  $d"; return; fi
	mkdir -p "$d"
	echo "unpack $a -> $d"
	if 7z x -y -bd -o"$d" "$a" >"$d/.log" 2>&1; then touch "$d/.done"; else echo "FAILED $a (see $d/.log)"; fi
}

x "$ISO/archive_org/BeOS_1_1d6.7z"                                   "$W/media/DR6"
x "$ISO/archive_org/BeOS_1_1d7_1.7z"                                 "$W/media/DR7"
x "$ISO/archive_org/BeOS_Developer_Release_8_Mac.7z"                 "$W/media/DR8"
x "$ISO/archive_org/BeOS-DR8.2.7z"                                   "$W/media/DR8.2"
x "$ISO/archive_org/BeOS Advanced Access.7z"                         "$W/media/AA"
x "$ISO/archive_org/BeOS Preview Release (230105) (Be, Inc.) (1997).7z" "$W/media/PR1"
x "$ISO/archive_org/BeOS Preview Release 2 (Be, Inc.)(1997).7z"      "$W/media/PR2"
x "$ISO/archive_org/BeOS Release 3 PowerPC.7z"                       "$W/media/R3"
x "$ISO/archive_org/BeOS Release 4.7z"                               "$W/media/R4"
x "$ISO/BeOS CD/BeOS-R4.5.7z"                                        "$W/media/R4.5"

x "$DOCS/DR8/UserDoc/DR8UsersGuide.pdf.7z"                           "$W/docs/DR8UsersGuide"
x "$DOCS/DR8/UserDoc/DR8InstallBook.pdf.7z"                          "$W/docs/DR8InstallBook"
x "$DOCS/publishing/www_arc/www.be.com.7z"                           "$W/docs/www.be.com"
x "$DOCS/development/BeNewsletterArch.7z"                            "$W/docs/BeNewsletter"
x "$DOCS/DR8/BeBook/html/BeBook.DR8.html.tar.7z"                     "$W/docs/BeBookDR8"
x "$DOCS/acrobat/BeDataSh.7z"                                        "$W/docs/BeDataSh"
x "$DOCS/Installing Developer Release 8.1 of BeOS.7z"                "$W/docs/InstallDR8.1"
x "$DOCS/Release Notes BeOS Developer Releases 8 and 8.1.7z"         "$W/docs/RelNotesDR8"
echo "extract-media: done"
