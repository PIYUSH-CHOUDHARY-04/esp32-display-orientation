#!/usr/bin/env bash
##
# @file uninstall.sh
# @brief Reverse what install.sh did: stop the daemon, delete the service, delete the binaries.
#
# ### Commands
# @verbatim
#   ./uninstall.sh              remove the service, the binaries, and the generated environment
#   ./uninstall.sh --clean      the above, plus the build artifacts (client/build, mcu/build)
#   ./uninstall.sh --purge      the above, plus the stored Wi-Fi credentials
#   ./uninstall.sh --all        --clean and --purge together
#   ./uninstall.sh --help
# @endverbatim
#
# ### What is removed by default
# Everything @c install.sh actually created:
# - the service, whichever route install.sh used: the systemd USER unit (stopped, disabled, unit
#   file DELETED, and the symlink @c enable left in @c graphical-session.target.wants/ removed
#   with it), or the XDG autostart entry and its session wrapper;
# - @c ~/.local/bin/esp_daemon, and the session wrapper if the autostart route was used;
# - @c ~/.local/bin/ap_provision;
# - @c env/idf_env.sh, the generated ESP-IDF environment.
#
# ### What is NOT removed by default, and why
#
# @b Build @b artifacts (@c client/build, @c mcu/build). These were not @e installed anywhere --
# they are ordinary output sitting in the working tree, exactly as a bare @c make would have
# left them. Deleting them would force a full ESP-IDF rebuild on the next build, which is slow,
# and would surprise anyone who is removing the service in order to debug and rebuild. Use
# @c --clean, or the @c "./install.sh clean" that already exists for precisely this.
#
# @b Credentials (@c ~/.ap_prov). Removing them is irreversible: the user key that protects the
# file is never written to disk by design, so a deleted credential file cannot be recovered by
# anyone, including the person who created it. Reinstalling would mean re-entering the SSID and
# password from scratch. That is a decision for the user to make deliberately, not a side effect
# of removing a binary. Use @c --purge.
#
# @note Nothing here needs root, because @c install.sh never used it: the binaries live under
#       @c ~/.local/bin, and the service is either a systemd USER unit or an XDG autostart entry --
#       both per-user, both inside @c $HOME. A SYSTEM service someone wrote by hand is a different
#       matter, and is reported rather than removed; see ::report_other_services.
##

set -euo pipefail

##
# @name Paths -- must mirror install.sh exactly
# @{
##
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"	##< Repository root.
CLIENT_DIR="$ROOT/client"				##< PC-side build output lives under here.
MCU_DIR="$ROOT/mcu"					##< Firmware build output lives under here.
BIN_DIR="$HOME/.local/bin"				##< Where install.sh put the binaries.
SYSTEMD_USER_DIR="$HOME/.config/systemd/user"		##< Where install.sh put the user unit.
ENV_SCRIPT="$ROOT/env/idf_env.sh"			##< The generated, machine-specific environment.
CRED_DIR="$HOME/.ap_prov"				##< Encrypted Wi-Fi credentials. Only removed with --purge.
DAEMON_NAME="esp_daemon"				##< The daemon: built, installed and serviced under one name.
PROV_NAME="ap_provision"				##< The interactive provisioning utility.
WRAPPER_NAME="esp_daemon-session"			##< Session wrapper (autostart route only).
##@}

##
# @name Flags
# @{
##
DO_CLEAN=0	##< --clean: also remove build artifacts.
DO_PURGE=0	##< --purge: also remove the stored credentials. Irreversible.
##@}

##
# @brief Print a step banner.
# @param $* Message.
##
say() {
	echo "==> $*"
}

##
# @brief Print the usage text and exit.
##
usage() {
	cat <<-EOF
	usage: ./uninstall.sh [--clean] [--purge] [--all]

	  (no flags)   remove the service, the binaries, and the generated environment
	  --clean      the above, plus the build artifacts (client/build, mcu/build)
	  --purge      the above, plus the stored Wi-Fi credentials  -- IRREVERSIBLE
	  --all        --clean and --purge together
	  --help       this message

	--purge cannot be undone. The user key protecting the credentials is never written
	to disk, so once the encrypted file is gone there is nothing left to decrypt and no
	key to decrypt it with -- not even for the person who set them.
	EOF

	exit 0
}

##
# @brief Parse the command line into ::DO_CLEAN and ::DO_PURGE.
#
# An unknown flag is an ERROR, not something to ignore. Silently accepting "--porge" and then
# running a default uninstall would tell the user their credentials had been purged when they had
# not -- and they would only find out much later, when they still could not reinstall cleanly.
##
parse_args() {
	local a

	for a in "$@"; do
		case "$a" in
			--clean)	DO_CLEAN=1 ;;
			--purge)	DO_PURGE=1 ;;
			--all)		DO_CLEAN=1; DO_PURGE=1 ;;
			--help|-h)	usage ;;
			*)
				echo "uninstall.sh: unknown option: $a" >&2
				echo "try: ./uninstall.sh --help" >&2
				exit 1
				;;
		esac
	done
}

##
# @brief Identify the init system by inspecting PID 1. Mirrors install.sh exactly.
# @return @c systemd, or @c other.
##
detect_init() {
	local pid1=""

	if [ -r /proc/1/comm ]; then
		pid1="$(cat /proc/1/comm 2>/dev/null || true)"
	fi

	if [ "$pid1" = "systemd" ]; then
		echo "systemd"
	else
		echo "other"
	fi
}

##
# @brief Stop the daemon and DELETE whichever service route was installed.
#
# ### Why no state file is needed
# install.sh recorded nothing about what it installed -- deliberately. A manifest of "what I did"
# would be a second source of truth: it can be tampered with, deleted, or go stale when the repo
# moves, and then the uninstaller is stranded with no fallback.
#
# The filesystem cannot go stale. So both routes are simply checked, and whichever files are
# actually there are removed. Checking a route that was never installed costs nothing, because every
# deletion is guarded -- and it means a user who switched init systems, or ran the installer twice,
# still ends up clean.
#
# Every step is best-effort. The service may already be stopped, may never have been enabled, or the
# files may already be gone by hand. None of that should abort the run: the goal is "it is not there
# afterwards", not "every command returned zero".
##
remove_service() {
	say "[service] init system: $(detect_init)"

	remove_systemd_route
	remove_autostart_route
}

##
# @brief Stop, disable, and delete the systemd user unit.
#
# The order matters. Deregistering AFTER deleting the unit file leaves systemd unable to find what
# it is being asked to forget, and the symlink 'enable' planted in graphical-session.target.wants/
# survives -- so systemd complains about a dangling reference on every subsequent daemon-reload.
##
remove_systemd_route() {
	local unit="$HOME/.config/systemd/user/$DAEMON_NAME.service"

	##
	# Both possible symlink homes are checked. The unit now installs into default.target.wants/,
	# but an older install put it under graphical-session.target.wants/ -- and a stale symlink there
	# makes systemd complain about a dangling reference on every daemon-reload, long after the unit
	# it points at has gone.
	##
	local wants_default="$HOME/.config/systemd/user/default.target.wants/$DAEMON_NAME.service"
	local wants_legacy="$HOME/.config/systemd/user/graphical-session.target.wants/$DAEMON_NAME.service"

	if ! command -v systemctl >/dev/null 2>&1; then
		return 0
	fi

	if [ ! -f "$unit" ] && [ ! -L "$wants_default" ] && [ ! -L "$wants_legacy" ]; then
		return 0
	fi

	systemctl --user stop    "$DAEMON_NAME.service" 2>/dev/null || true
	systemctl --user disable "$DAEMON_NAME.service" 2>/dev/null || true
	say "[service] stopped and disabled the systemd unit."

	local w
	for w in "$wants_default" "$wants_legacy"; do
		if [ -L "$w" ] || [ -e "$w" ]; then
			rm -f "$w"
			say "[service] removed the autostart symlink: $w"
		fi
	done

	if [ -f "$unit" ]; then
		rm -f "$unit"
		say "[service] DELETED $unit"
	fi

	systemctl --user daemon-reload 2>/dev/null || true
	systemctl --user reset-failed "$DAEMON_NAME.service" 2>/dev/null || true

	say "[service] systemd no longer knows about $DAEMON_NAME."
}

##
# @brief Kill the running wrapper and delete the XDG autostart entry.
#
# The wrapper is killed BEFORE the daemon. Killing the daemon first would simply make the wrapper's
# restart loop spawn a fresh one two seconds later -- the loop is doing exactly what it was built to
# do, and cannot tell an uninstall from a crash.
#
# Deleting the .desktop file is the whole of "disabling" it: the desktop session re-reads
# ~/.config/autostart at every login, so there is no registry to update.
##
remove_autostart_route() {
	local entry="$HOME/.config/autostart/$DAEMON_NAME.desktop"
	local wrapper="$BIN_DIR/$WRAPPER_NAME"

	if pgrep -f "$WRAPPER_NAME" >/dev/null 2>&1; then
		pkill -f "$WRAPPER_NAME" 2>/dev/null || true
		say "[service] stopped the running session wrapper."
	fi

	if pgrep -x "$DAEMON_NAME" >/dev/null 2>&1; then
		pkill -x "$DAEMON_NAME" 2>/dev/null || true
		say "[service] stopped the running daemon."
	fi

	if [ -f "$entry" ]; then
		rm -f "$entry"
		say "[service] DELETED $entry"
	fi

	if [ -f "$wrapper" ]; then
		rm -f "$wrapper"
		say "[service] DELETED $wrapper"
	fi
}

##
# @brief Report -- but do not remove -- a hand-installed system service.
#
# This project never installs one: both routes are per-user and neither needs root (see
# service/README.md for why -- a root service at boot has no X cookie and cannot rotate anything).
#
# But somebody may have written their own anyway, and if they have, an uninstall that silently left
# a root-owned service running would be worse than useless: the binaries would be gone, the service
# would keep failing to start, and nothing would say why.
#
# So the usual paths are checked and PRINTED. They are not deleted: this script did not create them,
# they are owned by root, and removing a service file without deregistering it from the init system
# leaves the machine in a worse state than before. That is the user's call, with their own tooling.
##
report_other_services() {
	local found=0
	local p

	for p in \
		"/etc/systemd/system/$DAEMON_NAME.service" \
		"/etc/init.d/$DAEMON_NAME" \
		"/etc/sv/$DAEMON_NAME" \
		"/var/service/$DAEMON_NAME"
	do
		if [ -e "$p" ]; then
			if [ "$found" -eq 0 ]; then
				say "[service] ------------------------------------------------------------"
				say "[service] A hand-installed SYSTEM service was found:"
				found=1
			fi

			say "[service]     $p"
		fi
	done

	if [ "$found" -eq 1 ]; then
		say "[service]"
		say "[service] This project did not install that -- it only ever installs per-user"
		say "[service] services, and never uses root. So it will not remove it either: the"
		say "[service] file is root-owned, and deleting it without deregistering it from your"
		say "[service] init system would leave things in a worse state than they are now."
		say "[service]"
		say "[service] Remove it with your init system's own tooling, e.g."
		say "[service]     sudo systemctl disable --now $DAEMON_NAME"
		say "[service]     sudo rm /etc/systemd/system/$DAEMON_NAME.service"
		say "[service] ------------------------------------------------------------"
	fi
}

##
# @brief Delete the installed binaries.
##
remove_binaries() {
	local b

	for b in "$DAEMON_NAME" "$PROV_NAME"; do
		if [ -f "$BIN_DIR/$b" ]; then
			rm -f "$BIN_DIR/$b"
			say "[bin] deleted $BIN_DIR/$b"
		fi
	done
}

##
# @brief Delete the generated ESP-IDF environment.
#
# Safe to remove unconditionally: it is machine-specific, gitignored, holds nothing the user
# typed, and @c "./install.sh env" regenerates it in full.
##
remove_env() {
	if [ -f "$ENV_SCRIPT" ]; then
		rm -f "$ENV_SCRIPT"
		say "[env] deleted $ENV_SCRIPT"
	fi
}

##
# @brief Delete the build artifacts. Only under @c --clean.
#
# Not part of a default uninstall: see the file-level notes. Nothing here was ever installed --
# it is just output sitting in the working tree.
##
clean_builds() {
	if [ -d "$CLIENT_DIR/build" ]; then
		rm -rf "$CLIENT_DIR/build"
		say "[clean] deleted $CLIENT_DIR/build"
	fi

	if [ -d "$MCU_DIR/build" ]; then
		rm -rf "$MCU_DIR/build"
		say "[clean] deleted $MCU_DIR/build"
		say "[clean] note: the next firmware build will be a full rebuild, and slow."
	fi
}

##
# @brief Delete the encrypted Wi-Fi credentials. Only under @c --purge.
#
# @warning Irreversible, and not merely inconvenient. The user key that protects this file is
#          never written to disk -- that is the entire point of the design -- so once the
#          ciphertext is gone there is nothing left to decrypt and no key to decrypt it with.
#          Not even the person who set the credentials can get them back.
##
purge_credentials() {
	if [ ! -d "$CRED_DIR" ]; then
		return 0
	fi

	say "[purge] deleting the stored Wi-Fi credentials in $CRED_DIR ..."
	rm -rf "$CRED_DIR"
	say "[purge] deleted. if the project is reinstalled, the SSID and password"
	say "[purge] will have to be entered again with:  ap_provision --set_creds"
}

parse_args "$@"

say "uninstalling ..."

remove_service
report_other_services
remove_binaries
remove_env

if [ "$DO_CLEAN" -eq 1 ]; then
	clean_builds
else
	if [ -d "$CLIENT_DIR/build" ] || [ -d "$MCU_DIR/build" ]; then
		say "[keep] build artifacts were left in place."
		say "[keep] remove them with:  ./uninstall.sh --clean   (or ./install.sh clean)"
	fi
fi

if [ "$DO_PURGE" -eq 1 ]; then
	purge_credentials
else
	if [ -d "$CRED_DIR" ]; then
		say "[keep] the stored Wi-Fi credentials in $CRED_DIR were left alone."
		say "[keep] remove them with:  ./uninstall.sh --purge"
		say "[keep] (this cannot be undone -- the key protecting them is not stored)"
	fi
fi

say "finished."
