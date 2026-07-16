#!/usr/bin/env bash
##
# @file install.sh
# @brief Top-level build, install, and device handler for the orientation project.
#
# Lives at the repository root, alongside @c client/, @c common/, @c mcu/ and @c init/.
#
# ### What this project is
# An ESP32 watches its accelerometer and tells a Linux PC to rotate the external display.
# Two halves ship from one repo:
# - @b client/ -- the PC side: an orientation daemon plus @c ap_provision, an interactive
#   utility for storing Wi-Fi credentials and handing them to the ESP.
# - @b mcu/ -- the ESP32 firmware.
#
# ### Commands
# @verbatim
#   ./install.sh                  build both halves
#   ./install.sh client           build the PC binaries only
#   ./install.sh mcu              build the firmware only
#   ./install.sh install          build, then install binaries + the login service
#   ./install.sh clean            clean both halves
#   ./install.sh env              (re)detect the ESP-IDF and regenerate env/idf_env.sh
#   ./install.sh --help           print usage and exit
#   ./install.sh <anything else>  forwarded verbatim to idf.py
# @endverbatim
#
# Anything unrecognised is passed straight to @c idf.py, so @c flash, @c monitor,
# @c menuconfig, @c size and the rest work without this script having to enumerate them:
# @verbatim
#   ./install.sh flash
#   ./install.sh -p /dev/ttyUSB1 flash monitor
# @endverbatim
#
# ### Why the client is built first, and in the parent shell
# Sourcing the ESP-IDF environment prepends the xtensa cross-compiler to @c PATH. If that
# happened before the client build, @c make would pick up the cross-compiler and try to
# build x86 PC binaries with it. So the client is built FIRST, in a shell whose @c PATH has
# never been touched, and the IDF environment is sourced ONLY inside the ::idf_run subshell
# -- where it evaporates on exit. No toolchain collision is possible by construction.
#
# ### ESP-IDF discovery
# The IDF's location is a fact about the user's machine, not about this repository, so it is
# discovered at install time (see ::detect_idf) and frozen into @c env/idf_env.sh, which is
# gitignored. A committed path would be correct for exactly one developer.
#
# @warning Requires ESP-IDF @c v5.0 or newer. The firmware uses the v5 @c esp_netif and
#          @c esp_event APIs; on v4.x the build fails with a wall of compiler errors rather
#          than anything informative, so ::check_idf_version refuses up front instead.
##

set -euo pipefail

##
# @name Paths
# @{
##
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"	            ##< Repository root (this file's directory).
CLIENT_DIR="$ROOT/client"				                            ##< PC-side sources and Makefile.
MCU_DIR="$ROOT/mcu"					                                ##< ESP-IDF project.
SERVICE_DIR="$ROOT/service"				                            ##< Holds the autostart route: the .desktop entry and its session wrapper.
ENV_DIR="$ROOT/env"					                                ##< Generated environment lives here.
ENV_TEMPLATE="$ENV_DIR/idf_env.sh.in"			                    ##< Shipped template, with @c \@IDF_PATH\@ placeholders.
ENV_SCRIPT="$ENV_DIR/idf_env.sh"			                        ##< Generated, machine-specific, gitignored.
##@}

##
# @name Requirements
# @{
##
IDF_MIN_VERSION="5.0"					                            ##< Minimum supported ESP-IDF. See the file-level warning.
##@}

##
# @name Install destinations
# @brief User-local, so the whole install needs no root.
# @{
##
BIN_DIR="$HOME/.local/bin"				                            ##< Both binaries land here; on PATH by default on most distros.
DAEMON_NAME="esp_daemon"				                            ##< The daemon: built, installed and serviced under one name.
PROV_NAME="ap_provision"				                            ##< The interactive provisioning utility.
WRAPPER_NAME="esp_daemon-session"			                        ##< Session wrapper; holds the restart loop for the autostart route.
##@}

##
# @name Client dependency floor
# @{
##
OPENSSL_MIN="1.1.0"					                                ##< See ::check_client_deps for why this, and not 3.x.
##@}


##
# @brief Print a step banner.
# @param $* Message.
##
say() {
	echo "==> $*"
}

##
# @brief Print an error and abort.
# @param $* Message.
##
die() {
	echo "==> ERROR: $*" >&2
	exit 1
}

##
# @brief Print the usage text and exit.
#
# The whole-project verbs are listed; the "anything else -> idf.py" passthrough is noted rather
# than enumerated, for the same reason the dispatcher does not enumerate it -- keeping a copy of
# idf.py's command list in sync with upstream would be a maintenance treadmill.
##
usage() {
	cat <<-EOF
	usage: ./install.sh [command]

	  (no command)   build both halves (the PC binaries and the firmware)
	  client         build the PC binaries only
	  mcu            build the firmware only
	  install        build, then install the binaries and the login service
	  clean          clean both halves
	  env            (re)detect the ESP-IDF and regenerate env/idf_env.sh
	  --help         this message

	Anything not listed above is forwarded verbatim to idf.py, so the usual firmware
	commands work through this script without being enumerated here:

	  ./install.sh flash
	  ./install.sh monitor
	  ./install.sh -p /dev/ttyUSB1 flash monitor

	The daemon and the provisioning utility install under ~/.local/bin, and the login
	service is a per-user XDG autostart entry -- nothing here needs root.
	EOF

	exit 0
}

##
# @brief Locate an ESP-IDF installation on this machine.
#
# Tried in order of trustworthiness:
# -# @c $IDF_PATH already exported -- the user has an environment set up, believe it;
# -# a previously generated @c env/idf_env.sh -- this script already answered the question;
# -# the conventional install locations;
# -# ask.
#
# A directory only counts if it contains @c export.sh, which is the IDF's own entry point.
# A bare directory of the right name is not an IDF.
#
# @return Prints the discovered path on stdout; empty if nothing was found and the user
#         declined to supply one.
##
detect_idf() {
	local candidate

	if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
		echo "$IDF_PATH"
		return 0
	fi

	if [ -f "$ENV_SCRIPT" ]; then
		candidate="$(grep -m1 '^export IDF_PATH=' "$ENV_SCRIPT" | cut -d'"' -f2 || true)"

		if [ -n "$candidate" ] && [ -f "$candidate/export.sh" ]; then
			echo "$candidate"
			return 0
		fi
	fi

	for candidate in \
		"$HOME/esp/esp-idf" \
		"$HOME/embedded/sdks/esp/esp-idf" \
		"$HOME/.espressif/esp-idf" \
		"/opt/esp-idf" \
		"/usr/share/esp-idf"
	do
		if [ -f "$candidate/export.sh" ]; then
			echo "$candidate"
			return 0
		fi
	done

	echo "" >&2
	echo "ESP-IDF was not found automatically." >&2
	echo "Looked for an export.sh in:" >&2
	echo "    \$IDF_PATH, ~/esp/esp-idf, ~/embedded/sdks/esp/esp-idf," >&2
	echo "    ~/.espressif/esp-idf, /opt/esp-idf, /usr/share/esp-idf" >&2
	echo "" >&2
	echo "Enter the path to your esp-idf directory (or leave blank to skip the" >&2
	echo "firmware and build the PC side only):" >&2
	printf "IDF_PATH: " >&2

	read -r candidate

	if [ -z "$candidate" ]; then
		echo ""
		return 0
	fi

	candidate="${candidate/#\~/$HOME}"

	if [ ! -f "$candidate/export.sh" ]; then
		die "no export.sh in '$candidate' -- that does not look like an ESP-IDF tree."
	fi

	echo "$candidate"
}

##
# @brief Read the ESP-IDF version from a tree.
#
# Tries @c version.txt (present in release tarballs) and falls back to @c git @c describe
# (present in a cloned tree). The leading @c 'v' is stripped, and anything after the
# major.minor is discarded -- @c "v5.5.3-dirty" and @c "v5.5.3" both reduce to @c "5.5".
#
# @param $1 Path to the IDF.
# @return Prints @c "major.minor", or nothing if the version could not be read.
##
idf_version() {
	local idf="$1"
	local raw=""

	if [ -f "$idf/version.txt" ]; then
		raw="$(cat "$idf/version.txt")"
	elif [ -d "$idf/.git" ]; then
		raw="$(git -C "$idf" describe --tags 2>/dev/null || true)"
	fi

	if [ -z "$raw" ]; then
		return 0
	fi

	raw="${raw#v}"
	echo "$raw" | cut -d. -f1,2
}

##
# @brief Refuse to build against an ESP-IDF older than ::IDF_MIN_VERSION.
#
# The firmware is written against the v5 @c esp_netif / @c esp_event APIs. On v4.x it does
# not fail gracefully -- it fails as several hundred lines of compiler errors, which tells
# the user nothing. Checking the version up front turns that into one actionable sentence.
#
# An unreadable version is a warning, not a failure: some vendored trees carry neither
# @c version.txt nor a git history, and refusing to build those would be worse than trying.
#
# @param $1 Path to the IDF.
##
check_idf_version() {
	local idf="$1"
	local have

	have="$(idf_version "$idf")"

	if [ -z "$have" ]; then
		say "[mcu] WARNING: could not determine the ESP-IDF version in $idf"
		say "[mcu]          this project needs v$IDF_MIN_VERSION or newer; continuing anyway."
		return 0
	fi

	local have_major="${have%%.*}"
	local have_minor="${have##*.}"
	local min_major="${IDF_MIN_VERSION%%.*}"
	local min_minor="${IDF_MIN_VERSION##*.}"

	if [ "$have_major" -gt "$min_major" ]; then
		say "[mcu] ESP-IDF v$have detected (need >= v$IDF_MIN_VERSION)"
		return 0
	fi

	if [ "$have_major" -eq "$min_major" ] && [ "$have_minor" -ge "$min_minor" ]; then
		say "[mcu] ESP-IDF v$have detected (need >= v$IDF_MIN_VERSION)"
		return 0
	fi

	echo "" >&2
	die "ESP-IDF v$have is too old.

    This firmware requires v$IDF_MIN_VERSION or newer: it uses the v5 esp_netif and
    esp_event APIs, which do not exist in v4.x. Building against v$have would
    produce a wall of compiler errors rather than anything useful.

    Found at: $idf

    Install a supported ESP-IDF and re-run ./install.sh env"
}

##
# @brief Generate @c env/idf_env.sh from the template with real paths substituted.
#
# Called whenever the environment is missing, or explicitly via @c "./install.sh env" after
# the IDF has moved.
#
# @c IDF_TOOLS_PATH is preserved from the caller's environment when it is already set, so a
# custom toolchain location survives; otherwise Espressif's own default is used rather than
# inventing one.
#
# @param $1 Path to the IDF.
##
generate_env() {
	local idf="$1"

	[ -f "$ENV_TEMPLATE" ] || die "missing template: $ENV_TEMPLATE"

	local tools="${IDF_TOOLS_PATH:-$HOME/.espressif}"
	local port="${ESPPORT:-/dev/ttyUSB0}"

	mkdir -p "$ENV_DIR"

	sed -e "s|@IDF_PATH@|$idf|g" \
	    -e "s|@IDF_TOOLS_PATH@|$tools|g" \
	    -e "s|@ESPPORT@|$port|g" \
	    "$ENV_TEMPLATE" > "$ENV_SCRIPT"

	chmod 0644 "$ENV_SCRIPT"

	say "[env] wrote $ENV_SCRIPT"
	say "[env]   IDF_PATH        = $idf"
	say "[env]   IDF_TOOLS_PATH  = $tools"
	say "[env]   ESPPORT         = $port"
}

##
# @brief Ensure a usable @c env/idf_env.sh exists, generating it if needed.
#
# @return 0 if the environment is ready; 1 if no IDF is available (the caller then skips the
#         firmware and builds the PC side only, rather than aborting -- a user who only wants
#         the daemon should not be forced to install an SDK for a chip they are not building).
##
ensure_env() {
	if [ -f "$ENV_SCRIPT" ]; then
		local existing
		existing="$(grep -m1 '^export IDF_PATH=' "$ENV_SCRIPT" | cut -d'"' -f2 || true)"

		if [ -n "$existing" ] && [ -f "$existing/export.sh" ]; then
			check_idf_version "$existing"
			return 0
		fi

		say "[env] $ENV_SCRIPT points at a missing IDF, re-detecting ..."
	fi

	local idf
	idf="$(detect_idf)"

	if [ -z "$idf" ]; then
		return 1
	fi

	check_idf_version "$idf"
	generate_env "$idf"

	return 0
}

##
# @brief Verify the PC-side build dependencies before invoking make.
#
# Without this, a missing OpenSSL header surfaces as
# @verbatim
#   ap_provision.c:75:10: fatal error: openssl/evp.h: No such file or directory
# @endverbatim
# which names the symptom and not the cure. Checking first turns that into the one command
# the user actually needs to run.
#
# ### The OpenSSL floor is 1.1.0, not 3.x
# @c ap_provision.c uses @c EVP_CIPHER_CTX_new, @c EVP_EncryptInit_ex, the @c EVP_CTRL_GCM_*
# controls, @c PKCS5_PBKDF2_HMAC, @c RAND_bytes and @c OPENSSL_cleanse. Every one of those is
# 1.1.0-era API and is still present, unchanged, in 3.x. Nothing here touches the 3.x provider
# API (@c EVP_MAC, @c OSSL_PARAM), so the code compiles against both -- and pinning the floor
# at 3.x would lock out Debian 10, Ubuntu 18.04 and RHEL 8 for no reason at all.
#
# 1.0.x genuinely will not work: it had no @c EVP_CIPHER_CTX_new, using a stack-allocated
# context and @c EVP_CIPHER_CTX_init instead.
#
# @note pthread is not checked. @c main.c and @c netlink_monitor.c use only @c pthread_create
#       and @c pthread_join, which are POSIX and present in every glibc and musl ever shipped.
#       There is no version worth asserting.
##
check_client_deps() {
	local missing=0

	##
	# Probe with the SAME compiler the build will use. ::build_client unsets CC so the
	# Makefile falls back to gcc, so the probe must do likewise -- testing one compiler and
	# building with another would make this check worthless.
	##
	local cc="gcc"

	if ! command -v "$cc" >/dev/null 2>&1; then
		echo "" >&2
		echo "==> ERROR: no C compiler found (looked for '$cc')." >&2
		echo "" >&2
		echo "    Debian/Ubuntu:  sudo apt install build-essential" >&2
		echo "    Fedora/RHEL:    sudo dnf install gcc make" >&2
		echo "    Arch:           sudo pacman -S base-devel" >&2
		echo "    Alpine:         sudo apk add build-base" >&2
		missing=1
	fi

	##
	# Probe by compiling, not by looking for a file. The header can live in a dozen places
	# depending on distro and multiarch layout, and only the compiler knows its own search
	# path.
	##
	if ! echo '#include <openssl/evp.h>
int main(void){ return 0; }' | "$cc" -x c - -o /dev/null 2>/dev/null; then
		echo "" >&2
		echo "==> ERROR: the OpenSSL development headers were not found." >&2
		echo "" >&2
		echo "    ap_provision needs them for AES-256-GCM and PBKDF2." >&2
		echo "" >&2
		echo "    Debian/Ubuntu:  sudo apt install libssl-dev" >&2
		echo "    Fedora/RHEL:    sudo dnf install openssl-devel" >&2
		echo "    Arch:           sudo pacman -S openssl" >&2
		echo "    Alpine:         sudo apk add openssl-dev" >&2
		missing=1
	fi

	if [ "$missing" -eq 1 ]; then
		exit 1
	fi

	##
	# Version check is advisory: pkg-config may be absent on a machine where OpenSSL is
	# perfectly fine, and refusing to build over a missing metadata tool would be absurd.
	##
	if command -v pkg-config >/dev/null 2>&1; then
		local have
		have="$(pkg-config --modversion openssl 2>/dev/null || true)"

		if [ -n "$have" ]; then
			local have_major="${have%%.*}"
			local min_major="${OPENSSL_MIN%%.*}"

			if [ "$have_major" -lt "$min_major" ]; then
				echo "" >&2
				die "OpenSSL $have is too old.

    ap_provision requires >= $OPENSSL_MIN. Version 1.0.x lacks EVP_CIPHER_CTX_new
    and the GCM control interface this code is built on.

    Install a newer libssl-dev / openssl-devel and try again."
			fi

			say "[client] OpenSSL $have detected (need >= $OPENSSL_MIN)"
		fi
	fi
}

##
# @brief Build the PC binaries with a sanitised, ESP-free environment.
#
# ### Two separate protections, against two separate hazards
#
# The first is structural and comes for free: this script never sources the IDF environment in
# its own shell. That only ever happens inside the ::idf_run subshell, which exits before
# anything else runs. So nothing @e this @e script does can contaminate the client build.
#
# The second is the one that is easy to miss. A user may have sourced @c export.sh in their own
# shell before invoking us:
# @verbatim
#   source ~/esp/esp-idf/export.sh
#   ./install.sh
# @endverbatim
# and we would then INHERIT a polluted environment -- @c PATH carrying the xtensa toolchain and
# the IDF's Python virtualenv, plus @c IDF_PATH, @c IDF_PYTHON_ENV_PATH, @c OPENOCD_SCRIPTS and
# the rest. Subshell isolation does nothing about variables that were already set before the
# script started.
#
# ### Why the build would probably survive that anyway -- and why that is not good enough
# The Makefile declares @c "CC ?= gcc", so it uses @c $CC from the environment when one is set.
# The IDF does not export @c CC, and the cross-compiler is named @c xtensa-esp32-elf-gcc rather
# than @c gcc, so a bare @c gcc still resolves to the system compiler. The build survives -- but
# only because two names happen not to collide, which is luck, not design. It also leaves
# @c pkg-config exposed to a @c PKG_CONFIG_PATH the IDF might one day set, which would hand the
# OpenSSL detection the wrong flags.
#
# So the environment is stripped explicitly instead. Unsetting @c CC and @c CXX is the
# load-bearing part: it forces the Makefile's @c ?= default back to @c gcc, and @c gcc is
# unambiguously the system one.
#
# @note @c PATH is deliberately NOT rewritten. Filtering IDF entries out of it would mean
#       pattern-matching paths, which is fragile and would break the moment Espressif changed
#       its layout. Clearing the compiler variables achieves the same end without guessing.
#
# @param $@ Forwarded to @c make.
##
build_client() {
	check_client_deps

	say "[client] building the PC binaries ..."

	(
		unset IDF_PATH IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH IDF_TARGET
		unset OPENOCD_SCRIPTS ESP_ROM_ELF_DIR ESPPORT
		unset CXX LD AR RANLIB
		unset PKG_CONFIG_PATH

		##
		# CC is PINNED rather than merely unset, and passed on make's command line so it
		# overrides the Makefile's "CC ?= gcc" outright.
		#
		# Unsetting alone is not enough: make carries a built-in default of "cc", which wins
		# before "?=" ever gets a chance to fire. On most systems cc is a symlink to gcc, so
		# it would work -- but that is an assumption about the user's machine, and it is the
		# exact class of assumption this function exists to remove. Naming gcc explicitly
		# means the client is built by the compiler we actually probed in
		# ::check_client_deps.
		##
		cd "$CLIENT_DIR" && make CC=gcc "$@"
	)

	say "[client] done."
}

##
# @brief Run @c idf.py inside an isolated subshell.
#
# The ordering inside the subshell is deliberate:
# -# @c cd into @c mcu/ first, so any @c $PWD-relative paths resolve against the IDF project;
# -# source the environment next, bringing the xtensa toolchain onto @c PATH -- but only
#    inside this subshell;
# -# run @c idf.py last.
#
# The @c && chain means a @c "return 1" from the environment script aborts before @c idf.py
# runs. When the subshell exits, every variable it exported (@c PATH included) disappears
# with it, so the caller's shell is never contaminated.
#
# @param $@ Forwarded verbatim to @c idf.py.
##
idf_run() {
	if [ ! -f "$ENV_SCRIPT" ]; then
		ensure_env || die "no ESP-IDF available -- cannot run idf.py $*"
	fi

	say "[mcu] idf.py $* ..."
	(
		cd "$MCU_DIR" \
		&& source "$ENV_SCRIPT" \
		&& check_toolchain \
		&& idf.py "$@"
	)
	say "[mcu] idf.py $* done."
}

##
# @brief Work out WHY the toolchain is unusable, and say so accurately.
#
# Called from inside the ::idf_run subshell, AFTER the environment has been sourced -- the only
# point at which the compiler could be on @c PATH.
#
# ### Three different failures look identical from PATH alone
# The obvious check is @c "command @c -v @c xtensa-esp32-elf-gcc", and it is what this function used
# to do. It is wrong, because a compiler missing from @c PATH does not mean a compiler that is not
# installed -- and reporting the wrong cause sends the user off to fix something that was never
# broken.
#
# @c export.sh is what puts the toolchain on @c PATH, and it does that LAST, after its own checks
# pass. If it aborts early -- most commonly because the Python virtualenv under @c IDF_TOOLS_PATH is
# missing or was built for a different Python -- then it never reaches the @c PATH step, and the
# compiler stays invisible even though it is sitting on disk exactly where it should be.
#
# So there are three distinct states, and they need three distinct messages:
#
# -# @b Toolchain @b absent. @c IDF_TOOLS_PATH has no @c xtensa-esp32-elf directory. The IDF was
#    cloned but @c install.sh was never run. This is the case the original check assumed.
# -# @b Toolchain @b present, @b venv @b missing. The compiler is on disk; the Python environment is
#    not, so @c export.sh bailed before exporting it. Typically a Python UPGRADE: the venv is named
#    for the version it was built with (@c idf5.5_py3.11_env), so moving to 3.12 orphans it.
# -# @b Both @b present, @b still @b not @b on @b PATH. Something else in @c export.sh failed. Show
#    what it actually printed rather than guessing.
#
# The remedy happens to be the same command in all three cases, but the DIAGNOSIS is not, and a user
# told "your compiler is not installed" while looking at a directory full of compiler will
# reasonably conclude the script is broken.
##
check_toolchain() {
	if command -v xtensa-esp32-elf-gcc >/dev/null 2>&1; then
		return 0
	fi

	local tools="${IDF_TOOLS_PATH:-$HOME/.espressif}"
	local have_compiler=0
	local have_venv=0

	if [ -d "$tools/tools/xtensa-esp-elf" ] || [ -d "$tools/tools/xtensa-esp32-elf" ]; then
		have_compiler=1
	fi

	if [ -d "$tools/python_env" ] && [ -n "$(ls -A "$tools/python_env" 2>/dev/null)" ]; then
		have_venv=1
	fi

	echo "" >&2

	if [ "$have_compiler" -eq 0 ]; then
		echo "==> ERROR: the ESP32 cross-compiler is not installed." >&2
		echo "" >&2
		echo "    The ESP-IDF is at:" >&2
		echo "        $IDF_PATH" >&2
		echo "" >&2
		echo "    ...but there is no toolchain under:" >&2
		echo "        $tools/tools" >&2
		echo "" >&2
		echo "    Cloning the IDF and installing its compiler are two separate" >&2
		echo "    steps, and only the first has been done." >&2

	elif [ "$have_venv" -eq 0 ]; then
		##
		# The interesting case, and the one the old check got wrong.
		##
		echo "==> ERROR: the toolchain is installed, but the IDF cannot use it." >&2
		echo "" >&2
		echo "    The compiler IS on disk:" >&2
		echo "        $tools/tools/" >&2
		echo "" >&2
		echo "    What is missing is the IDF's Python virtualenv, and export.sh" >&2
		echo "    aborts before it can put the compiler on PATH." >&2
		echo "" >&2
		echo "    This usually means Python was upgraded. The venv is named for the" >&2
		echo "    version it was built against -- idf5.5_py3.11_env -- so a move to" >&2
		echo "    3.12 leaves it orphaned. Nothing is broken; it just needs rebuilding." >&2

	else
		echo "==> ERROR: the compiler is installed and so is the Python venv," >&2
		echo "    but sourcing the IDF environment did not put it on PATH." >&2
		echo "" >&2
		echo "    Something else in export.sh failed. Run it by hand to see:" >&2
		echo "" >&2
		echo "        . \"$IDF_PATH/export.sh\"" >&2
	fi

	echo "" >&2
	echo "    Fix, in all three cases:" >&2
	echo "" >&2
	echo "        cd \"$IDF_PATH\" && ./install.sh esp32" >&2
	echo "" >&2

	return 1
}

##
# @brief Build the firmware, skipping cleanly if no ESP-IDF is installed.
#
# A missing IDF is not an error here: a user who only wants the PC daemon should not be
# forced to install a cross-toolchain for a chip they are not building.
##
build_mcu() {
	if ! ensure_env; then
		say "[mcu] skipped: no ESP-IDF found (the PC side was still built)."
		return 0
	fi

	idf_run build
}

##
# @brief Clean both halves.
#
# The firmware clean is best-effort: with no IDF there is nothing to clean, and that should
# not fail the whole command.
##
clean_all() {
	say "cleaning the client ..."
	( cd "$CLIENT_DIR" && make clean )

	if [ -f "$ENV_SCRIPT" ]; then
		idf_run fullclean || true
	else
		say "[mcu] clean skipped: no ESP-IDF environment."
	fi
}

##
# @name The login service: one route, and why
#
# The daemon rotates the screen with @c xrandr, which can only reach an X server if it holds that
# session's @c DISPLAY and its @c XAUTHORITY cookie. So the daemon MUST run as the logged-in user,
# INSIDE their session. Anything else is refused by X -- silently -- and every rotation fails while
# the daemon reports itself perfectly healthy.
#
# There is exactly one mechanism that satisfies that on every desktop and every init system: XDG
# autostart. @c ~/.config/autostart is read by the DESKTOP SESSION, after the display manager has
# authenticated the user, started X, and built the environment -- so the daemon starts as that user,
# in that session, with both variables already present. No root, nothing substituted, per-user by
# construction (two people on one machine each get their own, in their own home).
#
# ### Why not a systemd user unit, since systemd could express this too?
# It can, via @c graphical-session.target -- but that target is a CONVENTION, not a guarantee.
# GNOME and KDE activate it; Cinnamon, XFCE, i3, sway and others frequently do not, and on those the
# unit sits inert inside a perfectly good X session while @c "systemctl --user enable" reports
# success. That failure is silent and baffling. Binding to @c default.target instead trades one
# silent failure for another (it is reached for SSH logins too, with no DISPLAY). The autostart
# directory, by contrast, is read by the session itself and so is always in step with the session's
# lifetime -- which is the entire property we need. Rather than ship two routes that behave
# differently on different desktops, the installer ships the one that behaves the same everywhere.
#
# ### Why not a system init script (OpenRC, runit, s6, SysV)?
# They are SYSTEM supervisors: they start services at BOOT, as ROOT, when nobody is logged in and no
# X server exists -- so a service they start has no cookie and cannot talk to X. They offer no login
# hook to use instead, because they have no concept of a login; the display manager owns that, long
# after init has finished.
#
# The one thing autostart lacks is supervision, which the wrapper it launches supplies with a
# restart loop. A fair trade: the daemon's own main loop already recovers from every network failure
# internally, so the wrapper is a backstop rather than a crutch.
##

##
# @brief Warn if the session is Wayland rather than X11.
#
# The daemon rotates the screen by forking @c xrandr, which is an X11 tool and nothing else. On a
# Wayland session it is either absent or talks to XWayland -- where it can enumerate outputs and
# then rotate precisely nothing, because the real compositor owns them.
#
# The resulting failure is genuinely nasty to debug: the daemon finds the ESP, connects, receives
# every orientation command and acknowledges it, and the screen never moves. Everything looks
# healthy. Saying so at install time costs one variable read.
#
# A warning, not a refusal: the user may be installing on a machine they will later log into under
# X, and it is not this script's place to guess.
##
check_display_server() {
	if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
		say "[install] ------------------------------------------------------------"
		say "[install] WARNING: this looks like a Wayland session."
		say "[install]"
		say "[install] The daemon rotates the display with xrandr, which is X11-only."
		say "[install] Under Wayland it will connect to the ESP and acknowledge every"
		say "[install] command while rotating nothing at all -- the compositor owns the"
		say "[install] outputs, not X."
		say "[install]"
		say "[install] To use it, log in to an X11 (Xorg) session instead. Most display"
		say "[install] managers offer this from a gear icon on the login screen."
		say "[install] ------------------------------------------------------------"
		return 0
	fi

	if [ -z "${XDG_SESSION_TYPE:-}" ] && [ -z "${DISPLAY:-}" ]; then
		say "[install] NOTE: no graphical session detected in this shell."
		say "[install]       that is fine if you are installing over SSH -- the daemon"
		say "[install]       needs X only when it RUNS, not when it is installed."
	fi

	##
	# Explicit success. Without it the function falls off its end carrying the exit status of
	# whatever test evaluated last -- and under 'set -e' a false test there would abort the entire
	# install, silently.
	##
	return 0
}

##
# @brief Render a template into place, substituting @c \@BIN_DIR\@.
#
# @param $1 Source, relative to @c service/.
# @param $2 Destination path.
# @param $3 Mode.
##
render() {
	local source="$SERVICE_DIR/$1"
	local dest="$2"
	local mode="$3"

	[ -f "$source" ] || die "missing template: $source"

	mkdir -p "$(dirname "$dest")"

	sed -e "s|@BIN_DIR@|$BIN_DIR|g" "$source" > "$dest"
	chmod "$mode" "$dest"

	say "[install]   $dest"
}

##
# @brief Install the XDG autostart entry and its wrapper, and start it.
#
# Two files: the @c .desktop entry the desktop session reads at login, and the wrapper it launches.
# The wrapper holds the restart loop, because @c Exec= in a @c .desktop file is parsed by the desktop
# rather than by a shell and cannot express one.
#
# This is the only route the installer sets up; see the "one route, and why" notes above for the
# reasoning. There is nothing to "enable" -- placing the @c .desktop file IS enabling it, since the
# desktop re-reads @c ~/.config/autostart afresh at every login.
##
install_service() {
	render "autostart/$DAEMON_NAME.desktop" "$HOME/.config/autostart/$DAEMON_NAME.desktop" 0644
	render "autostart/$WRAPPER_NAME"        "$BIN_DIR/$WRAPPER_NAME"                        0755

	say "[install] autostart entry installed -- it will start at your next login."
	say "[install]"
	say "[install] The DESKTOP SESSION starts the daemon, so it runs as you, inside your"
	say "[install] session, and inherits DISPLAY and XAUTHORITY for free -- no root, and no"
	say "[install] username baked into a file in /etc."
	say "[install]"

	##
	# This login already happened, so the desktop will not re-scan the directory until the next one.
	# Start it by hand now if there is a session to start it in.
	#
	# Output is deliberately NOT redirected to a file here. The daemon writes plain text to stdout and
	# stderr and lets whatever launches it decide where that lands (see client/include/log.h). At the
	# next login that "whatever" is the desktop session, which routes it to ~/.xsession-errors on most
	# desktops. Starting it by hand here inherits THIS shell's stdout/stderr instead -- so on an
	# interactive install you see the first lines right here, which is a useful confirmation.
	##
	if [ -n "${DISPLAY:-}" ]; then
		say "[install] an X session is active -- starting it now ..."

		setsid "$BIN_DIR/$WRAPPER_NAME" &
		disown 2>/dev/null || true

		say "[install] started."
	else
		say "[install] no X session here -- it will start at your next graphical login."
	fi

	say "[install]"
	say "[install] logs:    ~/.xsession-errors      (where most desktops send session output)"
	say "[install] stop:    pkill -f $WRAPPER_NAME"
	say "[install] disable: rm ~/.config/autostart/$DAEMON_NAME.desktop"
}

##
# @brief Install the binaries and the login-time service.
#
# Everything lands under @c $HOME -- @c ~/.local/bin for the binaries, @c ~/.config/autostart for
# the service -- so nothing here needs root. That is not a convenience: a daemon that must hold the
# user's X cookie has no business running as root in the first place.
##
do_install() {
	build_client
	build_mcu

	say "[install] installing the binaries into $BIN_DIR ..."
	mkdir -p "$BIN_DIR"

	local daemon_bin="$CLIENT_DIR/build/$DAEMON_NAME"
	local prov_bin="$CLIENT_DIR/build/$PROV_NAME"

	[ -f "$daemon_bin" ] || die "daemon binary not found at $daemon_bin -- did 'make' succeed?"
	[ -f "$prov_bin" ]   || die "provisioning binary not found at $prov_bin -- did 'make' succeed?"

	install -m 0755 "$daemon_bin" "$BIN_DIR/$DAEMON_NAME"
	install -m 0755 "$prov_bin"   "$BIN_DIR/$PROV_NAME"

	say "[install]   $BIN_DIR/$DAEMON_NAME"
	say "[install]   $BIN_DIR/$PROV_NAME"

	case ":$PATH:" in
		*":$BIN_DIR:"*) ;;
		*)
			say "[install] NOTE: $BIN_DIR is not on your PATH."
			say "[install]       add this to your shell rc:"
			say "[install]           export PATH=\"\$HOME/.local/bin:\$PATH\""
			;;
	esac

	check_display_server

	install_service

	say "[install] done."
}

##
# @brief Dispatch.
#
# Whole-project verbs are handled here; everything else is forwarded to @c idf.py untouched.
# The idf commands are deliberately NOT enumerated -- keeping a list of them in sync with
# upstream would be a maintenance treadmill, and @c idf.py already rejects what it does not
# recognise.
##
cmd="${1:-all}"

case "$cmd" in
	all|"")
		build_client
		build_mcu
		;;
	client)
		shift || true
		build_client "$@"
		;;
	mcu)
		build_mcu
		;;
	install)
		do_install
		;;
	clean)
		clean_all
		;;
	env)
		rm -f "$ENV_SCRIPT"

		idf="$(detect_idf)"
		[ -n "$idf" ] || die "no ESP-IDF found."

		check_idf_version "$idf"
		generate_env "$idf"
		;;
	--help|-h|help)
		usage
		;;
	*)
		idf_run "$@"
		;;
esac

say "finished."
